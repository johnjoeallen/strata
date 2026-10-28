// Group 1: fiber execution contexts.
// One small contiguous stack per fiber, hand-written context switch.
// Measures address space, VMA count, idle RSS, creation, switch and teardown cost.
//
// usage: fiber_bench <mode> <nfibers> <stack_kb> <touch_kb> <rounds> <thp:0|1>
//   mode: pool      one mmap for all stacks, no guard pages
//         poolguard one mmap for all stacks, mprotect guard page per stack
//         each      one mmap (with guard) per fiber
//         malloc    glibc malloc per stack (no guard)
//   touch_kb: stack bytes each fiber dirties before parking (its idle footprint)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <alloca.h>
#include <sys/mman.h>

__asm__(
    ".text\n.globl ctx_switch\n.type ctx_switch,@function\nctx_switch:\n"
    "pushq %rbp\npushq %rbx\npushq %r12\npushq %r13\npushq %r14\npushq %r15\n"
    "movq %rsp, (%rdi)\nmovq %rsi, %rsp\n"
    "popq %r15\npopq %r14\npopq %r13\npopq %r12\npopq %rbx\npopq %rbp\nret\n");
void ctx_switch(void **save_sp, void *new_sp);

#define PAGE 4096UL

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

enum { M_POOL, M_POOLGUARD, M_EACH, M_MALLOC };

typedef struct { void *sp; char *base; } Fiber;

static Fiber *fibers;
static void *main_sp;
static size_t g_touch_bytes;
static long g_rounds;
static int g_cur;

static void fiber_main(void) {
    volatile char *b = alloca(g_touch_bytes ? g_touch_bytes : 1);
    for (size_t i = 0; i < g_touch_bytes; i += PAGE) b[i] = 1;
    for (long r = 0; r < g_rounds; r++)
        ctx_switch(&fibers[g_cur].sp, main_sp);
    for (;;) ctx_switch(&fibers[g_cur].sp, main_sp);
}

static void *init_stack(char *base, size_t size) {
    uintptr_t top = ((uintptr_t)base + size) & ~15UL;
    void **p = (void **)top;
    *--p = 0;                       // fake return address, keeps entry rsp = 8 mod 16
    *--p = (void *)fiber_main;      // popped by ret
    for (int i = 0; i < 6; i++) *--p = 0;
    return p;
}

int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: %s mode n stack_kb touch_kb rounds thp\n", argv[0]); return 2; }
    const char *mode_s = argv[1];
    int mode = !strcmp(mode_s, "pool") ? M_POOL : !strcmp(mode_s, "poolguard") ? M_POOLGUARD
             : !strcmp(mode_s, "each") ? M_EACH : M_MALLOC;
    long n = atol(argv[2]);
    size_t stack = (size_t)atol(argv[3]) * 1024;
    g_touch_bytes = (size_t)atol(argv[4]) * 1024;
    g_rounds = atol(argv[5]);
    int thp = atoi(argv[6]);
    if (g_touch_bytes + 512 > stack) { fprintf(stderr, "touch exceeds stack\n"); return 2; }

    fibers = calloc(n, sizeof(Fiber));
    long rss0 = status_kb("VmRSS:"), vsz0 = status_kb("VmSize:"), vma0 = count_vmas();

    double t0 = now();
    char *pool = NULL;
    size_t stride = (mode == M_POOLGUARD || mode == M_EACH) ? stack + PAGE : stack;
    if (mode == M_POOL || mode == M_POOLGUARD) {
        pool = mmap(NULL, stride * n, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (pool == MAP_FAILED) { perror("mmap pool"); return 1; }
        if (!thp) madvise(pool, stride * n, MADV_NOHUGEPAGE);
    }
    for (long i = 0; i < n; i++) {
        char *s;
        switch (mode) {
        case M_POOL: s = pool + i * stride; break;
        case M_POOLGUARD:
            s = pool + i * stride;
            if (mprotect(s, PAGE, PROT_NONE)) { perror("mprotect"); return 1; }
            s += PAGE; break;
        case M_EACH:
            s = mmap(NULL, stride, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (s == MAP_FAILED) { perror("mmap each"); return 1; }
            if (!thp) madvise(s, stride, MADV_NOHUGEPAGE);
            if (mprotect(s, PAGE, PROT_NONE)) { perror("mprotect"); return 1; }
            s += PAGE; break;
        default: s = malloc(stack); if (!s) { perror("malloc"); return 1; }
        }
        fibers[i].base = s;
        fibers[i].sp = init_stack(s, stack);
    }
    double t_create = now() - t0;

    // first visit: every fiber starts, dirties its footprint, and parks
    t0 = now();
    for (long i = 0; i < n; i++) { g_cur = i; ctx_switch(&main_sp, fibers[i].sp); }
    double t_start = now() - t0;

    long rss1 = status_kb("VmRSS:"), vsz1 = status_kb("VmSize:"), vma1 = count_vmas();

    // steady-state switching: g_rounds-1 further visits per fiber
    long visits = (g_rounds > 1 ? g_rounds - 1 : 0) * n;
    t0 = now();
    for (long r = 1; r < g_rounds; r++)
        for (long i = 0; i < n; i++) { g_cur = i; ctx_switch(&main_sp, fibers[i].sp); }
    double t_run = now() - t0;

    t0 = now();
    if (mode == M_POOL || mode == M_POOLGUARD) munmap(pool, stride * n);
    else for (long i = 0; i < n; i++) {
        if (mode == M_EACH) munmap(fibers[i].base - PAGE, stride);
        else free(fibers[i].base);
    }
    double t_teardown = now() - t0;

    printf("mode=%s n=%ld stack_kb=%zu touch_kb=%zu thp=%d "
           "rss_per_fiber_B=%.0f vsz_per_fiber_B=%.0f vmas=%ld "
           "create_ns=%.0f start_ns=%.0f switch_ns=%.1f teardown_ns=%.0f\n",
           mode_s, n, stack / 1024, g_touch_bytes / 1024, thp,
           (double)(rss1 - rss0) * 1024 / n, (double)(vsz1 - vsz0) * 1024 / n, vma1 - vma0,
           t_create * 1e9 / n, t_start * 1e9 / n,
           visits ? t_run * 1e9 / visits / 2 : 0.0, t_teardown * 1e9 / n);
    return 0;
}
