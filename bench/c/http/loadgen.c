// loadgen — hold many connections, drive a closed-loop subset, report throughput and latency.
//
//   conns=N      total connections (each does one request/response, then idles unless active)
//   active=A     the first A connections loop request->response for `seconds` after ramp-up
//   threads=T    client threads (pinned to cpu0..cpu0+T-1)
//   srcips=K     spread connections over source addresses 127.1.k.1 (28k ephemeral ports each)
//   hold=S       keep all connections open S seconds after the measurement (for RSS sampling)
//
// Phases: ramp (connect + one request each, <= inflight connecting per thread) -> go (active conns loop)
// -> measure `seconds` -> hold -> exit. Prints one result line; the READY marker lets a script sample memory.
//
// Assumes each response arrives in one recv (they are < 200 B and sent with one send()).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t u64;
typedef uint32_t u32;

#define NBINS 20000            // 1 us bins up to 20 ms; beyond goes to overflow
static const char REQ[] = "GET / HTTP/1.1\r\nHost: bench\r\n\r\n";
static const char REQ_MIN[] = "GET / HTTP/1.1\r\n\r\n";       // 18 B: used for pipelined batches
static const char REQ_MIN_HOST[] = "GET / HTTP/1.1\r\nHost: bench\r\n\r\n";   // host=1: strict servers (net/http) need a Host header
static const char REQ_SLOW_HOST[] = "GET /slow HTTP/1.1\r\nHost: bench\r\n\r\n";
static int g_host;
static const char REQ_SLOW[] = "GET /slow HTTP/1.1\r\n\r\n";
static int g_slowpos;
static char *pipe_buf; static size_t pipe_len; static int g_pipe = 1; static volatile long g_resp_len;

typedef struct { int fd; u32 id; unsigned char st; u32 got; u64 t0; } Conn;      // st: 0 connecting, 1 awaiting resp, 2 idle, 3 active-looping

typedef struct {
    int tid;
    long nconn;
    Conn *conns;
    long next;                  // next conn index to start
    long inflight;
    u32 *bins; u64 overflow, maxlat;
    u64 nreq, first_sum, first_cnt, first_max;
    u64 err_connect, err_io, err_partial;
    struct sockaddr_in dst;
} Th;

static int g_threads = 1, g_cpu0 = 8, g_srcips = 1, g_inflight = 400, g_port = 18080;
static long g_conns = 1000, g_active = 0;
static int g_secs = 5, g_hold = 0, g_timeout = 120;
static _Atomic long ready_cnt, failed_cnt;
static _Atomic int go, measuring, stop;

static inline u64 now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static int start_conn(Th *t, int ep, Conn *c) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { t->err_connect++; return -1; }
    struct linger lg = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in src = { .sin_family = AF_INET };
    src.sin_addr.s_addr = htonl((127u << 24) | (1u << 16) | ((c->id % g_srcips) << 8) | 1);
    setsockopt(fd, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)&src, sizeof src) < 0) { close(fd); t->err_connect++; return -1; }
    c->fd = fd; c->st = 0;
    if (connect(fd, (struct sockaddr *)&t->dst, sizeof t->dst) < 0 && errno != EINPROGRESS) { close(fd); c->fd = -1; t->err_connect++; return -1; }
    struct epoll_event ev = { .events = EPOLLOUT, .data.ptr = c };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    t->inflight++;
    return 0;
}

static inline void send_req(Th *t, Conn *c, int ep, int record_start) {
    if (record_start) c->t0 = now_ns();
    c->got = 0;
    if (c->st == 3 && g_pipe > 1) { if (send(c->fd, pipe_buf, pipe_len, MSG_NOSIGNAL) != (ssize_t)pipe_len) t->err_io++; }
    else if (send(c->fd, REQ, sizeof REQ - 1, MSG_NOSIGNAL) != (ssize_t)sizeof REQ - 1) { t->err_io++; }
    (void)ep;
}

static void *thread_main(void *arg) {
    Th *t = arg;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_cpu0 + t->tid, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    int ep = epoll_create1(0);
    struct epoll_event evs[256];
    static __thread char rb[65536];
    long done_first = 0;
    int kicked = 0;
    while (!atomic_load(&stop)) {
        while (t->inflight < g_inflight && t->next < t->nconn) {
            Conn *c = &t->conns[t->next++];
            if (start_conn(t, ep, c) < 0) { atomic_fetch_add(&failed_cnt, 1); done_first++; }
        }
        if (!kicked && atomic_load(&go)) {                    // release the active connections
            kicked = 1;
            for (long i = 0; i < t->nconn; i++) {
                Conn *c = &t->conns[i];
                if (c->fd > 0 && c->id < (u32)g_active && c->st == 2) { c->st = 3; send_req(t, c, ep, 1); }
            }
        }
        int n = epoll_wait(ep, evs, 256, 50);
        for (int i = 0; i < n; i++) {
            Conn *c = evs[i].data.ptr;
            u32 e = evs[i].events;
            if (c->st == 0) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
                t->inflight--;
                if (err || (e & (EPOLLERR | EPOLLHUP))) {
                    t->err_connect++; close(c->fd); c->fd = -1; atomic_fetch_add(&failed_cnt, 1); done_first++;
                    continue;
                }
                struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
                epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
                c->st = 1;
                send_req(t, c, ep, 0);
                continue;
            }
            if (e & (EPOLLERR | EPOLLHUP)) { t->err_io++; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); close(c->fd); c->fd = -1; continue; }
            ssize_t r = recv(c->fd, rb, sizeof rb, 0);
            if (r <= 0) { if (r < 0 && errno == EAGAIN) continue; t->err_io++; epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL); close(c->fd); c->fd = -1; continue; }
            if (!(c->st == 3 && g_pipe > 1) && (r < 40 || !memmem(rb, r, "\r\n\r\n", 4))) { t->err_partial++; continue; }
            if (c->st == 1) {                                  // first response: connection is established and warm
                if (!g_resp_len) g_resp_len = r;
                if (c->id < (u32)g_active) {
                    c->st = 2;
                    if (atomic_load(&go)) { c->st = 3; send_req(t, c, ep, 1); }
                } else c->st = 2;
                atomic_fetch_add(&ready_cnt, 1); done_first++;
            } else if (c->st == 3) {
                if (g_pipe > 1) {
                    if (c->got == 0 && atomic_load_explicit(&measuring, memory_order_relaxed)) {   // first bytes of this batch
                        u64 fu = (now_ns() - c->t0) / 1000;
                        t->first_sum += fu; t->first_cnt++; if (fu > t->first_max) t->first_max = fu;
                    }
                    c->got += (u32)r;
                    if (c->got < (u32)(g_pipe * g_resp_len)) continue;
                }
                u64 lat = (now_ns() - c->t0) / 1000;
                if (atomic_load_explicit(&measuring, memory_order_relaxed)) {
                    t->nreq += g_pipe;
                    if (lat < NBINS) t->bins[lat]++; else t->overflow++;
                    if (lat > t->maxlat) t->maxlat = lat;
                }
                send_req(t, c, ep, 1);
            }
        }
    }
    return NULL;
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}
static long mem_avail_kb(void) {
    FILE *f = fopen("/proc/meminfo", "r"); char l[256]; long v = -1;
    while (f && fgets(l, sizeof l, f)) if (!strncmp(l, "MemAvailable:", 13)) { v = atol(l + 13); break; }
    if (f) fclose(f);
    return v;
}

int main(int argc, char **argv) {
    g_conns = atol(arg(argc, argv, "conns", "1000"));
    g_active = atol(arg(argc, argv, "active", "0"));
    g_threads = atoi(arg(argc, argv, "threads", "1"));
    g_cpu0 = atoi(arg(argc, argv, "cpu0", "8"));
    g_srcips = atoi(arg(argc, argv, "srcips", "1"));
    g_secs = atoi(arg(argc, argv, "seconds", "5"));
    g_hold = atoi(arg(argc, argv, "hold", "0"));
    g_port = atoi(arg(argc, argv, "port", "18080"));
    g_inflight = atoi(arg(argc, argv, "inflight", "400"));
    g_pipe = atoi(arg(argc, argv, "pipeline", "1"));
    g_host = atoi(arg(argc, argv, "host", "0"));
    if (g_pipe > 1) {
        g_slowpos = atoi(arg(argc, argv, "slowpos", "0"));            // 1-based position of a GET /slow request in each batch
        pipe_buf = malloc((size_t)g_pipe * sizeof REQ_MIN_HOST + sizeof REQ_SLOW_HOST);
        pipe_len = 0;
        for (int i = 1; i <= g_pipe; i++) {
            const char *rq = i == g_slowpos ? (g_host ? REQ_SLOW_HOST : REQ_SLOW) : (g_host ? REQ_MIN_HOST : REQ_MIN);
            size_t n = (i == g_slowpos ? (g_host ? sizeof REQ_SLOW_HOST : sizeof REQ_SLOW) : (g_host ? sizeof REQ_MIN_HOST : sizeof REQ_MIN)) - 1;
            memcpy(pipe_buf + pipe_len, rq, n); pipe_len += n;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    Th *ths = calloc(g_threads, sizeof(Th));
    pthread_t pt[64];
    long avail0 = mem_avail_kb();
    for (int i = 0; i < g_threads; i++) {
        Th *t = &ths[i];
        t->tid = i;
        t->nconn = 0;
        for (long g = i; g < g_conns; g += g_threads) t->nconn++;
        t->conns = calloc(t->nconn ? t->nconn : 1, sizeof(Conn));
        long k = 0;
        for (long g = i; g < g_conns; g += g_threads) { t->conns[k].id = (u32)g; t->conns[k].fd = -1; k++; }
        t->bins = calloc(NBINS, sizeof(u32));
        t->dst = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(g_port) };
        t->dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    u64 t_start = now_ns();
    for (int i = 0; i < g_threads; i++) pthread_create(&pt[i], NULL, thread_main, &ths[i]);

    while (atomic_load(&ready_cnt) + atomic_load(&failed_cnt) < g_conns) {
        usleep(20000);
        if ((now_ns() - t_start) / 1000000000ull > (u64)g_timeout) { fprintf(stderr, "ramp timeout\n"); break; }
    }
    double ramp = (now_ns() - t_start) / 1e9;
    long ready = atomic_load(&ready_cnt);
    printf("READY conns=%ld failed=%ld ramp=%.2fs\n", ready, atomic_load(&failed_cnt), ramp);
    fflush(stdout);
    usleep(500000);                                            // let the server finish settling
    long avail1 = mem_avail_kb();

    atomic_store(&go, 1);
    usleep(200000);                                            // active conns warm up
    u64 m0 = now_ns();
    atomic_store(&measuring, 1);
    sleep(g_secs);
    atomic_store(&measuring, 0);
    double el = (now_ns() - m0) / 1e9;
    if (g_hold) { printf("HOLD %ds\n", g_hold); fflush(stdout); sleep(g_hold); }
    atomic_store(&stop, 1);
    for (int i = 0; i < g_threads; i++) pthread_join(pt[i], NULL);

    u64 total = 0, over = 0, maxl = 0, ec = 0, ei = 0, ep_ = 0, fsum = 0, fcnt = 0, fmax = 0;
    u64 *bins = calloc(NBINS, sizeof(u64));
    for (int i = 0; i < g_threads; i++) {
        fsum += ths[i].first_sum; fcnt += ths[i].first_cnt; if (ths[i].first_max > fmax) fmax = ths[i].first_max;
        total += ths[i].nreq; over += ths[i].overflow; if (ths[i].maxlat > maxl) maxl = ths[i].maxlat;
        ec += ths[i].err_connect; ei += ths[i].err_io; ep_ += ths[i].err_partial;
        for (int b = 0; b < NBINS; b++) bins[b] += ths[i].bins[b];
    }
    double pc[4] = { 0.50, 0.90, 0.99, 0.999 }; long pv[4] = { -1, -1, -1, -1 };
    u64 acc = 0; int pi = 0;
    u64 nb = over; for (int b = 0; b < NBINS; b++) nb += bins[b];     // latency samples = batches
    for (int b = 0; b < NBINS && pi < 4; b++) {
        acc += bins[b];
        while (pi < 4 && nb && acc >= (u64)(pc[pi] * nb)) pv[pi++] = b;
    }
    for (; pi < 4; pi++) pv[pi] = (long)maxl;
    printf("conns=%ld ready=%ld active=%ld threads=%d | reqs=%lu rps=%.0f | lat_us p50=%ld p90=%ld p99=%ld p999=%ld max=%lu (>20ms: %lu) | "
           "err connect=%lu io=%lu partial=%lu | first_resp_us mean=%.0f max=%lu | client_mem_delta=%ld MB\n",
           g_conns, ready, g_active, g_threads, (unsigned long)total, total / el, pv[0], pv[1], pv[2], pv[3], (unsigned long)maxl,
           (unsigned long)over, (unsigned long)ec, (unsigned long)ei, (unsigned long)ep_, fcnt ? (double)fsum / fcnt : 0.0, (unsigned long)fmax, (avail0 - avail1) / 1024);
    return 0;
}
