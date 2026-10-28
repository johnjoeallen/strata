// Strata C runtime, as it would exist next to compiler output (hand-written stand-in).
//
// Model: one worker per core, each with its own epoll instance, run queue and per-type arenas.
// A task is a compiler-sized frame plus a small header; it either finishes (DONE) or suspends (PARKED).
// Readiness is edge-triggered: a task always tries the syscall first and parks only on EAGAIN, so an
// edge that arrives while the task is running is never lost (events are consumed only between task runs).
#ifndef STRATA_RT_H
#define STRATA_RT_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

enum { DONE = 0, PARKED = 1 };
enum { W_NONE = 0, W_RD = 1, W_WR = 2, W_CHILD = 4, W_TIMER = 8 };      // a task may wait on several at once

#define CNT(x) __atomic_store_n(&(x), (x) + 1, __ATOMIC_RELAXED)
#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

typedef struct Task Task;
typedef struct Worker Worker;
typedef struct { u64 when; Task *t; u32 *idxp; } TimerEnt;     /* idxp: where the owner keeps this entry's heap index (NULL: not cancellable) */
#define TIMER_NONE 0xFFFFFFFFu
#define NCLS 4                              /* input buffer size classes (see server.c) */

struct Task {
    int (*resume)(Task *);          // compiler-emitted entry for this task's root frame
    Task *next;                     // run queue link
    Worker *w;
    int fd;
    u8 parked, queued, wait, rd_ready;
};

// ---- per-type arena: geometric chunks 4 KB -> 64 KB, LIFO free list, never returned to the OS (v1) ----
typedef struct {
    size_t sz;
    void *freel;
    char *bump, *end;
    size_t chunk;
    u64 live, chunks, carved;
} Pool;

static void pool_init(Pool *p, size_t sz) {
    memset(p, 0, sizeof *p);
    p->sz = (sz + 7) & ~(size_t)7;
    p->chunk = 4096;
}
static inline void *pool_alloc(Pool *p) {
    p->live++;
    if (p->freel) { void *o = p->freel; p->freel = *(void **)o; return o; }
    if (p->bump + p->sz > p->end) {
        size_t c = p->chunk;
        if (c < p->sz) c = (p->sz + 4095) & ~(size_t)4095;      // a chunk must hold at least one object
        char *m = mmap(NULL, c, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (m == MAP_FAILED) { perror("mmap chunk"); exit(1); }
        madvise(m, c, MADV_NOHUGEPAGE);
        p->bump = m; p->end = m + c;
        p->chunks++; p->carved += c;
        if (p->chunk < 65536) p->chunk *= 2;
    }
    void *o = p->bump;
    p->bump += p->sz;
    return o;
}
static inline void pool_free(Pool *p, void *o) {
    p->live--;
    *(void **)o = p->freel;
    p->freel = o;
}

// ---- worker ----
struct Worker {
    int id, ep;
    Task *qh, *qt;
    Pool tasks, bufs, reqs, rings, heads, bodies, lingers, cls[NCLS];
    TimerEnt *th; u32 tn, tcap;                 // per-worker timer min-heap (sleeping tasks)
    u64 nacc, nreq, nclose, live;
    char pad[64];
};

static inline void enqueue(Worker *w, Task *t) {
    if (t->queued) return;
    t->queued = 1; t->next = NULL;
    if (w->qt) w->qt->next = t; else w->qh = t;
    w->qt = t;
}
static inline void wake(Worker *w, Task *t) { t->parked = 0; enqueue(w, t); }
static inline int park(Task *t, int kind) { t->parked = 1; t->wait = (u8)kind; return PARKED; }
static inline int yield_now(Task *t) { t->parked = 0; enqueue(t->w, t); return PARKED; }

static inline u64 mono_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec;
}
static inline void th_set(Worker *w, u32 i, TimerEnt e) { w->th[i] = e; if (e.idxp) *e.idxp = i; }
static void th_up(Worker *w, u32 i, TimerEnt e) {
    while (i > 0) {
        u32 p = (i - 1) / 2;
        if (w->th[p].when <= e.when) break;
        th_set(w, i, w->th[p]); i = p;
    }
    th_set(w, i, e);
}
static void th_down(Worker *w, u32 i, TimerEnt e) {
    for (;;) {
        u32 c = 2 * i + 1;
        if (c >= w->tn) break;
        if (c + 1 < w->tn && w->th[c + 1].when < w->th[c].when) c++;
        if (w->th[c].when >= e.when) break;
        th_set(w, i, w->th[c]); i = c;
    }
    th_set(w, i, e);
}
// Wake `t` at `when` (ns); the task parks with W_TIMER. If `idxp` is given, the heap keeps *idxp equal to the entry's index (TIMER_NONE once it
// has fired or been cancelled), which is what makes timer_cancel possible: a task that finishes early must not leave an entry that could later
// wake a recycled task slot.
static void timer_add_c(Worker *w, Task *t, u64 when, u32 *idxp) {
    if (w->tn == w->tcap) {
        w->tcap = w->tcap ? w->tcap * 2 : 64;
        w->th = realloc(w->th, w->tcap * sizeof(TimerEnt));
    }
    TimerEnt e = { when, t, idxp };
    u32 i = w->tn++;
    th_up(w, i, e);
}
static void timer_add(Worker *w, Task *t, u64 when) { timer_add_c(w, t, when, NULL); }
static void timer_cancel(Worker *w, u32 *idxp) {
    u32 i = *idxp;
    if (i == TIMER_NONE || i >= w->tn) { *idxp = TIMER_NONE; return; }
    *idxp = TIMER_NONE;
    TimerEnt last = w->th[--w->tn];
    if (i == w->tn) return;                               // it was the last entry
    if (i > 0 && w->th[(i - 1) / 2].when > last.when) th_up(w, i, last); else th_down(w, i, last);
}
static Task *timer_pop_due(Worker *w, u64 now) {
    if (!w->tn || w->th[0].when > now) return NULL;
    Task *t = w->th[0].t;
    if (w->th[0].idxp) *w->th[0].idxp = TIMER_NONE;
    TimerEnt last = w->th[--w->tn];
    if (w->tn) th_down(w, 0, last);
    return t;
}

static void worker_loop(Worker *w) {
    struct epoll_event evs[512];
    for (;;) {
        struct timespec to, *tp = NULL;
        if (w->qh) { to.tv_sec = 0; to.tv_nsec = 0; tp = &to; }
        else if (w->tn) {
            u64 now = mono_ns(), d = w->th[0].when > now ? w->th[0].when - now : 0;
            to.tv_sec = d / 1000000000ull; to.tv_nsec = d % 1000000000ull; tp = &to;
        }
        int n = epoll_pwait2(w->ep, evs, 512, tp, NULL);
        if (w->tn) {
            u64 now = mono_ns();
            Task *tt;
            while ((tt = timer_pop_due(w, now))) wake(w, tt);
        }
        for (int i = 0; i < n; i++) {
            Task *t = evs[i].data.ptr;
            u32 e = evs[i].events;
            if (e & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                t->rd_ready = 1;
                if (t->parked && (t->wait & W_RD)) wake(w, t);
            }
            if ((e & (EPOLLOUT | EPOLLERR | EPOLLHUP)) && t->parked && (t->wait & W_WR)) wake(w, t);
        }
        Task *t = w->qh;
        w->qh = w->qt = NULL;                       // tasks enqueued while running go to the next round
        while (t) {
            Task *nx = t->next;
            t->queued = 0; t->parked = 0;
            t->resume(t);                           // may free t; do not touch it afterwards
            t = nx;
        }
    }
}

static inline double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r"); char l[256]; long v = -1;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { v = atol(l + 6); break; }
    if (f) fclose(f);
    return v;
}
#endif
