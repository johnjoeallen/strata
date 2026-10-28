#ifndef RT_SYNC_H
#define RT_SYNC_H
// runtime/rt_sync.h - C counterpart of runtime/sync.stra and of the parking sync/mutate lock (design doc 7.1). NEW runtime support; the rest of the runtime is
// c/runtime/{rt,rt_buf,rt_io,rt_net}.h, unchanged and included by server.c.
//
//   * cross-worker wake: any thread asks worker W to wake task T (or a mailbox's consumer). The request goes on W's lock-free stack and an eventfd that W's
//     epoll already watches; W's wake task drains it. A wake on the worker's own thread skips all of that. A wake is only ever a hint: every park point in
//     the application re-checks its condition on resume, so a spurious or duplicate wake is harmless.
//   * Mailbox: bounded queue, many producers on any worker, ONE consumer task. A full queue refuses the message and counts it (nothing is dropped silently).
//   * Bell: an auto-reset signal (a permit) for one waiting task.
//   * RwLock: the parking lock. FIFO, readers batch, writers exclusive; unlock hands the lock to the next waiters and wakes them. Waiting parks the task.
//   * Limiter: counting limit that refuses instead of waiting.
#include "rt.h"
#include <sys/eventfd.h>
#include <sys/uio.h>

#define W_SIG 16                             // parked waiting for a cross-worker wake (lock grant, mailbox, bell, shutdown); combines with W_RD, W_TIMER
#define MAXW 64
#define SPIN_LOCK(l) do { while (__atomic_exchange_n(&(l), 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&(l), __ATOMIC_RELAXED)) __builtin_ia32_pause(); } while (0)
#define SPIN_UNLOCK(l) __atomic_store_n(&(l), 0, __ATOMIC_RELEASE)

// ---------------------------------------------------------------- cross-worker wake
struct Mailbox;
typedef struct WakeReq { struct WakeReq *next; Task *t; struct Mailbox *mb; int kind; } WakeReq;   // kind 0: wake t. 1: shutdown. 2: wake mb's consumer
typedef struct { int efd; WakeReq *head; Task *task; } WakeQ;     // task: the worker's wake task, alive for the whole process
static WakeQ g_wq[MAXW];
static __thread Worker *tl_w;                // the worker whose thread this is (NULL on other threads)
static void (*rt_on_shutdown)(Worker *w);    // the application wakes what must notice shutdown

static void wakeq_push(Worker *w, WakeReq *r) {
    WakeQ *q = &g_wq[w->id];
    int fd = __atomic_load_n(&q->efd, __ATOMIC_ACQUIRE);
    if (fd <= 0) { free(r); return; }                    // the worker has not finished starting: it has no tasks to wake yet
    WakeReq *old = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    do { r->next = old; } while (!__atomic_compare_exchange_n(&q->head, &old, r, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    if (!old) { u64 one = 1; if (write(fd, &one, 8) < 0) { /* counter full or closing: a later push writes again */ } }
}
// Wake task `t` (parked on worker w) from any thread. `t` must stay allocated until the wake is handled: lock waiters and the purge task are.
static void wake_task(Worker *w, Task *t) {
    if (w == tl_w) { if (t->parked) wake(w, t); return; }
    WakeReq *r = malloc(sizeof *r); r->t = t; r->mb = NULL; r->kind = 0;
    wakeq_push(w, r);
}
static void wake_shutdown(Worker *w) { WakeReq *r = malloc(sizeof *r); r->t = NULL; r->mb = NULL; r->kind = 1; wakeq_push(w, r); }
#define LIVE_ADD(w, d) __atomic_store_n(&(w)->live, (w)->live + (d), __ATOMIC_RELAXED)   /* single writer (its worker); main only reads */

// ---------------------------------------------------------------- Mailbox
typedef struct { void *a, *b; } Msg;
static void (*g_msg_drop)(Msg);              // releases what a queued message holds (set by the application)
typedef struct Mailbox {
    u32 refs;                                // atomic: the consumer, each topic it is in, the reaper queue, pending wakes
    u8 lk;                                   // spinlock for the ring
    u32 head, count, cap, alloc;             // cap: the declared limit. alloc: slots allocated so far (starts small, doubles up to cap)
    u32 lost, armed;                         // atomic
    Msg *ring;
    Task *owner;                             // the consumer task: read and written ONLY on its own worker's thread (cleared when the task ends)
    Worker *ow;                              // its worker: immutable, readable anywhere
} Mailbox;

static Mailbox *mb_new(u32 cap, Task *owner, Worker *ow) {
    Mailbox *m = calloc(1, sizeof *m);
    m->refs = 1; m->cap = cap; m->alloc = cap < 8 ? cap : 8; m->ring = malloc(m->alloc * sizeof(Msg)); m->owner = owner; m->ow = ow;
    return m;
}
static Mailbox *mb_ref(Mailbox *m) { __atomic_add_fetch(&m->refs, 1, __ATOMIC_RELAXED); return m; }
static void mb_unref(Mailbox *m) {
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    for (u32 i = 0; i < m->count; i++) g_msg_drop(m->ring[(m->head + i) % m->alloc]);
    free(m->ring); free(m);
}
// Any thread. Never parks. 0: queue full, message NOT queued and the lost count goes up.
static int mb_push(Mailbox *m, Msg x) {
    SPIN_LOCK(m->lk);
    if (m->count == m->cap) { SPIN_UNLOCK(m->lk); __atomic_add_fetch(&m->lost, 1, __ATOMIC_RELAXED); return 0; }
    if (m->count == m->alloc) {                                          // grow toward the declared capacity: memory follows use, the limit stays the limit
        u32 na = m->alloc * 2 > m->cap ? m->cap : m->alloc * 2;
        Msg *nr = malloc(na * sizeof(Msg));
        for (u32 i = 0; i < m->count; i++) nr[i] = m->ring[(m->head + i) % m->alloc];
        free(m->ring); m->ring = nr; m->head = 0; m->alloc = na;
    }
    m->ring[(m->head + m->count) % m->alloc] = x; m->count++;
    SPIN_UNLOCK(m->lk);
#ifdef BREAK_NOWAKE                                          /* negative control: a push never wakes the consumer */
    if (0) {
#else
    if (__atomic_exchange_n(&m->armed, 0, __ATOMIC_SEQ_CST)) {           // the consumer is (about to be) parked on this mailbox
#endif
        if (m->ow == tl_w) { Task *t = m->owner; if (t && t->parked) wake(m->ow, t); }
        else { WakeReq *r = malloc(sizeof *r); r->t = NULL; r->mb = mb_ref(m); r->kind = 2; wakeq_push(m->ow, r); }
    }
    return 1;
}
static int mb_pop(Mailbox *m, Msg *x) {                                  // consumer only
    SPIN_LOCK(m->lk);
    if (!m->count) { SPIN_UNLOCK(m->lk); return 0; }
    *x = m->ring[m->head]; m->head = (m->head + 1) % m->alloc; m->count--;
    SPIN_UNLOCK(m->lk);
    return 1;
}
static u32 mb_take_lost(Mailbox *m) { return __atomic_exchange_n(&m->lost, 0, __ATOMIC_RELAXED); }
// Consumer, just before parking: returns 1 if a message is already there (do not park); otherwise the next push will wake the consumer.
static int mb_arm(Mailbox *m) {
    __atomic_store_n(&m->armed, 1, __ATOMIC_SEQ_CST);
    SPIN_LOCK(m->lk); u32 n = m->count; SPIN_UNLOCK(m->lk);
    if (n) { __atomic_store_n(&m->armed, 0, __ATOMIC_SEQ_CST); return 1; }
    return 0;
}

// ---------------------------------------------------------------- Bell (auto-reset signal for one waiting task)
typedef struct { u32 pending, armed; Task *t; Worker *w; } Bell;
static void bell_ring(Bell *b) {
    __atomic_store_n(&b->pending, 1, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&b->armed, 0, __ATOMIC_SEQ_CST)) wake_task(b->w, b->t);
}
static void bell_take(Bell *b) { __atomic_store_n(&b->pending, 0, __ATOMIC_SEQ_CST); }
static int bell_arm(Bell *b) {               // 1: a permit is waiting, do not park
    __atomic_store_n(&b->armed, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&b->pending, __ATOMIC_SEQ_CST)) { __atomic_store_n(&b->armed, 0, __ATOMIC_SEQ_CST); return 1; }
    return 0;
}

// ---------------------------------------------------------------- RwLock: entering parks
typedef struct LockWait { struct LockWait *next; Task *t; u8 write; u32 granted; } LockWait;
typedef struct { u8 sp; u32 readers; u8 writer; LockWait *qh, *qt; u64 contended; } RwLock;
// 1: held now. 0: queued; the task parks (W_SIG) and, when resumed, checks lw->granted (the lock is then already its own).
static int rw_acquire(RwLock *l, LockWait *lw, Task *t, int write) {
// Before parking, spin for a bounded time while the holder (which is running, and cannot park inside the block) finishes. The contract is unchanged
// (entering may park); only the cost differs: see results/miniredis.md. LOCK_SPIN rounds of 8 pause instructions; -DLOCK_SPIN=0 parks at once.
#ifndef LOCK_SPIN
#define LOCK_SPIN 2000
#endif
#if LOCK_SPIN > 0
    for (int spin = 0;; spin++) {
        SPIN_LOCK(l->sp);
        if (!l->qh && !l->writer && (!write || l->readers == 0)) { if (write) l->writer = 1; else l->readers++; SPIN_UNLOCK(l->sp); return 1; }
        if (l->qh || spin >= LOCK_SPIN) break;           /* a queue exists (fairness: do not overtake it) or the budget is spent: park, still holding l->sp */
        SPIN_UNLOCK(l->sp);
        for (int k = 0; k < 8; k++) __builtin_ia32_pause();
    }
#else
    SPIN_LOCK(l->sp);
    if (!l->qh && !l->writer && (!write || l->readers == 0)) {
        if (write) l->writer = 1; else l->readers++;
        SPIN_UNLOCK(l->sp);
        return 1;
    }
#endif
    lw->t = t; lw->write = (u8)write; lw->next = NULL; lw->granted = 0;
    if (l->qt) l->qt->next = lw; else l->qh = lw;
    l->qt = lw; l->contended++;
    SPIN_UNLOCK(l->sp);
    return 0;
}
static void rw_release(RwLock *l, int write) {
    SPIN_LOCK(l->sp);
    if (write) l->writer = 0; else l->readers--;
    while (l->qh) {
        LockWait *h = l->qh;
        if (h->write) { if (l->readers || l->writer) break; l->writer = 1; }
        else { if (l->writer) break; l->readers++; }
        l->qh = h->next; if (!l->qh) l->qt = NULL;
        Task *t = h->t; Worker *w = t->w;                                // read before granting: the waiter may run at once and reuse h
        __atomic_store_n(&h->granted, 1, __ATOMIC_RELEASE);
        wake_task(w, t);
        if (h->write) break;
    }
    SPIN_UNLOCK(l->sp);
}

// ---------------------------------------------------------------- Limiter, shutdown flag
typedef struct { u32 live, limit; } Limiter;
static int lim_acquire(Limiter *l) {
    u32 v = __atomic_load_n(&l->live, __ATOMIC_RELAXED);
    do { if (v >= l->limit) return 0; } while (!__atomic_compare_exchange_n(&l->live, &v, v + 1, 1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
    return 1;
}
static void lim_release(Limiter *l) { __atomic_sub_fetch(&l->live, 1, __ATOMIC_RELEASE); }
static u32 g_shutdown;                       // sticky; set by the signal handler
#define SHUTTING() __atomic_load_n(&g_shutdown, __ATOMIC_RELAXED)

// ---------------------------------------------------------------- the per-worker wake task
static int wakeq_resume(Task *t) {
    Worker *w = t->w;
    WakeQ *q = &g_wq[w->id];
    u64 v; if (read(t->fd, &v, 8) < 0) { /* EAGAIN: already drained */ }
    WakeReq *r = __atomic_exchange_n(&q->head, NULL, __ATOMIC_ACQUIRE), *rev = NULL;
    while (r) { WakeReq *n = r->next; r->next = rev; rev = r; r = n; }   // oldest first
    while (rev) {
        WakeReq *n = rev->next;
        if (rev->kind == 0) { if (rev->t->parked) wake(w, rev->t); }
        else if (rev->kind == 1) { if (rt_on_shutdown) rt_on_shutdown(w); }
        else { Task *o = rev->mb->owner; if (o && o->parked) wake(w, o); mb_unref(rev->mb); }
        free(rev);
        rev = n;
    }
    return park(t, W_RD);
}
// Per-worker setup: call from rt_worker_init (it runs on the worker's own thread, after its epoll exists).
static void sync_worker_init(Worker *w) {
    tl_w = w;
    WakeQ *q = &g_wq[w->id];
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    Task *wt = q->task = calloc(1, sizeof *wt);
    wt->resume = wakeq_resume; wt->w = w; wt->fd = fd;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = wt };
    epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &ev);
    enqueue(w, wt);
    __atomic_store_n(&q->efd, fd, __ATOMIC_RELEASE);     // published last: wake requests are dropped until now
}
#endif
