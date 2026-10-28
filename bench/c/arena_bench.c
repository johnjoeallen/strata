// Group 2: typed arena variants (layout, chunk sizing, free policy, contraction) + workloads.
//
// usage: arena_bench key=value ...
//   layout=reserve|chunked|malloc   reserve: one contiguous NORESERVE range, address = base + idx*slot
//                                   chunked: separate mmap per chunk, address via chunk table
//   first=<log2 slots>  cap=<log2 slots>   chunk 0 holds 2^first slots, doubling up to 2^cap (first==cap: fixed)
//   policy=recent|lowest            which chunk with a free slot to allocate from
//   contract=none|madvise|munmap    what to do with a chunk that becomes empty (munmap: chunked only)
//   spare=<k>                       empty chunks kept committed before decommitting
//   slot=<bytes>  n=<objects>  work=fill|churn|ramp|mixed|deref  pattern=random|fifo|group
//
// Slot layout: [u32 next_free+1][u32 generation][payload...]. Handles are u32 indices.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef uint16_t u16;

#define PAGE 4096UL
#define MAXCHUNKS (1u << 17)

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) { v = atol(line + 6); break; }
    if (f) fclose(f);
    return v;
}
static u64 rng_state = 88172645463325252ULL;
static inline u64 rnd(void) {
    u64 x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x;
}

enum { L_RESERVE, L_CHUNKED, L_MALLOC };
enum { P_RECENT, P_LOWEST };
enum { C_NONE, C_MADVISE, C_MUNMAP };

typedef struct {
    char *base;
    u32 nslots, start, live, bump, free_head;  // free_head: global idx + 1, 0 = none
    u8 committed, in_nonfull, empty_counted, frozen;
    u16 epoch;                // bumped on every decommit; resident, so it survives MADV_DONTNEED
} Chunk;

typedef struct {
    u32 slot, first, cap, g;
    int layout, policy, contract;
    u32 spare, empty_committed;
    char *region;
    Chunk *chunks;
    u32 nchunks;
    u32 *stack, nstack;       // recent: stack of chunks that may have space
    u64 *bitmap;              // lowest: bit per chunk with space
    u32 hint;                 // lowest: no set bit below this chunk
    u64 live, decommits, commits;
    u32 sgbits, smask, emask;     // generation = (chunk epoch << sgbits) | in-slot counter
    u64 retired, frozen_chunks;
    u32 mag[512], nmag, magcap;   // per-worker LIFO magazine of recently freed slots (hot reuse)
} Arena;

static inline u32 start_of(const Arena *a, u32 c) {
    u32 S0 = 1u << a->first;
    if (c <= a->g) return S0 * ((1u << c) - 1);
    return S0 * ((1u << a->g) - 1) + (c - a->g) * (1u << a->cap);
}
static inline u32 size_of_chunk(const Arena *a, u32 c) { return c < a->g ? (1u << a->first) << c : 1u << a->cap; }
static inline u32 chunk_of(const Arena *a, u32 idx) {
    u32 sg = (1u << a->first) * ((1u << a->g) - 1);
    if (idx < sg) return 31 - __builtin_clz((idx >> a->first) + 1);
    return a->g + ((idx - sg) >> a->cap);
}

static void arena_init(Arena *a, int layout, u32 first, u32 cap, int policy, int contract, u32 spare,
                       u32 slot, u64 max_slots) {
    memset(a, 0, sizeof *a);
    a->layout = layout; a->first = first; a->cap = cap; a->g = cap - first;
    a->policy = policy; a->contract = contract; a->spare = spare; a->slot = slot;
    a->sgbits = 20; a->smask = (1u << 20) - 1; a->emask = (1u << 12) - 1;
    a->chunks = calloc(MAXCHUNKS, sizeof(Chunk));
    a->stack = malloc(MAXCHUNKS * sizeof(u32));
    a->bitmap = calloc(MAXCHUNKS / 64, sizeof(u64));
    if (layout == L_RESERVE) {
        size_t bytes = (size_t)max_slots * slot;
        a->region = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (a->region == MAP_FAILED) { perror("mmap region"); exit(1); }
        madvise(a->region, bytes, MADV_NOHUGEPAGE);
    }
}

static inline char *arena_ptr(const Arena *a, u32 idx) {
    if (a->layout == L_RESERVE) return a->region + (size_t)idx * a->slot;
    u32 c = chunk_of(a, idx);
    return a->chunks[c].base + (size_t)(idx - a->chunks[c].start) * a->slot;
}

static void set_nonfull(Arena *a, u32 c) {
    Chunk *ch = &a->chunks[c];
    if (ch->in_nonfull) return;
    ch->in_nonfull = 1;
    if (a->policy == P_LOWEST) { a->bitmap[c >> 6] |= 1ULL << (c & 63); if (c < a->hint) a->hint = c; }
    else a->stack[a->nstack++] = c;
}

static void chunk_commit(Arena *a, u32 c) {
    Chunk *ch = &a->chunks[c];
    if (a->layout == L_CHUNKED && !ch->base) {
        size_t bytes = (size_t)ch->nslots * a->slot;
        bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
        ch->base = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (ch->base == MAP_FAILED) { perror("mmap chunk"); exit(1); }
        madvise(ch->base, bytes, MADV_NOHUGEPAGE);
        a->commits++;
    }
    ch->committed = 1;
}

static void chunk_decommit(Arena *a, u32 c) {
    Chunk *ch = &a->chunks[c];
    size_t bytes = (size_t)ch->nslots * a->slot;
    if (a->contract == C_MUNMAP && a->layout == L_CHUNKED) {
        munmap(ch->base, (bytes + PAGE - 1) & ~(PAGE - 1));
        ch->base = NULL;
    } else {
        uintptr_t s = ((uintptr_t)ch->base + PAGE - 1) & ~(PAGE - 1);
        uintptr_t e = ((uintptr_t)ch->base + bytes) & ~(PAGE - 1);
        if (e > s) madvise((void *)s, e - s, MADV_DONTNEED);
    }
    ch->committed = 0; ch->free_head = 0; ch->bump = 0;
    a->decommits++;
    ch->epoch++;                                            // every outstanding handle to this chunk is now stale
    if (ch->epoch >= a->emask) { ch->frozen = 1; a->frozen_chunks++; }   // epoch exhausted: never decommit again
}

static u32 new_chunk(Arena *a) {
    u32 c = a->nchunks++;
    if (c >= MAXCHUNKS) { fprintf(stderr, "too many chunks\n"); exit(1); }
    Chunk *ch = &a->chunks[c];
    ch->nslots = size_of_chunk(a, c);
    ch->start = start_of(a, c);
    if (a->layout == L_RESERVE) ch->base = a->region + (size_t)ch->start * a->slot;
    chunk_commit(a, c);
    set_nonfull(a, c);
    return c;
}

static int pick_chunk(Arena *a) {
    if (a->policy == P_LOWEST) {
        for (u32 w = a->hint >> 6; w < (a->nchunks + 63) / 64; w++) {
            u64 bits = a->bitmap[w];
            if (w == (a->hint >> 6)) bits &= ~0ULL << (a->hint & 63);
            if (bits) { a->hint = (w << 6) + __builtin_ctzll(bits); return a->hint; }
        }
        return -1;
    }
    while (a->nstack) {
        u32 c = a->stack[a->nstack - 1];
        Chunk *ch = &a->chunks[c];
        if (ch->free_head || ch->bump < ch->nslots) return c;
        ch->in_nonfull = 0; a->nstack--;
    }
    return -1;
}

static u32 arena_alloc_chunk(Arena *a) {
    int ci = pick_chunk(a);
    if (ci < 0) ci = new_chunk(a);
    Chunk *ch = &a->chunks[ci];
    if (!ch->committed) chunk_commit(a, ci);
    if (ch->empty_counted) { ch->empty_counted = 0; a->empty_committed--; }
    u32 idx;
    if (ch->free_head) {
        idx = ch->free_head - 1;
        char *p = a->layout == L_RESERVE ? a->region + (size_t)idx * a->slot
                                         : ch->base + (size_t)(idx - ch->start) * a->slot;
        ch->free_head = *(u32 *)p;
    } else {
        idx = ch->start + ch->bump++;
    }
    ch->live++; a->live++;
    if (!ch->free_head && ch->bump == ch->nslots && a->policy == P_LOWEST) {   // now full
        ch->in_nonfull = 0;
        a->bitmap[ci >> 6] &= ~(1ULL << (ci & 63));
    }                       // recent: a full chunk is popped lazily by pick_chunk
    return idx;
}

// Return a slot to its chunk. The generation was already bumped by arena_free. A retired slot (its in-slot
// counter saturated) is not put back on the free list: it stays unusable until the chunk is decommitted.
static void arena_release(Arena *a, u32 idx, int retired) {
    u32 c = chunk_of(a, idx);
    Chunk *ch = &a->chunks[c];
    if (!retired) {
        char *p = a->layout == L_RESERVE ? a->region + (size_t)idx * a->slot
                                         : ch->base + (size_t)(idx - ch->start) * a->slot;
        *(u32 *)p = ch->free_head;
        ch->free_head = idx + 1;
        set_nonfull(a, c);
    }
    ch->live--; a->live--;
    if (ch->live == 0) {
        ch->empty_counted = 1;
        a->empty_committed++;
        if (a->contract != C_NONE && a->empty_committed > a->spare && !ch->frozen) {
            chunk_decommit(a, c);
            ch->empty_counted = 0;
            a->empty_committed--;
        }
    }
}

static inline u32 arena_alloc(Arena *a) {
    if (a->nmag) return a->mag[--a->nmag];
    return arena_alloc_chunk(a);
}
static inline char *slot_ptr(const Arena *a, u32 idx) {
    if (a->layout == L_RESERVE) return a->region + (size_t)idx * a->slot;
    u32 c = chunk_of(a, idx);
    return a->chunks[c].base + (size_t)(idx - a->chunks[c].start) * a->slot;
}
static inline void arena_free(Arena *a, u32 idx) {
    u32 sg = ++((u32 *)slot_ptr(a, idx))[1];                // logical free: invalidates handles immediately
    if (sg >= a->smask) { a->retired++; arena_release(a, idx, 1); return; }
    if (!a->magcap) { arena_release(a, idx, 0); return; }
    if (a->nmag == a->magcap) {                              // flush the older half back to the chunks
        u32 half = a->magcap / 2;
        for (u32 i = 0; i < half; i++) arena_release(a, a->mag[i], 0);
        memmove(a->mag, a->mag + half, (a->nmag - half) * sizeof(u32));
        a->nmag -= half;
    }
    a->mag[a->nmag++] = idx;
}

// ---- generational handles: 64-bit = index | (chunk epoch << sgbits | slot counter) << 32 ----
static inline u64 arena_handle(const Arena *a, u32 idx) {
    u32 c = chunk_of(a, idx);
    u32 sg = ((u32 *)slot_ptr(a, idx))[1];
    return (u64)idx | ((u64)(((u32)a->chunks[c].epoch << a->sgbits) | sg) << 32);
}
static inline void *arena_resolve(const Arena *a, u64 h) {
    u32 idx = (u32)h, gen = (u32)(h >> 32);
    u32 c = chunk_of(a, idx);
    if (c >= a->nchunks) return NULL;
    const Chunk *ch = &a->chunks[c];
    if ((gen >> a->sgbits) != ch->epoch) return NULL;       // checked first: a decommitted chunk may be unmapped
    char *p = a->layout == L_RESERVE ? a->region + (size_t)idx * a->slot
                                     : ch->base + (size_t)(idx - ch->start) * a->slot;
    return ((u32 *)p)[1] == (gen & a->smask) ? p : NULL;
}
static void arena_set_sgbits(Arena *a, u32 bits) {
    a->sgbits = bits; a->smask = (1u << bits) - 1; a->emask = (1u << (32 - bits)) - 1;
    if (a->emask > 0xFFFF) a->emask = 0xFFFF;               // epoch is a u16
}

// ---- backend wrapper so workloads run unchanged on malloc ----
typedef struct { void *p; u32 id; } Obj;
static Arena A;
static int g_layout;
static u32 g_slot;
static long g_malloc_live;

static inline Obj obj_alloc(void) {
    Obj o;
    if (g_layout == L_MALLOC) { o.p = malloc(g_slot); o.id = 0; g_malloc_live++; }
    else { o.id = arena_alloc(&A); o.p = arena_ptr(&A, o.id); }
    return o;
}
static inline void obj_free(Obj o) {
    if (g_layout == L_MALLOC) { free(o.p); g_malloc_live--; }
    else arena_free(&A, o.id);
}
static inline void obj_touch(Obj o, u64 v) {
    char *p = o.p;
    *(u64 *)(p + 8) = v;
    for (u32 i = 64; i < g_slot; i += 64) p[i] = (char)v;
}
static inline u64 obj_read(Obj o) { return *(u64 *)((char *)o.p + 8); }

static void report(const char *tag, double rss_mb, double live_mb, const char *extra) {
    printf("  %-14s rss=%8.1f MB  live=%8.1f MB  ratio=%5.2f %s\n", tag, rss_mb, live_mb,
           live_mb > 0 ? rss_mb / live_mb : 0.0, extra);
}
static double mb(double bytes) { return bytes / (1024.0 * 1024.0); }
static double rss_delta_mb(long base) { return (rss_kb() - base) / 1024.0; }

// ---- workloads ----
static void shuffle(u32 *v, long n) {
    for (long i = n - 1; i > 0; i--) { long j = rnd() % (i + 1); u32 t = v[i]; v[i] = v[j]; v[j] = t; }
}

static void w_fill(long n) {
    Obj *o = malloc(n * sizeof *o);
    u32 *order = malloc(n * sizeof *order);
    memset(o, 1, n * sizeof *o); memset(order, 1, n * sizeof *order);   // harness arrays stay out of RSS deltas
    long base = rss_kb();
    double t0 = now();
    for (long i = 0; i < n; i++) { o[i] = obj_alloc(); obj_touch(o[i], i); }
    double t1 = now();
    double rss_full = rss_delta_mb(base);
    for (long i = 0; i < n; i++) order[i] = i;
    shuffle(order, n);
    long bad = 0;
    double t2 = now();
    for (long i = 0; i < n; i++) { Obj x = o[order[i]]; bad += obj_read(x) != order[i]; obj_free(x); }
    double t3 = now();
    printf("fill: alloc+touch %.1f ns/op, free (random order) %.1f ns/op, corrupt=%ld\n",
           (t1 - t0) * 1e9 / n, (t3 - t2) * 1e9 / n, bad);
    report("full", rss_full, mb((double)n * g_slot), "");
    report("after free all", rss_delta_mb(base), 0, "");
    free(o); free(order);
}

static void w_churn(long n) {
    Obj *o = malloc(n * sizeof *o);
    memset(o, 1, n * sizeof *o);
    long base = rss_kb();
    for (long i = 0; i < n; i++) { o[i] = obj_alloc(); obj_touch(o[i], i); }
    long ops = 4 * n, bad = 0;
    double t0 = now();
    for (long k = 0; k < ops; k++) {
        long i = rnd() % n;
        bad += obj_read(o[i]) != (u64)i;
        obj_free(o[i]);
        o[i] = obj_alloc();
        obj_touch(o[i], i);
    }
    double t1 = now();
    printf("churn: %.1f ns per free+alloc pair, corrupt=%ld\n", (t1 - t0) * 1e9 / ops, bad);
    report("after churn", rss_delta_mb(base), mb((double)n * g_slot), "");
    for (long i = 0; i < n; i++) obj_free(o[i]);
    free(o);
}

// ramp: fill n, free 90% in a given order, re-fill 50% more, then free everything
static void w_ramp(long n, const char *pattern) {
    Obj *o = malloc(2 * n * sizeof *o);
    char *alive = calloc(2 * n, 1);
    u32 *ord = malloc(n * sizeof *ord);
    memset(o, 1, 2 * n * sizeof *o); memset(alive, 1, 2 * n); memset(ord, 1, n * sizeof *ord);
    long base = rss_kb();
    for (long i = 0; i < n; i++) { o[i] = obj_alloc(); obj_touch(o[i], i); alive[i] = 1; }
    double rss_full = rss_delta_mb(base);
    long keep_every = 10, freed = 0;
    if (!strcmp(pattern, "random")) {
        for (long i = 0; i < n; i++) ord[i] = i;
        shuffle(ord, n);
        for (long k = 0; k < n * 9 / 10; k++) { obj_free(o[ord[k]]); alive[ord[k]] = 0; freed++; }
    } else if (!strcmp(pattern, "fifo")) {                    // oldest 90% die
        for (long i = 0; i < n * 9 / 10; i++) { obj_free(o[i]); alive[i] = 0; freed++; }
    } else {                                                  // group: runs of 100, 1 in 10 groups survive
        for (long i = 0; i < n; i++)
            if ((i / 100) % keep_every != 0) { obj_free(o[i]); alive[i] = 0; freed++; }
    }
    long live = n - freed;
    double rss_freed = rss_delta_mb(base);
    long m = n / 2;
    for (long i = 0; i < m; i++) { o[n + i] = obj_alloc(); obj_touch(o[n + i], n + i); alive[n + i] = 1; }
    double rss_refill = rss_delta_mb(base);
    long bad = 0;
    for (long i = 0; i < n + m; i++) if (alive[i]) { bad += obj_read(o[i]) != (u64)i; obj_free(o[i]); }
    printf("ramp(%s): live after free=%ld (%.0f%%), corrupt=%ld\n", pattern, live, 100.0 * live / n, bad);
    report("full", rss_full, mb((double)n * g_slot), "");
    report("after free 90%", rss_freed, mb((double)live * g_slot), "");
    report("after refill", rss_refill, mb((double)(live + m) * g_slot), "");
    report("after free all", rss_delta_mb(base), 0, "");
    free(o); free(alive); free(ord);
}

// mixed lifetimes: short-lived objects live exactly S steps (FIFO), 2% of allocations are immortal
static void w_mixed(long n) {
    long S = n, steps = 5 * n;
    Obj *ring = malloc(S * sizeof *ring);
    Obj *lng = malloc((steps / 40) * sizeof *lng);
    long nl = 0, head = 0, count = 0;
    memset(ring, 1, S * sizeof *ring); memset(lng, 1, (steps / 40) * sizeof *lng);
    long base = rss_kb();
    double t0 = now();
    for (long k = 0; k < steps; k++) {
        Obj x = obj_alloc();
        obj_touch(x, k);
        if (rnd() % 50 == 0 && nl < steps / 40) { lng[nl++] = x; continue; }
        if (count == S) { obj_free(ring[head]); ring[head] = x; head = (head + 1) % S; }
        else ring[(head + count++) % S] = x;
    }
    double t1 = now();
    double live_mb = mb((double)(count + nl) * g_slot);
    printf("mixed: %.1f ns/step, short live=%ld, immortal=%ld\n", (t1 - t0) * 1e9 / steps, count, nl);
    report("steady state", rss_delta_mb(base), live_mb, "");
    for (long i = 0; i < count; i++) obj_free(ring[(head + i) % S]);   // burst ends: short-lived all die
    report("after burst", rss_delta_mb(base), mb((double)nl * g_slot), "(immortals only live)");
    for (long i = 0; i < nl; i++) obj_free(lng[i]);
    free(ring); free(lng);
}


// compact: fill n, kill a random 30% (holes everywhere), then churn random replacements with 2% extra immortals.
// Under lowest-first allocation, live objects should migrate downward so high chunks empty and can be released.
static void w_compact(long n) {
    long m = n * 7 / 10, nsteps = 6 * n;
    Obj *sh = malloc(n * sizeof *sh), *lng = malloc((nsteps / 40) * sizeof *lng);
    u32 *ord = malloc(n * sizeof *ord);
    memset(sh, 1, n * sizeof *sh); memset(lng, 1, (nsteps / 40) * sizeof *lng); memset(ord, 1, n * sizeof *ord);
    Obj *keep = malloc(m * sizeof *keep);
    memset(keep, 1, m * sizeof *keep);                        // all harness arrays touched before the baseline
    long base = rss_kb(), nl = 0;
    for (long i = 0; i < n; i++) { sh[i] = obj_alloc(); obj_touch(sh[i], i); ord[i] = i; }
    shuffle(ord, n);
    for (long i = 0; i < n; i++) { if (i < m) keep[i] = sh[ord[i]]; else obj_free(sh[ord[i]]); }
    Obj *all = sh; sh = keep;
    printf("compact: n=%ld, live short=%ld after 30%% random kill\n", n, m);
    report("after kill", rss_delta_mb(base), mb((double)m * g_slot), "");
    double t0 = now();
    for (long k = 1; k <= nsteps; k++) {
        long i = rnd() % m;
        obj_free(sh[i]);
        sh[i] = obj_alloc(); obj_touch(sh[i], k);
        if (rnd() % 50 == 0 && nl < nsteps / 40) { lng[nl] = obj_alloc(); obj_touch(lng[nl++], k); }
        if (k == n || k == 3 * n || k == nsteps) {
            char tag[32]; snprintf(tag, sizeof tag, "churn %ldn", k / n);
            report(tag, rss_delta_mb(base), mb((double)(m + nl) * g_slot), "");
        }
    }
    printf("  %.1f ns/step\n", (now() - t0) * 1e9 / nsteps);
    for (long i = 0; i < m; i++) obj_free(sh[i]);
    report("after burst", rss_delta_mb(base), mb((double)nl * g_slot), "(immortals only live)");
    for (long i = 0; i < nl; i++) obj_free(lng[i]);
    free(sh); free(all); free(lng); free(ord);
}

// handle forms: dereference cost
static void w_deref(long n) {
    u32 slot = g_slot;
    Arena R, C, G;
    u64 cap = (u64)n * 2;
    arena_init(&R, L_RESERVE, 12, 12, P_RECENT, C_NONE, 0, slot, cap);
    arena_init(&C, L_CHUNKED, 12, 12, P_RECENT, C_NONE, 0, slot, 0);   // fixed chunks, table lookup
    arena_init(&G, L_CHUNKED, 6, 12, P_RECENT, C_NONE, 0, slot, 0);    // geometric chunks, clz lookup
    u32 *ir = malloc(n * 4), *ic = malloc(n * 4), *ig = malloc(n * 4);
    char **raw = malloc(n * sizeof *raw);
    for (long i = 0; i < n; i++) {
        ir[i] = arena_alloc(&R); ic[i] = arena_alloc(&C); ig[i] = arena_alloc(&G);
        raw[i] = arena_ptr(&R, ir[i]);
        *(u64 *)(raw[i] + 8) = i;
        *(u64 *)(arena_ptr(&C, ic[i]) + 8) = i;
        *(u64 *)(arena_ptr(&G, ig[i]) + 8) = i;
    }
    u32 *ord = malloc(n * 4);
    for (long i = 0; i < n; i++) ord[i] = i;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) shuffle(ord, n);
        printf("deref (%s order), ns per access:\n", pass ? "random" : "sequential");
        volatile u64 sink = 0;
        u64 s; double t0;
        const int REP = 3;
        s = 0; t0 = now();
        for (int r = 0; r < REP; r++) for (long i = 0; i < n; i++) s += *(u64 *)(raw[ord[i]] + 8);
        printf("  raw pointer            %.2f\n", (now() - t0) * 1e9 / (REP * n)); sink += s;
        s = 0; t0 = now();
        for (int r = 0; r < REP; r++) for (long i = 0; i < n; i++) s += *(u64 *)(R.region + (size_t)ir[ord[i]] * slot + 8);
        printf("  index, contiguous      %.2f\n", (now() - t0) * 1e9 / (REP * n)); sink += s;
        s = 0; t0 = now();
        for (int r = 0; r < REP; r++) for (long i = 0; i < n; i++) {
            u32 idx = ic[ord[i]];
            s += *(u64 *)(C.chunks[idx >> 12].base + (size_t)(idx & 4095) * slot + 8);
        }
        printf("  index, chunk table     %.2f\n", (now() - t0) * 1e9 / (REP * n)); sink += s;
        s = 0; t0 = now();
        for (int r = 0; r < REP; r++) for (long i = 0; i < n; i++) s += *(u64 *)(arena_ptr(&G, ig[ord[i]]) + 8);
        printf("  index, geometric clz   %.2f\n", (now() - t0) * 1e9 / (REP * n)); sink += s;
        s = 0; t0 = now();
        long stale = 0;
        for (int r = 0; r < REP; r++) for (long i = 0; i < n; i++) {
            u32 idx = ic[ord[i]];
            char *p = C.chunks[idx >> 12].base + (size_t)(idx & 4095) * slot;
            stale += ((u32 *)p)[1] != 0;                       // generation check against handle's gen (0)
            s += *(u64 *)(p + 8);
        }
        printf("  index+generation       %.2f  (stale=%ld)\n", (now() - t0) * 1e9 / (REP * n), stale); sink += s;
        (void)sink;
    }
}


// ---- generational-handle correctness tests ----
static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void mk(Arena *a, int layout, u32 first, u32 cap, int contract, u32 spare, u32 sgbits, u32 mag, u32 slot) {
    arena_init(a, layout, first, cap, P_RECENT, contract, spare, slot, 1u << 24);
    arena_set_sgbits(a, sgbits);
    a->magcap = mag;
}

// T1: a freed slot's handle is invalid immediately, and stays invalid after the slot is reused
static void t_basic(void) {
    Arena a; mk(&a, L_RESERVE, 8, 8, C_NONE, 0, 20, 0, 64);
    u32 i1 = arena_alloc(&a); u64 h1 = arena_handle(&a, i1);
    CHECK(arena_resolve(&a, h1) != NULL, "live handle rejected");
    arena_free(&a, i1);
    CHECK(arena_resolve(&a, h1) == NULL, "freed handle accepted");
    u32 i2 = arena_alloc(&a); u64 h2 = arena_handle(&a, i2);
    CHECK(i2 == i1, "slot not reused (test premise)");
    CHECK(arena_resolve(&a, h2) != NULL, "new handle rejected");
    CHECK(arena_resolve(&a, h1) == NULL, "old handle accepted after reuse (ABA)");
    printf("T1 basic free/reuse:                         %s\n", g_fail ? "FAIL" : "pass");
}

// T2: stale handles across many decommit/recommit cycles, driving the chunk epoch to saturation
static void t_decommit(int layout, u32 sgbits, int cycles) {
    Arena a; mk(&a, layout, 8, 8, layout == L_CHUNKED ? C_MUNMAP : C_MADVISE, 0, sgbits, 0, 64);
    u32 per = 1u << 8;
    u64 *first = malloc(cycles * sizeof *first), *last = malloc(cycles * sizeof *last);
    u64 *cur = malloc(per * sizeof *cur);
    long false_accept = 0, false_reject = 0; int before = g_fail;
    for (int c = 0; c < cycles; c++) {
        for (u32 k = 0; k < per; k++) {
            u32 idx = arena_alloc(&a);
            *(u64 *)((char *)arena_ptr(&a, idx) + 8) = c;
            cur[k] = arena_handle(&a, idx);
        }
        first[c] = cur[0]; last[c] = cur[per - 1];
        for (u32 k = 0; k < per; k++) {
            char *p = arena_resolve(&a, cur[k]);
            if (!p) false_reject++;
            else if (*(u64 *)(p + 8) != (u64)c) false_reject++;
        }
        for (u32 k = 0; k < per; k++) arena_free(&a, (u32)cur[k]);
        for (int o = 0; o <= c; o++) {                       // every handle ever issued must be dead now
            false_accept += arena_resolve(&a, first[o]) != NULL;
            false_accept += arena_resolve(&a, last[o]) != NULL;
        }
    }
    CHECK(false_accept == 0, "%ld stale handles accepted", false_accept);
    CHECK(false_reject == 0, "%ld live handles rejected", false_reject);
    printf("T2 decommit cycles (%s, sgbits=%u, %d cycles):  %s   epoch=%u frozen=%lu decommits=%lu retired=%lu\n",
           layout == L_CHUNKED ? "munmap " : "madvise", sgbits, cycles, g_fail > before ? "FAIL" : "pass",
           a.chunks[0].epoch, a.frozen_chunks, a.decommits, a.retired);
    free(first); free(last); free(cur);
}

// T3: in-slot counter saturation: hammer one slot with a tiny counter, nothing stale may ever be accepted
static void t_saturate(u32 sgbits, u32 mag) {
    Arena a; mk(&a, L_RESERVE, 8, 8, C_NONE, 0, sgbits, mag, 64);
    int N = 5000; u64 *h = malloc(N * sizeof *h); long false_accept = 0; int before = g_fail;
    for (int i = 0; i < N; i++) {
        u32 idx = arena_alloc(&a); h[i] = arena_handle(&a, idx);
        CHECK(arena_resolve(&a, h[i]) != NULL, "live handle rejected at %d", i);
        arena_free(&a, idx);
        for (int o = (i > 600 ? i - 600 : 0); o <= i; o++) false_accept += arena_resolve(&a, h[o]) != NULL;
    }
    for (int i = 0; i < N; i++) false_accept += arena_resolve(&a, h[i]) != NULL;
    CHECK(false_accept == 0, "%ld stale handles accepted", false_accept);
    printf("T3 saturation (sgbits=%2u, mag=%2u, %d reuses):  %s   retired slots=%lu chunks=%u\n",
           sgbits, mag, N, g_fail > before ? "FAIL" : "pass", a.retired, a.nchunks);
    free(h);
}

// T5: randomized fuzz against a model, decommit active
static void t_fuzz(int layout, u32 sgbits, u32 mag, long ops) {
    Arena a; mk(&a, layout, 8, 8, layout == L_CHUNKED ? C_MUNMAP : C_MADVISE, 0, sgbits, mag, 64);
    enum { LIVE = 3000, HIST = 8192 };
    u64 *live = malloc(LIVE * sizeof *live), *hist = malloc(HIST * sizeof *hist);
    u64 *tag = malloc(LIVE * sizeof *tag);
    long nl = 0, nh = 0, hh = 0, fa = 0, fr = 0, allocs = 0, frees = 0, probes = 0; int before = g_fail;
    u64 seq = 1;
    for (long k = 0; k < ops; k++) {
        u64 r = rnd() % 100;
        if (r < 40 && nl < LIVE) {
            u32 idx = arena_alloc(&a);
            *(u64 *)((char *)arena_ptr(&a, idx) + 8) = seq;
            live[nl] = arena_handle(&a, idx); tag[nl++] = seq++; allocs++;
        } else if (r < 80 && nl > 0) {
            long i = rnd() % nl;
            char *p = arena_resolve(&a, live[i]);
            if (!p || *(u64 *)(p + 8) != tag[i]) fr++;
            arena_free(&a, (u32)live[i]); frees++;
            hist[hh++ % HIST] = live[i]; if (nh < HIST) nh++;
            live[i] = live[nl - 1]; tag[i] = tag[nl - 1]; nl--;
        } else if (r < 90 && nh > 0) {
            probes++; fa += arena_resolve(&a, hist[rnd() % nh]) != NULL;
        } else if (nl > 0) {
            long i = rnd() % nl;
            char *p = arena_resolve(&a, live[i]);
            if (!p || *(u64 *)(p + 8) != tag[i]) fr++;
        }
    }
    CHECK(fa == 0, "%ld stale accepted", fa);
    CHECK(fr == 0, "%ld live rejected/corrupt", fr);
    printf("T5 fuzz (%s, sgbits=%2u, mag=%2u): %s  allocs=%ld frees=%ld stale probes=%ld  decommits=%lu retired=%lu frozen=%lu\n",
           layout == L_CHUNKED ? "munmap " : "madvise", sgbits, mag, g_fail > before ? "FAIL" : "pass",
           allocs, frees, probes, a.decommits, a.retired, a.frozen_chunks);
    free(live); free(hist); free(tag);
}

static void w_handles(void) {
    rng_state = 88172645463325252ULL;
    t_basic();
    t_decommit(L_RESERVE, 20, 5000);
    t_decommit(L_CHUNKED, 20, 5000);
    t_decommit(L_RESERVE, 8, 600);
    t_saturate(8, 0);
    t_saturate(8, 16);
    t_saturate(20, 16);
    t_fuzz(L_RESERVE, 20, 0, 20000000);
    t_fuzz(L_RESERVE, 20, 8, 20000000);
    t_fuzz(L_CHUNKED, 20, 0, 20000000);
    t_fuzz(L_CHUNKED, 12, 8, 20000000);
    t_fuzz(L_RESERVE, 8, 0, 20000000);
    printf("%s (%d failures)\n", g_fail ? "SOME TESTS FAILED" : "ALL HANDLE TESTS PASSED", g_fail);
}

// resolve cost: epoch + slot counter check, vs the earlier unchecked table lookup
static void w_resolve(long n) {
    Arena a; mk(&a, L_RESERVE, 12, 12, C_NONE, 0, 20, 0, 64);
    arena_init(&a, L_RESERVE, 12, 12, P_RECENT, C_NONE, 0, 64, (u64)n * 2); arena_set_sgbits(&a, 20);
    u64 *h = malloc(n * sizeof *h); u32 *ord = malloc(n * 4);
    for (long i = 0; i < n; i++) { u32 idx = arena_alloc(&a); *(u64 *)(arena_ptr(&a, idx) + 8) = i; h[i] = arena_handle(&a, idx); ord[i] = i; }
    for (int pass = 0; pass < 2; pass++) {
        if (pass) shuffle(ord, n);
        u64 s = 0; double t0 = now(); long miss = 0;
        for (int r = 0; r < 3; r++) for (long i = 0; i < n; i++) { char *p = arena_resolve(&a, h[ord[i]]); if (p) s += *(u64 *)(p + 8); else miss++; }
        printf("resolve (%s order): %.2f ns per access, misses=%ld\n", pass ? "random" : "sequential", (now() - t0) * 1e9 / (3 * n), miss);
        if (s == 1) printf(" ");
    }
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

int main(int argc, char **argv) {
    const char *ls = arg(argc, argv, "layout", "reserve");
    g_layout = !strcmp(ls, "malloc") ? L_MALLOC : !strcmp(ls, "chunked") ? L_CHUNKED : L_RESERVE;
    u32 first = atoi(arg(argc, argv, "first", "12")), cap = atoi(arg(argc, argv, "cap", "12"));
    const char *ps = arg(argc, argv, "policy", "lowest");
    int policy = !strcmp(ps, "recent") ? P_RECENT : P_LOWEST;
    const char *cs = arg(argc, argv, "contract", "madvise");
    int contract = !strcmp(cs, "none") ? C_NONE : !strcmp(cs, "munmap") ? C_MUNMAP : C_MADVISE;
    u32 spare = atoi(arg(argc, argv, "spare", "1"));
    g_slot = atoi(arg(argc, argv, "slot", "64"));
    long n = atol(arg(argc, argv, "n", "2000000"));
    const char *work = arg(argc, argv, "work", "ramp"), *pattern = arg(argc, argv, "pattern", "random");
    if (g_slot < 16 || g_slot % 16) { fprintf(stderr, "slot must be a multiple of 16\n"); return 2; }
    if (((u64)1 << first) * g_slot < PAGE && g_layout != L_MALLOC) { fprintf(stderr, "first chunk < 1 page\n"); return 2; }
    if (contract == C_MUNMAP && g_layout != L_CHUNKED) contract = C_MADVISE;

    if (g_layout != L_MALLOC) {
        arena_init(&A, g_layout, first, cap, policy, contract, spare, g_slot, (u64)n * 8);
        A.magcap = atoi(arg(argc, argv, "mag", "0"));
        if (A.magcap > 512) A.magcap = 512;
    }
    printf("config: layout=%s first=%u cap=%u (chunk %.0f..%.0f KB) policy=%s contract=%s spare=%u slot=%u mag=%s n=%ld work=%s\n",
           ls, first, cap, (double)(1u << first) * g_slot / 1024, (double)(1u << cap) * g_slot / 1024,
           ps, cs, spare, g_slot, arg(argc, argv, "mag", "0"), n, work);
    if (!strcmp(work, "fill")) w_fill(n);
    else if (!strcmp(work, "churn")) w_churn(n);
    else if (!strcmp(work, "ramp")) w_ramp(n, pattern);
    else if (!strcmp(work, "mixed")) w_mixed(n);
    else if (!strcmp(work, "compact")) w_compact(n);
    else if (!strcmp(work, "deref")) w_deref(n);
    else if (!strcmp(work, "handles")) { w_handles(); return g_fail != 0; }
    else if (!strcmp(work, "resolve")) w_resolve(n);
    else { fprintf(stderr, "unknown work\n"); return 2; }
    if (g_layout != L_MALLOC)
        printf("  chunks=%u decommits=%lu commits=%lu\n", A.nchunks, A.decommits, A.commits);
    return 0;
}
