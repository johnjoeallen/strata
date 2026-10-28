// Biased reference counting with the full lifecycle (owner merge, queued merge requests), vs plain atomic counts.
//
// Object: owner-only `local` count + atomic `shared` word = (count << 2) | flags.   flags: Q=1 (queued), M=2 (merged)
//   incref: owner -> local++ ; anyone else (or owner after merge) -> atomic add
//   decref owner: local--; at 0 merge: publish M (keeping Q), owner = NONE; free if count==0 and not queued
//   decref other: atomic; if count goes negative (the owner's local refs paid for this release) and not M/Q, set Q and
//                 push the object on its home thread's merge queue; if M and not Q and count==0, free
//   queue drain (home thread): total = count + local; publish M, clear Q; free if total == 0
// Message passing: the sender increfs once per receiver; each receiver decrefs once (so receivers hit the negative
// path and queue merges). With check=1 objects are never really freed, so use-after-free and double free are detected.
//
// usage: brc mode=biased|atomic threads=8 fan=2 hops=2 priv=8 objs=200000 check=1       (stress: fixed object count)
//        brc mode=biased|atomic threads=8 fan=2 priv=8 seconds=2                         (bench: timed, real free)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

typedef uint64_t u64;
typedef uint32_t u32;
#define NONE 0xFFFFFFFFu
#define MAXT 32
#define RING 4096
#define Q 1L
#define M 2L

typedef struct Obj {
    _Atomic u32 owner;           // creating thread while its local count is live, NONE after merge
    u32 local;                   // owner-only
    _Atomic long shared;
    struct Obj *_Atomic qnext;
    _Atomic u32 state;           // check mode: 0 alive, 1 freed
    u32 home;                    // immutable creating thread
    char pad[28];
} __attribute__((aligned(64))) Obj;

static int g_plain, g_biased, g_threads, g_fan, g_hops, g_priv, g_check, g_secs, g_pub;
static struct { pthread_mutex_t m; struct Obj *o; char pad[40]; } cell[32];   // published object per thread
static long g_objs;

// ---- counters ----
typedef struct { u64 allocs, frees, dfree, uaf, sent, produced, queued, qmerge_alive, self_alive; char pad[16]; } Stats;
static Stats st[MAXT];
static __thread u32 tid;
static _Atomic long msgs_pending, queue_pending, producers_done;
static _Atomic int stop_flag, ready;

// ---- merge queue (lock-free stack per home thread) ----
static struct { Obj *_Atomic head; char pad[56]; } mq[MAXT];
static void queue_push(Obj *o) {
#ifdef BUG_NOQUEUE
    return;                      // negative control: the merge request is dropped, so the object leaks
#endif
    atomic_fetch_add(&queue_pending, 1);
    st[tid].queued++;
    Obj *h = atomic_load_explicit(&mq[o->home].head, memory_order_relaxed);
    do { atomic_store_explicit(&o->qnext, h, memory_order_relaxed); }
    while (!atomic_compare_exchange_weak_explicit(&mq[o->home].head, &h, o, memory_order_release, memory_order_relaxed));
}

static void free_obj(Obj *o) {
    Stats *s = &st[tid];
    s->frees++;
    if (g_check) { if (atomic_exchange(&o->state, 1) == 1) s->dfree++; }
    else free(o);
}
static inline void check_alive(Obj *o) { if (g_check && atomic_load_explicit(&o->state, memory_order_relaxed)) st[tid].uaf++; }

static Obj *obj_new(void) {
    Obj *o = aligned_alloc(64, sizeof(Obj));
    memset(o, 0, sizeof *o);
    atomic_store(&o->owner, g_biased ? tid : NONE);
    o->local = (g_biased || g_plain) ? 1 : 0;
    atomic_store(&o->shared, g_biased ? 0 : (1L << 2));     // atomic mode: shared holds the whole count
    o->home = tid;
    st[tid].allocs++;
    return o;
}

static inline void incref(Obj *o) {
    if (g_plain) { o->local++; return; }
    check_alive(o);
    if (g_biased && atomic_load_explicit(&o->owner, memory_order_relaxed) == tid) o->local++;
    else atomic_fetch_add_explicit(&o->shared, 4, memory_order_relaxed);
}

static void merge_self(Obj *o) {
    atomic_store_explicit(&o->owner, NONE, memory_order_relaxed);
    long s = atomic_load_explicit(&o->shared, memory_order_acquire), ns;
    do { ns = (s & Q) | M | (s & ~3L); }
    while (!atomic_compare_exchange_weak_explicit(&o->shared, &s, ns, memory_order_acq_rel, memory_order_acquire));
    if (!(ns & Q) && (ns >> 2) == 0) free_obj(o);
    else if ((ns >> 2) > 0) st[tid].self_alive++;      // owner merged while others still hold references
}

static inline void decref(Obj *o) {
    if (g_plain) { if (--o->local == 0) free_obj(o); return; }
    check_alive(o);
    if (!g_biased) {
        if (atomic_fetch_sub_explicit(&o->shared, 4, memory_order_acq_rel) == 4) free_obj(o);
        return;
    }
    if (atomic_load_explicit(&o->owner, memory_order_relaxed) == tid) {
        if (--o->local == 0) merge_self(o);
        return;
    }
    long s = atomic_load_explicit(&o->shared, memory_order_acquire);
    for (;;) {
        long ns = s - 4; int push = 0;
        if (!(ns & (Q | M)) && (ns >> 2) < 0) { ns |= Q; push = 1; }
        if (atomic_compare_exchange_weak_explicit(&o->shared, &s, ns, memory_order_acq_rel, memory_order_acquire)) {
            if (push) queue_push(o);
#ifdef BUG_EARLY_FREE
            else if ((ns >> 2) == 0) free_obj(o);          // negative control: treat count 0 as dead even if not merged
#else
            else if ((ns & M) && !(ns & Q) && (ns >> 2) == 0) free_obj(o);
#endif
            return;
        }
    }
}

static void queue_drain(void) {
    Obj *p = atomic_exchange_explicit(&mq[tid].head, NULL, memory_order_acquire);
    while (p) {
        Obj *next = atomic_load_explicit(&p->qnext, memory_order_relaxed);
        u32 loc = atomic_load_explicit(&p->owner, memory_order_relaxed) == tid ? p->local : 0;
        atomic_store_explicit(&p->owner, NONE, memory_order_relaxed);
        long s = atomic_load_explicit(&p->shared, memory_order_acquire), ns, total;
        do { total = (s >> 2) + loc; ns = (total << 2) | M; }
        while (!atomic_compare_exchange_weak_explicit(&p->shared, &s, ns, memory_order_acq_rel, memory_order_acquire));
        if (total == 0) free_obj(p); else st[tid].qmerge_alive++;
        atomic_fetch_sub(&queue_pending, 1);
        p = next;
    }
}

// ---- inbox: bounded MPSC ring (Vyukov), message = object pointer | hops ----
typedef struct { _Atomic u64 seq; void *data; } Cell;
typedef struct { Cell cells[RING]; _Atomic u64 enq; char p1[56]; u64 deq; char p2[56]; } Ring;
static Ring *ring[MAXT];

static int ring_push(Ring *r, void *p) {
    u64 pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
    for (;;) {
        Cell *c = &r->cells[pos & (RING - 1)];
        long dif = (long)atomic_load_explicit(&c->seq, memory_order_acquire) - (long)pos;
        if (dif == 0) { if (atomic_compare_exchange_weak_explicit(&r->enq, &pos, pos + 1, memory_order_relaxed, memory_order_relaxed)) { c->data = p; atomic_store_explicit(&c->seq, pos + 1, memory_order_release); return 1; } }
        else if (dif < 0) return 0;
        else pos = atomic_load_explicit(&r->enq, memory_order_relaxed);
    }
}
static void *ring_pop(Ring *r) {
    Cell *c = &r->cells[r->deq & (RING - 1)];
    if (atomic_load_explicit(&c->seq, memory_order_acquire) != r->deq + 1) return NULL;
    void *p = c->data;
    atomic_store_explicit(&c->seq, r->deq + RING, memory_order_release);
    r->deq++;
    return p;
}

static u64 rng_state_next(u64 *s) { u64 x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }
static void process_inbox(int allow_forward, u64 *rng);

static void send(Obj *o, u32 hops, u64 *rng) {
    u32 dst = g_threads > 1 ? (tid + 1 + rng_state_next(rng) % (g_threads - 1)) % g_threads : tid;
    atomic_fetch_add(&msgs_pending, 1);
    st[tid].sent++;
    while (!ring_push(ring[dst], (void *)((uintptr_t)o | hops))) { process_inbox(0, rng); queue_drain(); }
}

static void process_inbox(int allow_forward, u64 *rng) {
    void *m;
    while ((m = ring_pop(ring[tid]))) {
        Obj *o = (Obj *)((uintptr_t)m & ~(uintptr_t)63);
        u32 hops = (uintptr_t)m & 63;
        if (allow_forward && hops > 0) { incref(o); send(o, hops - 1, rng); }
        decref(o);                                    // the reference the sender made for us
        atomic_fetch_sub(&msgs_pending, 1);
    }
}

static void *worker(void *arg) {
    tid = (u32)(long)arg;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(tid, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    u64 rng = 0x9E3779B97F4A7C15ULL * (tid + 1);
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) < g_threads) ;
    long n = 0;
    while (g_secs ? !atomic_load_explicit(&stop_flag, memory_order_relaxed) : n < g_objs) {
        Obj *o = obj_new();
        for (int i = 0; i < g_priv; i++) { incref(o); decref(o); }       // local copies / borrows
        for (int f = 0; f < g_fan; f++) { incref(o); send(o, g_hops, &rng); }
        if (g_pub) {
            // publish: the cell takes over the creator's reference; readers take refs under the lock
            pthread_mutex_lock(&cell[tid].m); Obj *old = cell[tid].o; cell[tid].o = o; pthread_mutex_unlock(&cell[tid].m);
            if (old) decref(old);                                         // owner may merge while readers still hold refs
            for (int r = 0; r < g_pub; r++) {
                u32 u = rng_state_next(&rng) % g_threads;
                pthread_mutex_lock(&cell[u].m); Obj *o2 = cell[u].o; if (o2) incref(o2); pthread_mutex_unlock(&cell[u].m);
                if (o2) { for (int k = 0; k < 20; k++) __builtin_ia32_pause(); decref(o2); }
            }
        } else decref(o);                                                 // creator drops its reference
        process_inbox(1, &rng);
        queue_drain();
        n++;
    }
    st[tid].produced = n;
    atomic_fetch_add(&producers_done, 1);
    if (g_pub) {                                                          // all readers are done once everyone finished producing
        while (atomic_load(&producers_done) < g_threads) { process_inbox(1, &rng); queue_drain(); }
        Obj *last = cell[tid].o; cell[tid].o = NULL;
        if (last) decref(last);
    }
    while (!(atomic_load(&producers_done) == g_threads && atomic_load(&msgs_pending) == 0 && atomic_load(&queue_pending) == 0)) {
        process_inbox(1, &rng);
        queue_drain();
    }
    return NULL;
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

int main(int argc, char **argv) {
    const char *ms = arg(argc, argv, "mode", "biased");
    g_plain = !strcmp(ms, "plain");
    g_biased = !strcmp(ms, "biased");
    if (g_plain && (atoi(arg(argc, argv, "fan", "2")) || atoi(arg(argc, argv, "pub", "0")))) { fprintf(stderr, "plain counts are only valid for thread-private objects (fan=0 pub=0)\n"); return 2; }
    g_threads = atoi(arg(argc, argv, "threads", "8"));
    g_fan = atoi(arg(argc, argv, "fan", "2"));
    g_hops = atoi(arg(argc, argv, "hops", "0"));
    g_priv = atoi(arg(argc, argv, "priv", "8"));
    g_objs = atol(arg(argc, argv, "objs", "200000"));
    g_check = atoi(arg(argc, argv, "check", "0"));
    g_secs = atoi(arg(argc, argv, "seconds", "0"));
    g_pub = atoi(arg(argc, argv, "pub", "0"));
    for (int i = 0; i < 32; i++) pthread_mutex_init(&cell[i].m, NULL);
    for (int i = 0; i < g_threads; i++) {
        ring[i] = aligned_alloc(64, sizeof(Ring));
        memset(ring[i], 0, sizeof(Ring));
        for (u64 k = 0; k < RING; k++) atomic_store(&ring[i]->cells[k].seq, k);
    }
    pthread_t th[MAXT];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (long i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    if (g_secs) { struct timespec ts = {g_secs, 0}; nanosleep(&ts, NULL); atomic_store(&stop_flag, 1); }
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;

    u64 allocs = 0, frees = 0, dfree = 0, uaf = 0, sent = 0, produced = 0, queued = 0, qalive = 0, salive = 0;
    for (int i = 0; i < g_threads; i++) { allocs += st[i].allocs; frees += st[i].frees; dfree += st[i].dfree; uaf += st[i].uaf; sent += st[i].sent; produced += st[i].produced; queued += st[i].queued; qalive += st[i].qmerge_alive; salive += st[i].self_alive; }
    int bad = (allocs != frees) || dfree || uaf;
    printf("mode=%-6s threads=%2d fan=%d hops=%d priv=%d pub=%d  objects=%lu msgs=%lu  %.2f Mobj/s  allocs=%lu frees=%lu leaked=%ld double_free=%lu uaf=%lu | queued=%lu queue-merge-with-live-refs=%lu owner-merge-with-live-refs=%lu%s\n",
           g_plain ? "plain" : g_biased ? "biased" : "atomic", g_threads, g_fan, g_hops, g_priv, g_pub, (unsigned long)produced, (unsigned long)sent,
           produced / el / 1e6, (unsigned long)allocs, (unsigned long)frees, (long)allocs - (long)frees,
           (unsigned long)dfree, (unsigned long)uaf, (unsigned long)queued, (unsigned long)qalive, (unsigned long)salive, g_check ? (bad ? "  => FAIL" : "  => pass") : "");
    return g_check && bad;
}
