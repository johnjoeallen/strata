// Option C: compiler-sized fiber frames (stackless state machines), hand-written as a
// Strata compiler would emit them. Each parking function gets an exact-size frame
// allocated from its own per-type arena. Non-parking code uses the native stack.
//
// usage: frames_bench <n> <foot_bytes> <depth> <resume:nested|direct> <alloc:arena|malloc> <rounds>
//   foot_bytes  live state the root task holds across a park (like fiber touch_kb)
//   depth       parking calls nested under the root, besides the leaf (frames = root + depth + 1) (a request handler call chain)
//   resume      nested: waking a task re-enters every frame from the root (what Rust async does)
//               direct: scheduler resumes the innermost frame; callers resumed only on return
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static long status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t n = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, key, n)) { v = atol(line + n); break; }
    if (f) fclose(f);
    return v;
}
static long count_vmas(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    long n = 0;
    int c;
    while (f && (c = fgetc(f)) != EOF) if (c == '\n') n++;
    if (f) fclose(f);
    return n;
}

// ---- per-type frame arena: exact-size slots, bump allocation in chunks, free list ----
#define CHUNK (256UL * 1024)
typedef struct { size_t slot; char *cur, *end; void *freelist; int use_malloc; } Arena;

static void *arena_alloc(Arena *a) {
    if (a->use_malloc) return malloc(a->slot);
    if (a->freelist) { void *p = a->freelist; a->freelist = *(void **)p; return p; }
    if (a->cur + a->slot > a->end) {
        char *c = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (c == MAP_FAILED) { perror("mmap"); exit(1); }
        madvise(c, CHUNK, MADV_NOHUGEPAGE);
        a->cur = c; a->end = c + CHUNK;
    }
    void *p = a->cur;
    a->cur += a->slot;
    return p;
}
static void arena_free(Arena *a, void *p) {
    if (a->use_malloc) { free(p); return; }
    *(void **)p = a->freelist;
    a->freelist = p;
}

// ---- frames ----
enum { R_YIELD, R_PARK, R_DONE, R_CALL };

typedef struct Frame Frame;
struct Frame {
    int (*resume)(Frame *);
    Frame *parent;   // caller frame
    Frame *child;    // callee frame while suspended in a call
    Frame *next;     // run-queue link (root frames only)
    Frame *leaf;     // root only: innermost frame, for direct resume
    uint32_t state;
    uint32_t depth;  // remaining depth below this level
    uint32_t round;  // leaf progress
    uint32_t is_root;
};
typedef struct { Frame h; } LevelFrame;                 // 40 B + padding: a small call frame
typedef struct { Frame h; char buf[]; } RootFrame;      // header + live state across park

static Arena root_arena, level_arena;
static int g_direct;
static long g_rounds, g_started, g_done_tasks;
static size_t g_foot;
static int (*level_fn)(Frame *);

// run queue (intrusive FIFO of root frames)
static Frame *qhead, *qtail;
static void enqueue(Frame *t) {
    t->next = NULL;
    if (qtail) qtail->next = t; else qhead = t;
    qtail = t;
}
static Frame *dequeue(void) {
    Frame *t = qhead;
    if (t) { qhead = t->next; if (!qhead) qtail = NULL; }
    return t;
}

static Frame *new_level(Frame *parent, uint32_t depth) {
    Frame *f = arena_alloc(&level_arena);
    memset(f, 0, sizeof *f);
    f->resume = level_fn;
    f->parent = parent;
    f->depth = depth;
    return f;
}

// Level function: what the compiler emits for a parking function that calls another one.
// At depth 0 it is the leaf: it announces itself, parks at the gate, then yields g_rounds times.
static int level_resume(Frame *f) {
    switch (f->state) {
    case 0:
        if (f->depth == 0) {
            g_started++;
            f->state = 1;
            return R_PARK;          // gate: idle point where memory is measured
        case 1:
            while (f->round < g_rounds) {
                f->round++;
                f->state = 1;
                return R_YIELD;
            }
            return R_DONE;
        }
        f->child = new_level(f, f->depth - 1);
        f->state = 2;
        if (g_direct) return R_CALL;
        // fallthrough: nested mode polls the child from here
    case 2:
        if (g_direct) return R_DONE;    // child finished; scheduler resumed us at the call return
        {
            Frame *c = f->child;
            int r = c->resume(c);       // re-enter the chain from the top
            if (r != R_DONE) return r;
            arena_free(&level_arena, c);
            return R_DONE;
        }
    }
    return R_DONE;
}

static int root_resume(Frame *f) {
    RootFrame *rf = (RootFrame *)f;
    switch (f->state) {
    case 0:
        for (size_t i = 0; i < g_foot; i += 64) rf->buf[i] = 1;   // the live state
        f->child = new_level(f, f->depth);
        f->state = 1;
        if (g_direct) return R_CALL;
    case 1:
        if (g_direct) { g_done_tasks++; return R_DONE; }
        {
            Frame *c = f->child;
            int r = c->resume(c);
            if (r != R_DONE) return r;
            arena_free(&level_arena, c);
            g_done_tasks++;
            return R_DONE;
        }
    }
    return R_DONE;
}

// Scheduler: runs a root's innermost frame (direct) or the root itself (nested).
// Returns when the queue is empty.
static void run_queue(void) {
    Frame *t;
    while ((t = dequeue())) {
        Frame *f = g_direct ? (t->leaf ? t->leaf : t) : t;
        for (;;) {
            int r = f->resume(f);
            if (r == R_CALL) {                       // callee frame becomes the innermost
                f = f->child;
                t->leaf = f;
                continue;
            }
            if (r == R_YIELD) { t->leaf = f; enqueue(t); }
            else if (r == R_PARK) { t->leaf = f; }
            else {                                   // R_DONE
                if (f == t) { arena_free(&root_arena, t); }
                else {                               // return to caller, free callee
                    Frame *p = f->parent;
                    arena_free(&level_arena, f);
                    f = p;
                    t->leaf = f;
                    continue;
                }
            }
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s n foot depth nested|direct arena|malloc rounds\n", argv[0]);
        return 2;
    }
    long n = atol(argv[1]);
    g_foot = (size_t)atol(argv[2]);
    uint32_t depth = (uint32_t)atoi(argv[3]);
    g_direct = !strcmp(argv[4], "direct");
    int use_malloc = !strcmp(argv[5], "malloc");
    g_rounds = atol(argv[6]);

    level_fn = level_resume;
    root_arena = (Arena){.slot = (sizeof(RootFrame) + g_foot + 15) & ~15UL, .use_malloc = use_malloc};
    level_arena = (Arena){.slot = (sizeof(LevelFrame) + 15) & ~15UL, .use_malloc = use_malloc};

    Frame **roots = malloc(n * sizeof *roots);
    long rss0 = status_kb("VmRSS:"), vsz0 = status_kb("VmSize:"), vma0 = count_vmas();

    double t0 = now();
    for (long i = 0; i < n; i++) {
        Frame *r = arena_alloc(&root_arena);
        memset(r, 0, sizeof *r);
        r->resume = root_resume;
        r->depth = depth;
        r->is_root = 1;
        roots[i] = r;
        enqueue(r);
    }
    double t_spawn = now() - t0;

    t0 = now();
    run_queue();                           // every task runs to its gate and parks
    double t_start = now() - t0;
    if (g_started != n) { fprintf(stderr, "only %ld/%ld tasks reached the gate\n", g_started, n); return 1; }

    long rss1 = status_kb("VmRSS:"), vsz1 = status_kb("VmSize:"), vma1 = count_vmas();

    for (long i = 0; i < n; i++) enqueue(roots[i]);   // wake everyone (their leaf is resumed)
    t0 = now();
    run_queue();                           // g_rounds yields each, then unwind and free
    double t_run = now() - t0;
    if (g_done_tasks != n) { fprintf(stderr, "only %ld/%ld tasks finished\n", g_done_tasks, n); return 1; }

    printf("frames resume=%s alloc=%s n=%ld foot_B=%zu depth=%u "
           "rss_per_task_B=%.0f vsz_per_task_B=%.0f vmas=%ld frame_B=%zu "
           "spawn_ns=%.0f start_ns=%.0f yield_ns=%.1f\n",
           argv[4], argv[5], n, g_foot, depth,
           (double)(rss1 - rss0) * 1024 / n, (double)(vsz1 - vsz0) * 1024 / n, vma1 - vma0,
           root_arena.slot + (size_t)(depth + 1) * level_arena.slot,
           t_spawn * 1e9 / n, t_start * 1e9 / n, t_run * 1e9 / ((double)n * g_rounds));
    return 0;
}
