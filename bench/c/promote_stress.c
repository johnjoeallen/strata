// Concurrent weak-promotion stress: promote(handle) racing free, slot reuse and chunk decommit.
//
//   mode=A  slot word = {full generation incl. chunk epoch : 32 | strong : 32}; promote = one CAS on that word.
//   mode=B  slot word holds only the in-slot counter; the chunk epoch is read separately before the CAS
//           (the design from the single-threaded tests). Expected to admit ABA under decommit races.
//
// Writers publish (handle, seq) pairs into a shared table (seqlock) and later drop them; readers promote
// handles (live and stale) and check that the promoted object's payload seq matches the published seq.
// A mismatch is an ABA violation; a payload change while a strong ref is held is a use-after-free.
//
// usage: promote_stress mode=A|B seconds=5 writers=4 readers=8 sgbits=6 entries=2048
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

#define SLOT 64
#define CH_SLOTS 64                    // one 4 KB page per chunk: decommits are frequent
#define NCHUNKS 256
#define CAP (CH_SLOTS * NCHUNKS)
#define NONE (~0ULL)

static int g_B, g_delay;           // g_delay: spin iterations injected inside the promote race window
static inline void race_delay(void) { for (int i = 0; i < g_delay; i++) __builtin_ia32_pause(); }
static u32 g_sgbits, g_smask, g_emask;
static char *region;

typedef struct { u32 live, bump, free_head; _Atomic u32 epoch; u32 frozen; } Chunk;
static Chunk chunks[NCHUNKS];
static pthread_mutex_t M = PTHREAD_MUTEX_INITIALIZER;
static u64 n_decommits, n_retired;

static inline _Atomic u64 *state_of(u32 idx) { return (_Atomic u64 *)(region + (size_t)idx * SLOT); }
static inline u64 *seq_of(u32 idx) { return (u64 *)(region + (size_t)idx * SLOT + 8); }
static inline u32 *next_of(u32 idx) { return (u32 *)(region + (size_t)idx * SLOT + 16); }

static u64 rng_next(u64 *s) {
    u64 x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x;
}

static u64 alloc_obj(u64 seq) {
    pthread_mutex_lock(&M);
    u32 c;
    for (c = 0; c < NCHUNKS; c++) if (chunks[c].free_head || chunks[c].bump < CH_SLOTS) break;
    if (c == NCHUNKS) { pthread_mutex_unlock(&M); return NONE; }
    Chunk *ch = &chunks[c];
    u32 idx;
    if (ch->free_head) { idx = ch->free_head - 1; ch->free_head = *next_of(idx); }
    else idx = c * CH_SLOTS + ch->bump++;
    ch->live++;
    u32 ep = atomic_load_explicit(&ch->epoch, memory_order_relaxed);
    pthread_mutex_unlock(&M);

    u64 old = atomic_load_explicit(state_of(idx), memory_order_relaxed);
    u32 sgen = (u32)(old >> 32) & g_smask;                  // 0 after a decommit zeroed the slot
    u32 hgen = (ep << g_sgbits) | sgen;
    u32 sgen_slot = g_B ? sgen : hgen;
    *seq_of(idx) = seq;
    atomic_store_explicit(state_of(idx), ((u64)sgen_slot << 32) | 1, memory_order_release);
    return (u64)idx | ((u64)hgen << 32);
}

static void free_slot(u32 idx, int retire) {
    pthread_mutex_lock(&M);
    Chunk *ch = &chunks[idx / CH_SLOTS];
    if (retire) n_retired++;
    else { *next_of(idx) = ch->free_head; ch->free_head = idx + 1; }
    ch->live--;
    if (ch->live == 0 && !ch->frozen) {
        madvise(region + (size_t)(idx / CH_SLOTS) * CH_SLOTS * SLOT, CH_SLOTS * SLOT, MADV_DONTNEED);
        ch->bump = 0; ch->free_head = 0;
        u32 e = atomic_load_explicit(&ch->epoch, memory_order_relaxed) + 1;
        atomic_store_explicit(&ch->epoch, e, memory_order_release);
        if (e >= g_emask) ch->frozen = 1;
        n_decommits++;
    }
    pthread_mutex_unlock(&M);
}

static void release_obj(u32 idx) {
    _Atomic u64 *st = state_of(idx);
    u64 s = atomic_load_explicit(st, memory_order_acquire);
    for (;;) {
        u32 strong = (u32)s, gen = (u32)(s >> 32);
        if (strong > 1) {
            if (atomic_compare_exchange_weak_explicit(st, &s, s - 1, memory_order_acq_rel, memory_order_acquire)) return;
            continue;
        }
        u32 low = gen & g_smask;
        int retire = low >= g_smask - 1;
        u32 ng = retire ? (gen | g_smask) : gen + 1;        // bump generation as the count reaches zero
        if (atomic_compare_exchange_weak_explicit(st, &s, (u64)ng << 32, memory_order_acq_rel, memory_order_acquire)) {
            free_slot(idx, retire);
            return;
        }
    }
}

static int promote(u64 h) {
    u32 idx = (u32)h, hgen = (u32)(h >> 32);
    if (idx >= CAP) return 0;
    u32 want = hgen;
    if (g_B) {
        u32 ep = atomic_load_explicit(&chunks[idx / CH_SLOTS].epoch, memory_order_acquire);
        if ((hgen >> g_sgbits) != ep) return 0;              // epoch checked separately, before the CAS
        want = hgen & g_smask;
        race_delay();                                        // window: epoch checked, CAS not yet done
    }
    _Atomic u64 *st = state_of(idx);
    u64 s = atomic_load_explicit(st, memory_order_acquire);
    if (!g_B) race_delay();                                   // same window for mode A: word read, CAS not yet done
    for (;;) {
        if ((u32)(s >> 32) != want || (u32)s == 0) return 0;
        if (atomic_compare_exchange_weak_explicit(st, &s, s + 1, memory_order_acq_rel, memory_order_acquire)) return 1;
    }
}

// ---- shared table with a seqlock per entry (single writer per entry) ----
typedef struct { _Atomic u32 ver; u64 h, seq; } Entry;
static Entry *table;
static int g_entries, g_writers, g_readers;
static _Atomic int stop;

static _Atomic u64 viol_aba, viol_uaf, n_ok, n_rej, n_stale_ok;

static void publish(Entry *e, u64 h, u64 seq) {
    u32 v = atomic_load_explicit(&e->ver, memory_order_relaxed);
    atomic_store_explicit(&e->ver, v + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    e->h = h; e->seq = seq;
    atomic_store_explicit(&e->ver, v + 2, memory_order_release);
}
static int snapshot(Entry *e, u64 *h, u64 *seq) {
    u32 v1 = atomic_load_explicit(&e->ver, memory_order_acquire);
    if (v1 & 1) return 0;
    *h = e->h; *seq = e->seq;
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&e->ver, memory_order_relaxed) == v1;
}

static void *writer(void *arg) {
    int w = (int)(long)arg;
    u64 rng = 0x9E3779B97F4A7C15ULL * (w + 1), ctr = 0;
    while (!atomic_load(&stop)) {
        for (int j = w; j < g_entries; j += g_writers) {      // fill phase
            u64 seq = ((u64)(w + 1) << 48) | ++ctr;
            u64 h = alloc_obj(seq);
            if (h == NONE) continue;
            u64 oh = table[j].h;
            publish(&table[j], h, seq);
            if (oh != NONE) release_obj((u32)oh);
        }
        for (int j = w; j < g_entries; j += g_writers) {      // drain phase: chunks empty, decommit fires
            u64 oh = table[j].h;
            publish(&table[j], NONE, 0);
            if (oh != NONE) release_obj((u32)oh);
        }
        (void)rng_next(&rng);
    }
    return NULL;
}

static void *reader(void *arg) {
    u64 rng = 0xD1B54A32D192ED03ULL * ((long)arg + 7);
    u64 hist_h[256], hist_s[256]; int nh = 0, ph = 0;
    u64 ok = 0, rej = 0, stale_ok = 0, aba = 0, uaf = 0;
    while (!atomic_load(&stop)) {
        for (int it = 0; it < 256; it++) {
            u64 h, seq;
            if ((rng_next(&rng) & 3) == 0 && nh > 0) {        // stale probe from history
                int k = rng_next(&rng) % nh; h = hist_h[k]; seq = hist_s[k];
            } else {
                if (!snapshot(&table[rng_next(&rng) % g_entries], &h, &seq) || h == NONE) continue;
                hist_h[ph % 256] = h; hist_s[ph % 256] = seq; ph++; if (nh < 256) nh++;
            }
            if (promote(h)) {
                ok++;
                u32 idx = (u32)h;
                if (*(volatile u64 *)seq_of(idx) != seq) aba++;   // different object behind this handle
                for (int s = 0; s < 30; s++) {                 // hold the ref: payload must stay put
                    if (*(volatile u64 *)seq_of(idx) != seq) { uaf++; break; }
                    __builtin_ia32_pause();
                }
                release_obj(idx);
            } else rej++;
        }
    }
    atomic_fetch_add(&n_ok, ok); atomic_fetch_add(&n_rej, rej); atomic_fetch_add(&n_stale_ok, stale_ok);
    atomic_fetch_add(&viol_aba, aba); atomic_fetch_add(&viol_uaf, uaf);
    return NULL;
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

int main(int argc, char **argv) {
    g_B = !strcmp(arg(argc, argv, "mode", "A"), "B");
    int secs = atoi(arg(argc, argv, "seconds", "5"));
    g_writers = atoi(arg(argc, argv, "writers", "4"));
    g_readers = atoi(arg(argc, argv, "readers", "8"));
    g_sgbits = atoi(arg(argc, argv, "sgbits", "6"));
    g_entries = atoi(arg(argc, argv, "entries", "2048"));
    g_delay = atoi(arg(argc, argv, "delay", "0"));
    g_smask = (1u << g_sgbits) - 1;
    g_emask = (g_sgbits >= 32 ? 0 : (1u << (32 - g_sgbits)) - 1);
    region = mmap(NULL, (size_t)CAP * SLOT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    madvise(region, (size_t)CAP * SLOT, MADV_NOHUGEPAGE);
    table = calloc(g_entries, sizeof(Entry));
    for (int i = 0; i < g_entries; i++) table[i].h = NONE;

    pthread_t th[64]; int n = 0;
    for (long i = 0; i < g_writers; i++) pthread_create(&th[n++], NULL, writer, (void *)i);
    for (long i = 0; i < g_readers; i++) pthread_create(&th[n++], NULL, reader, (void *)i);
    struct timespec ts = {secs, 0}; nanosleep(&ts, NULL);
    atomic_store(&stop, 1);
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);

    printf("mode=%s delay=%d sgbits=%u writers=%d readers=%d %ds: promotions ok=%lu rejected=%lu | decommits=%lu retired=%lu | "
           "ABA violations=%lu  use-after-free=%lu  => %s\n",
           g_B ? "B (separate epoch check)" : "A (epoch in slot word)", g_delay, g_sgbits, g_writers, g_readers, secs,
           (unsigned long)n_ok, (unsigned long)n_rej, n_decommits, n_retired,
           (unsigned long)viol_aba, (unsigned long)viol_uaf, (viol_aba || viol_uaf) ? "FAIL" : "pass");
    return (viol_aba || viol_uaf) ? 1 : 0;
}
