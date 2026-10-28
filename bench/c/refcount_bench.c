// Group 6: reference-count cost under sharing. One iteration = inc then dec on one object.
//
//   impl=plain    non-atomic (only meaningful when objects are thread-private)
//   impl=atomic   fetch_add / fetch_sub on one count
//   impl=biased   owner thread: plain local count; other threads: atomic shared count (fast paths only; the
//                 merge/queue protocol that runs when the owner's count reaches zero is NOT measured)
//   scn=own       each thread uses only its own object
//   scn=hot       every thread uses the same object (owner = thread 0)
//   scn=mix       each thread uses its own object, but with probability share picks another thread's object
//
// usage: refcount_bench impl=biased scn=mix share=0.1 threads=8 seconds=2
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
#define MAXT 64

typedef struct {
    u32 owner;
    u32 local;                   // owner-only
    _Atomic int shared;          // everyone else
    _Atomic int total;           // used by impl=atomic
    u32 plain;                   // used by impl=plain
    char pad[64 - 20];
} __attribute__((aligned(64))) Obj;

static Obj objs[MAXT];
enum { I_PLAIN, I_ATOMIC, I_BIASED };
enum { S_OWN, S_HOT, S_MIX };
static int g_impl, g_scn, g_threads, g_secs;
static double g_share;
static _Atomic int stop_flag, ready;
static _Atomic u64 total_iters;

static inline u64 rng_next(u64 *s) { u64 x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

static void *worker(void *arg) {
    u32 t = (u32)(long)arg;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(t, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    u64 rng = 0x9E3779B97F4A7C15ULL * (t + 1), iters = 0;
    u64 thresh = (u64)(g_share * 4294967296.0);
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) < g_threads) ;
    while (!atomic_load_explicit(&stop_flag, memory_order_relaxed)) {
        for (int k = 0; k < 256; k++) {
            Obj *o;
            if (g_scn == S_HOT) o = &objs[0];
            else if (g_scn == S_MIX && g_threads > 1 && (rng_next(&rng) >> 32) < thresh) o = &objs[rng_next(&rng) % g_threads];
            else o = &objs[t];
            switch (g_impl) {
            case I_PLAIN:
                *(volatile u32 *)&o->plain += 1;
                *(volatile u32 *)&o->plain -= 1;
                break;
            case I_ATOMIC:
                atomic_fetch_add_explicit(&o->total, 1, memory_order_relaxed);
                atomic_fetch_sub_explicit(&o->total, 1, memory_order_acq_rel);
                break;
            default:
                if (o->owner == t) {
                    *(volatile u32 *)&o->local += 1;
                    *(volatile u32 *)&o->local -= 1;
                } else {
                    atomic_fetch_add_explicit(&o->shared, 1, memory_order_relaxed);
                    atomic_fetch_sub_explicit(&o->shared, 1, memory_order_acq_rel);
                }
            }
        }
        iters += 256;
    }
    atomic_fetch_add(&total_iters, iters);
    return NULL;
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

int main(int argc, char **argv) {
    const char *is = arg(argc, argv, "impl", "atomic"), *ss = arg(argc, argv, "scn", "own");
    g_impl = !strcmp(is, "plain") ? I_PLAIN : !strcmp(is, "biased") ? I_BIASED : I_ATOMIC;
    g_scn = !strcmp(ss, "hot") ? S_HOT : !strcmp(ss, "mix") ? S_MIX : S_OWN;
    g_threads = atoi(arg(argc, argv, "threads", "4"));
    g_share = atof(arg(argc, argv, "share", "0.1"));
    g_secs = atoi(arg(argc, argv, "seconds", "2"));
    for (int i = 0; i < MAXT; i++) objs[i].owner = i;
    pthread_t th[MAXT];
    for (long i = 0; i < g_threads; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    struct timespec ts = {g_secs, 0}; nanosleep(&ts, NULL);
    atomic_store(&stop_flag, 1);
    for (int i = 0; i < g_threads; i++) pthread_join(th[i], NULL);
    double mp = total_iters / 1e6 / g_secs;
    printf("impl=%-6s scn=%-3s share=%.2f threads=%2d  %9.1f Mpairs/s total  %7.2f ns/pair/thread\n",
           is, ss, g_scn == S_MIX ? g_share : 0.0, g_threads, mp, g_threads * 1e3 / mp);
    return 0;
}
