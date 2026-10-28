// Group 5: allocation under cross-thread frees.
// Each thread allocates batches of 64 B objects; a fraction r are handed to the next thread (which frees them),
// the rest are freed locally. Every thread also drains its inbox. Throughput = total alloc+free pairs per second.
//
//   mode=owned    per-thread arenas; chunk header names its owner; remote frees go on the owner's lock-free stack
//   mode=mutex    one global free list behind a pthread mutex
//   mode=lfstack  one global lock-free (tagged) Treiber stack + atomic bump
//   mode=glibc    malloc/free
//
// usage: conc_alloc mode=owned threads=8 remote=0.5 seconds=3
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>

typedef uint64_t u64;
typedef uint32_t u32;

#define OBJ 64
#define CHUNK (64u * 1024)
#define MAXT 64
#define INBOX_CAP 8192

enum { M_OWNED, M_MUTEX, M_LF, M_GLIBC, M_OWNED_B };
#define REMOTE_BATCH 32
static int g_mode, g_threads;
static double g_remote;
static int g_secs;

static char *region;                    // shared reserved range, carved into chunks / objects
static size_t region_bytes;

// ---- owned ----
typedef struct Arena Arena;
typedef struct { Arena *owner; } ChunkHdr;
struct Arena {
    void *local;                        // thread-private free list (linked through first word)
    char *bump, *bump_end;
    _Atomic(void *) remote;             // MPSC stack of objects freed by other threads
    int id;
    void *out_head[64], *out_tail[64]; int out_cnt[64];   // owned_b: outbound remote frees, batched per owner
    char pad[64];
};
static Arena arenas[MAXT];
static _Atomic u64 next_chunk;
static __thread Arena *me;

static void arena_new_chunk(Arena *a) {
    u64 c = atomic_fetch_add(&next_chunk, 1);
    char *base = region + c * CHUNK;
    if ((size_t)(c + 1) * CHUNK > region_bytes) { fprintf(stderr, "out of region\n"); exit(1); }
    ((ChunkHdr *)base)->owner = a;
    a->bump = base + 64; a->bump_end = base + CHUNK;
}
static inline void *owned_alloc(void) {
    Arena *a = me;
    void *p = a->local;
    if (p) { a->local = *(void **)p; return p; }
    if (atomic_load_explicit(&a->remote, memory_order_relaxed)) {
        p = atomic_exchange_explicit(&a->remote, NULL, memory_order_acquire);
        a->local = *(void **)p;
        return p;
    }
    if (a->bump + OBJ > a->bump_end) arena_new_chunk(a);
    p = a->bump; a->bump += OBJ;
    return p;
}
static inline void owned_free(void *p) {
    Arena *owner = ((ChunkHdr *)((uintptr_t)p & ~(uintptr_t)(CHUNK - 1)))->owner;
    if (owner == me) { *(void **)p = me->local; me->local = p; return; }
    if (g_mode == M_OWNED_B) {                                 // batch per destination, one CAS per batch
        int o = owner->id;
        *(void **)p = me->out_head[o];
        if (!me->out_head[o]) me->out_tail[o] = p;
        me->out_head[o] = p;
        if (++me->out_cnt[o] < REMOTE_BATCH) return;
        void *h = atomic_load_explicit(&owner->remote, memory_order_relaxed);
        do { *(void **)me->out_tail[o] = h; } while (!atomic_compare_exchange_weak_explicit(&owner->remote, &h, me->out_head[o], memory_order_release, memory_order_relaxed));
        me->out_head[o] = me->out_tail[o] = NULL; me->out_cnt[o] = 0;
        return;
    }
    void *h = atomic_load_explicit(&owner->remote, memory_order_relaxed);
    do { *(void **)p = h; } while (!atomic_compare_exchange_weak_explicit(&owner->remote, &h, p, memory_order_release, memory_order_relaxed));
}

// ---- mutex ----
static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static void *g_free; static u64 g_bump;
static inline void *mutex_alloc(void) {
    pthread_mutex_lock(&gm);
    void *p = g_free;
    if (p) g_free = *(void **)p; else p = region + (g_bump++) * OBJ;
    pthread_mutex_unlock(&gm);
    return p;
}
static inline void mutex_free(void *p) {
    pthread_mutex_lock(&gm);
    *(void **)p = g_free; g_free = p;
    pthread_mutex_unlock(&gm);
}

// ---- lock-free stack (tagged index) ----
static _Atomic u64 lf_head;             // low 32: index+1 (0 = empty), high 32: tag
static _Atomic u64 lf_bump;
static inline void *lf_alloc(void) {
    u64 h = atomic_load_explicit(&lf_head, memory_order_acquire);
    while ((u32)h) {
        char *p = region + (u64)((u32)h - 1) * OBJ;
        u64 nxt = *(u64 *)p;                               // low 32 = next index+1
        u64 nh = ((h >> 32) + 1) << 32 | (u32)nxt;
        if (atomic_compare_exchange_weak_explicit(&lf_head, &h, nh, memory_order_acquire, memory_order_acquire)) return p;
    }
    return region + atomic_fetch_add(&lf_bump, 1) * OBJ;
}
static inline void lf_free(void *p) {
    u32 idx = (u32)(((char *)p - region) / OBJ) + 1;
    u64 h = atomic_load_explicit(&lf_head, memory_order_relaxed);
    do { *(u64 *)p = (u32)h; } while (!atomic_compare_exchange_weak_explicit(&lf_head, &h, (((h >> 32) + 1) << 32) | idx, memory_order_release, memory_order_relaxed));
}

static inline void *obj_alloc(void) {
    switch (g_mode) {
    case M_OWNED: case M_OWNED_B: return owned_alloc();
    case M_MUTEX: return mutex_alloc();
    case M_LF: return lf_alloc();
    default: return malloc(OBJ);
    }
}
static inline void obj_free(void *p) {
    switch (g_mode) {
    case M_OWNED: case M_OWNED_B: owned_free(p); break;
    case M_MUTEX: mutex_free(p); break;
    case M_LF: lf_free(p); break;
    default: free(p);
    }
}

// ---- inboxes ----
typedef struct { _Atomic(void *) head; _Atomic int count; char pad[56]; } Inbox;
static Inbox inbox[MAXT];
static _Atomic int stop_flag;
static _Atomic int ready;
static _Atomic u64 total_pairs;

static inline u64 rng_next(u64 *s) { u64 x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

static void drain(int t) {
    void *p = atomic_exchange_explicit(&inbox[t].head, NULL, memory_order_acquire);
    int n = 0;
    while (p) { void *nx = *(void **)((char *)p + 8); obj_free(p); p = nx; n++; }
    if (n) atomic_fetch_sub(&inbox[t].count, n);
}

static void *worker(void *arg) {
    int t = (int)(long)arg;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(t, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    me = &arenas[t]; me->id = t;
    u64 rng = 0x9E3779B97F4A7C15ULL * (t + 1), pairs = 0;
    u64 thresh = (u64)(g_remote * 4294967296.0);
    int dst = (t + 1) % g_threads;
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) < g_threads) ;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        void *batch[32];
        for (int i = 0; i < 32; i++) { batch[i] = obj_alloc(); *(u64 *)((char *)batch[i] + 16) = i; }
        int allow = g_threads > 1 && atomic_load_explicit(&inbox[dst].count, memory_order_relaxed) < INBOX_CAP;
        void *rh = NULL, *rt = NULL; int rn = 0;
        for (int i = 0; i < 32; i++) {
            void *p = batch[i];
            if (allow && ((rng_next(&rng) >> 32) < thresh)) {   // hand off: collect, push the whole chain once
                *(void **)((char *)p + 8) = rh;
                if (!rh) rt = p;
                rh = p; rn++;
            } else obj_free(p);
            pairs++;
        }
        if (rn) {
            void *h = atomic_load_explicit(&inbox[dst].head, memory_order_relaxed);
            do { *(void **)((char *)rt + 8) = h; } while (!atomic_compare_exchange_weak_explicit(&inbox[dst].head, &h, rh, memory_order_release, memory_order_relaxed));
            atomic_fetch_add_explicit(&inbox[dst].count, rn, memory_order_relaxed);
        }
        drain(t);
    }
    atomic_fetch_add(&total_pairs, pairs);
    return NULL;
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r"); char l[256]; long v = -1;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { v = atol(l + 6); break; }
    if (f) fclose(f);
    return v;
}

int main(int argc, char **argv) {
    const char *m = arg(argc, argv, "mode", "owned");
    g_mode = !strcmp(m, "owned_b") ? M_OWNED_B : !strcmp(m, "mutex") ? M_MUTEX : !strcmp(m, "lfstack") ? M_LF : !strcmp(m, "glibc") ? M_GLIBC : M_OWNED;
    g_threads = atoi(arg(argc, argv, "threads", "4"));
    g_remote = atof(arg(argc, argv, "remote", "0.5"));
    g_secs = atoi(arg(argc, argv, "seconds", "3"));
    region_bytes = (size_t)64 << 30;
    region = mmap(NULL, region_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    madvise(region, region_bytes, MADV_NOHUGEPAGE);
    // chunk-align the region for owner lookup
    region = (char *)(((uintptr_t)region + CHUNK - 1) & ~(uintptr_t)(CHUNK - 1));
    region_bytes -= CHUNK;

    pthread_t th[MAXT];
    for (long i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    struct timespec ts = {g_secs, 0}; nanosleep(&ts, NULL);
    atomic_store(&stop_flag, 1);
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);

    u64 objs_bump = (g_mode == M_OWNED || g_mode == M_OWNED_B) ? atomic_load(&next_chunk) * (CHUNK / OBJ) : g_mode == M_MUTEX ? g_bump : atomic_load(&lf_bump);
    printf("mode=%-7s threads=%2d remote=%.2f  %7.1f Mpairs/s (%5.1f per thread)  rss=%ld MB  objects_carved=%lu\n",
           m, g_threads, g_remote, total_pairs / 1e6 / g_secs, total_pairs / 1e6 / g_secs / g_threads,
           rss_kb() / 1024, (unsigned long)objs_bump);
    return 0;
}
