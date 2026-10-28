// Lazy frames vs eager frames vs embedded frames vs plain calls, for a call chain whose leaf
// parks with probability p. All variants compute the same function (checked by checksum).
//
//   level(d, x):  if d == 0: [may park]; return mix(x)
//                 a = x + d;  r = level(d-1, a*3+1);  return r + a      (a is live across the call)
//   request loop: for r in 0..R: acc += level(D, seed + r)
//
// usage: lazy_bench <plain|eager|embed|lazy> <ntasks> <requests_per_task> <depth> <p> <work>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef uint64_t u64;
typedef uint32_t u32;
enum { DONE, PARKED };

typedef struct Frame Frame;
typedef struct Task Task;
struct Frame {
    int (*resume)(Frame *);
    Frame *parent, *child;
    u64 a, x, ret;
    u32 state, d, r;
};
struct Task {
    Frame *root, *leaf, *spill;
    u64 rng, acc, id;
    Task *next;
};

int lazy_level_resume(Frame *);

static Task *cur;
static u32 g_D, g_R, g_W;
static u64 g_thresh;
static long g_parks, g_allocs;

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static inline int draw(Task *t) {
    u64 x = t->rng;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    t->rng = x;
    return ((x * 0x2545F4914F6CDD1DULL) >> 32) < g_thresh;
}
static inline u64 mix(u64 x) {
    for (u32 i = 0; i < g_W; i++) x = x * 0x9E3779B97F4A7C15ULL ^ (x >> 29);
    return x;
}
static inline u64 seed(Task *t, u32 r) { return t->id * 1000003ULL + r; }

// ---- frame arena: exact-size slots, LIFO free list ----
static Frame *freelist;
static char *bump, *bump_end;
static inline Frame *alloc_frame(void) {
    g_allocs++;
    if (freelist) { Frame *f = freelist; freelist = f->parent; return f; }
    if (bump + sizeof(Frame) > bump_end) {
        bump = malloc(1 << 20);
        bump_end = bump + (1 << 20);
    }
    Frame *f = (Frame *)bump;
    bump += sizeof(Frame);
    return f;
}
static inline void free_frame(Frame *f) { f->parent = freelist; freelist = f; }

// ---- plain: ordinary calls, cannot park ----
static u64 plain_level(Task *t, u32 d, u64 x) {
    if (d == 0) { draw(t); return mix(x); }
    u64 a = x + d;
    u64 r = plain_level(t, d - 1, a * 3 + 1);
    return r + a;
}

// ---- eager / embedded: every call has a frame, state machines, synchronous callee resume ----
static inline __attribute__((always_inline)) int level_sm(Frame *f, int embed) {
    switch (f->state) {
    case 0:
        if (f->d == 0) {
            if (draw(cur)) { f->state = 1; cur->leaf = f; g_parks++; return PARKED; }
    case 1:
            f->ret = mix(f->x);
            return DONE;
        }
        f->a = f->x + f->d;
        {
            Frame *c = embed ? f + 1 : alloc_frame();
            c->resume = f->resume; c->parent = f; c->child = NULL;
            c->d = f->d - 1; c->x = f->a * 3 + 1; c->state = 0;
            f->child = c; f->state = 2;
            if (c->resume(c) == PARKED) return PARKED;
        }
    case 2:
        f->ret = f->child->ret + f->a;
        if (!embed) free_frame(f->child);
        return DONE;
    }
    return DONE;
}
static int level_heap(Frame *f) { return level_sm(f, 0); }
static int level_embed(Frame *f) { return level_sm(f, 1); }

static inline __attribute__((always_inline)) int root_sm(Frame *f, int embed) {
    Task *t = cur;
    switch (f->state) {
    case 0:
        f->r = 0;
    loop:
        if (f->r >= g_R) return DONE;
        {
            Frame *c = embed ? f + 1 : alloc_frame();
            c->resume = embed ? level_embed : level_heap;
            c->parent = f; c->child = NULL;
            c->d = g_D; c->x = seed(t, f->r); c->state = 0;
            f->child = c; f->state = 1;
            if (c->resume(c) == PARKED) return PARKED;
        }
    case 1:
        t->acc += f->child->ret;
        if (!embed) free_frame(f->child);
        f->r++;
        goto loop;
    }
    return DONE;
}
static int root_heap(Frame *f) { return root_sm(f, 0); }
static int root_embed(Frame *f) { return root_sm(f, 1); }

// ---- lazy: native-stack fast path; frames are created only while unwinding a real park ----
static int lazy_level_fast(Task *t, u32 d, u64 x, u64 *out) {
    if (d == 0) {
        if (draw(t)) {
            Frame *f = alloc_frame();
            f->resume = lazy_level_resume; f->parent = NULL; f->child = NULL;
            f->state = 1; f->d = 0; f->x = x;
            t->leaf = f; t->spill = f; g_parks++;
            return PARKED;
        }
        *out = mix(x);
        return DONE;
    }
    u64 a = x + d, r;
    if (lazy_level_fast(t, d - 1, a * 3 + 1, &r) == PARKED) {
        Frame *f = alloc_frame();                         // spill live locals
        f->state = 2; f->d = d; f->a = a;
        f->child = t->spill; f->child->parent = f;
        f->resume = lazy_level_resume;
        t->spill = f;
        return PARKED;
    }
    *out = r + a;
    return DONE;
}
int lazy_level_resume(Frame *f) {                         // slow path: continue after a park
    if (f->state == 1) f->ret = mix(f->x);                // the parked leaf
    else { f->ret = f->child->ret + f->a; free_frame(f->child); }
    return DONE;
}
static int lazy_root(Frame *f) {
    Task *t = cur;
    u64 val;
    if (f->state == 1) {                                  // resumed: the parked chain has finished
        t->acc += f->child->ret;
        free_frame(f->child);
        f->r++;
    }
    for (; f->r < g_R; f->r++) {
        if (lazy_level_fast(t, g_D, seed(t, f->r), &val) == PARKED) {
            Frame *c = t->spill;
            // the top spilled frame's resume pointer was set during the spill; attach to the root
            c->parent = f; f->child = c; f->state = 1;
            return PARKED;
        }
        t->acc += val;
    }
    return DONE;
}

// ---- scheduler ----
static Task *qhead, *qtail;
static void enqueue(Task *t) { t->next = NULL; if (qtail) qtail->next = t; else qhead = t; qtail = t; }
static Task *dequeue(void) {
    Task *t = qhead;
    if (t) { qhead = t->next; if (!qhead) qtail = NULL; }
    return t;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s plain|eager|embed|lazy ntasks requests depth p work\n", argv[0]);
        return 2;
    }
    const char *v = argv[1];
    long n = atol(argv[2]);
    g_R = (u32)atol(argv[3]);
    g_D = (u32)atoi(argv[4]);
    double p = atof(argv[5]);
    g_W = (u32)atoi(argv[6]);
    g_thresh = (u64)(p * 4294967296.0);
    int plain = !strcmp(v, "plain"), eager = !strcmp(v, "eager"),
        embed = !strcmp(v, "embed"), lazy = !strcmp(v, "lazy");
    if (plain && p > 0) { fprintf(stderr, "plain cannot park; use p=0\n"); return 2; }

    Task *tasks = calloc(n, sizeof(Task));
    for (long i = 0; i < n; i++) {
        Task *t = &tasks[i];
        t->id = i + 1;
        t->rng = 0x9E3779B97F4A7C15ULL * (i + 1);
        if (embed) {
            Frame *fr = calloc(g_D + 2, sizeof(Frame));
            t->root = fr;
            fr->resume = root_embed;
        } else if (eager) {
            t->root = alloc_frame();
            memset(t->root, 0, sizeof(Frame));
            t->root->resume = root_heap;
        } else if (lazy) {
            t->root = alloc_frame();
            memset(t->root, 0, sizeof(Frame));
            t->root->resume = lazy_root;
        }
    }
    g_allocs = 0;

    double t0 = now();
    long finished = 0;
    if (plain) {
        for (long i = 0; i < n; i++) {
            Task *t = &tasks[i];
            for (u32 r = 0; r < g_R; r++) t->acc += plain_level(t, g_D, seed(t, r));
        }
    } else {
        for (long i = 0; i < n; i++) enqueue(&tasks[i]);
        Task *t;
        while ((t = dequeue())) {
            cur = t;
            Frame *f = t->leaf ? t->leaf : t->root;
            t->leaf = NULL;
            int r = f->resume(f);
            while (r == DONE && f->parent) { f = f->parent; r = f->resume(f); }
            if (r == PARKED) enqueue(t); else finished++;
        }
        if (finished != n) { fprintf(stderr, "only %ld/%ld finished\n", finished, n); return 1; }
    }
    double el = now() - t0;

    u64 sum = 0;
    for (long i = 0; i < n; i++) sum ^= tasks[i].acc * (i + 1);
    double reqs = (double)n * g_R;
    printf("%s D=%u p=%g W=%u ns_per_req=%.1f ns_per_call=%.2f parks_per_req=%.4f allocs_per_req=%.2f checksum=%016lx\n",
           v, g_D, p, g_W, el * 1e9 / reqs, el * 1e9 / reqs / (g_D + 1), g_parks / reqs, g_allocs / reqs,
           (unsigned long)sum);
    return 0;
}
