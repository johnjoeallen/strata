// loadgen_resp - RESP load generator for mini-redis (the original and the Strata/C one speak the same protocol).
//
//   mode=getset  `conns` closed-loop connections; each sends `pipe` commands (readpct% GET, the rest SET of `vsize` bytes over `keys` keys), waits for all the replies, repeats
//   mode=pubsub  `subs` subscribers (each on all `chans` channels) + `conns` publishers closed-loop PUBLISHing `vsize`-byte messages whose first 16 bytes are a timestamp;
//                reports publish rate, deliveries/s and publish-to-delivery latency (same machine, same monotonic clock)
//   mode=idle    open `conns` connections, PING once each, hold them (RSS sampling by a script);  mode=idlesub: the same, each subscribed to `chans` channels
//   threads=T cpu0=C (client threads pinned to C..C+T-1)  seconds=S  hold=S  port=P  srcips=K (spread connections over 127.1.k.1 for more than 28k)
// Output: one result line, and READY once all connections are up (for memory sampling).
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
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
#define NBINS 50000                                      // 1 us bins up to 50 ms; the rest go to `over`

static int g_srcbase = 1, g_rate = 0;
static int g_port = 6379, g_threads = 1, g_cpu0 = 8, g_srcips = 1, g_pipe = 1, g_vsize = 16, g_readpct = 90, g_secs = 5, g_hold = 0;
static long g_conns = 16, g_subs = 0, g_keys = 10000, g_chans = 1;
static const char *g_mode = "getset";
static pthread_barrier_t g_bar;
static volatile int g_ready_cnt;
static u64 g_t_start, g_t_end;

static u64 now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

typedef struct { int fd; int role; char *rb; size_t rlen, rcap; int outstanding; u64 t0, due; u32 rng; } Conn;   // role 0 client, 1 subscriber
typedef struct {
    int tid; long nclients, nsubs, nidle;
    Conn *c; long nc;
    u32 *bins, *dbins; u64 over, dover, nops, ndeliv, nbatches, errs, maxlat, dmax;
    char *wbuf; size_t wcap;
} Th;

// One reply at p[0..n): its length, 0 if incomplete, -1 if malformed.
static long reply_len(const char *p, size_t n) {
    if (!n) return 0;
    const char *e = memchr(p, '\n', n);
    if (!e) return 0;
    size_t hl = (size_t)(e - p) + 1;
    switch (p[0]) {
    case '+': case '-': case ':': return (long)hl;
    case '$': { long len = atol(p + 1); if (len < 0) return (long)hl; size_t tot = hl + (size_t)len + 2; return n >= tot ? (long)tot : 0; }
    case '*': { long cnt = atol(p + 1); size_t off = hl; for (long i = 0; i < cnt; i++) { long r = reply_len(p + off, n - off); if (r <= 0) return r; off += (size_t)r; } return (long)off; }
    }
    return -1;
}

static int dial(int tid, long idx) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (g_srcips > 1) {
        struct sockaddr_in s = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7F000001u + ((u32)g_srcbase << 16) + (u32)((idx % g_srcips) << 8)) };
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, (struct sockaddr *)&s, sizeof s) < 0) { perror("bind"); exit(1); }
    }
    struct sockaddr_in d = { .sin_family = AF_INET, .sin_port = htons(g_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    for (int tries = 0; connect(fd, (struct sockaddr *)&d, sizeof d) < 0; tries++) {
        if (tries > 50) { perror("connect"); exit(1); }
        usleep(2000);
    }
    (void)tid;
    return fd;
}
static size_t enc_cmd(char *o, int argc, const char **argv, const size_t *lens) {      // RESP array of bulk strings
    size_t n = (size_t)sprintf(o, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) { n += (size_t)sprintf(o + n, "$%zu\r\n", lens[i]); memcpy(o + n, argv[i], lens[i]); n += lens[i]; memcpy(o + n, "\r\n", 2); n += 2; }
    return n;
}
// synchronous read of `count` replies on a connection that is not yet in the epoll loop
static void read_replies(Conn *c, int count) {
    while (count > 0) {
        long r = reply_len(c->rb, c->rlen);
        if (r < 0) { fprintf(stderr, "malformed reply\n"); exit(1); }
        if (r > 0) { memmove(c->rb, c->rb + r, c->rlen - (size_t)r); c->rlen -= (size_t)r; count--; continue; }
        if (c->rlen == c->rcap) { c->rcap *= 2; c->rb = realloc(c->rb, c->rcap); }
        ssize_t k = recv(c->fd, c->rb + c->rlen, c->rcap - c->rlen, 0);
        if (k <= 0) { fprintf(stderr, "closed during setup\n"); exit(1); }
        c->rlen += (size_t)k;
    }
}
static void subscribe_all(Th *t, Conn *c) {
    char chan[32]; char *buf = malloc(64 + (size_t)g_chans * 40); size_t n = 0;
    n += (size_t)sprintf(buf, "*%ld\r\n$9\r\nsubscribe\r\n", g_chans + 1);
    for (long i = 0; i < g_chans; i++) { int l = sprintf(chan, "c%ld", i); n += (size_t)sprintf(buf + n, "$%d\r\n%s\r\n", l, chan); }
    if (send(c->fd, buf, n, 0) != (ssize_t)n) { perror("send subscribe"); exit(1); }
    free(buf);
    read_replies(c, (int)g_chans);
    (void)t;
}

static inline u32 xr(u32 *s) { u32 x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static void record(u32 *bins, u64 *over, u64 *mx, u64 ns) { u64 us = ns / 1000; if (us < NBINS) bins[us]++; else (*over)++; if (ns > *mx) *mx = ns; }

// Build and send the next batch of `pipe` commands on a client connection.
static void send_batch(Th *t, Conn *c) {
    size_t n = 0;
    char val[1 << 16]; size_t vs = (size_t)g_vsize < sizeof val ? (size_t)g_vsize : sizeof val;
    for (int i = 0; i < g_pipe; i++) {
        if (n + vs + 256 > t->wcap) { t->wcap = (n + vs + 256) * 2; t->wbuf = realloc(t->wbuf, t->wcap); }
        char key[32]; size_t kl;
        if (!strcmp(g_mode, "pubsub")) {
            u64 ts = now_ns(); memset(val, 'x', vs);
            if (vs >= 16) { char h[17]; sprintf(h, "%016llx", (unsigned long long)ts); memcpy(val, h, 16); }
            kl = (size_t)sprintf(key, "c%u", xr(&c->rng) % (u32)g_chans);
            const char *a[3] = { "publish", key, val }; size_t l[3] = { 7, kl, vs };
            n += enc_cmd(t->wbuf + n, 3, a, l);
        } else {
            kl = (size_t)sprintf(key, "key:%u", xr(&c->rng) % (u32)g_keys);
            if ((int)(xr(&c->rng) % 100) < g_readpct) { const char *a[2] = { "get", key }; size_t l[2] = { 3, kl }; n += enc_cmd(t->wbuf + n, 2, a, l); }
            else { memset(val, 'v', vs); const char *a[3] = { "set", key, val }; size_t l[3] = { 3, kl, vs }; n += enc_cmd(t->wbuf + n, 3, a, l); }
        }
    }
    c->t0 = now_ns(); c->outstanding = g_pipe;
    size_t off = 0;
    while (off < n) {                                    // blocking-ish send: loopback with small batches; retry on EAGAIN
        ssize_t k = send(c->fd, t->wbuf + off, n - off, MSG_NOSIGNAL);
        if (k > 0) { off += (size_t)k; continue; }
        if (errno == EAGAIN || errno == EINTR) { usleep(20); continue; }
        t->errs++; return;
    }
}

static void *thread_main(void *arg_) {
    Th *t = arg_;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(g_cpu0 + t->tid, &cs); pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    long total = t->nclients + t->nsubs + t->nidle;
    t->c = calloc((size_t)total, sizeof(Conn)); t->nc = total;
    t->bins = calloc(NBINS, sizeof(u32)); t->dbins = calloc(NBINS, sizeof(u32));
    t->wcap = 1 << 16; t->wbuf = malloc(t->wcap);
    int ep = epoll_create1(0);
    int idle = !strcmp(g_mode, "idle") || !strcmp(g_mode, "idlesub");
    for (long i = 0; i < total; i++) {
        Conn *c = &t->c[i];
        c->fd = dial(t->tid, t->tid + i * g_threads);
        c->rcap = idle ? 256 : 1 << 16; c->rb = malloc(c->rcap); c->rng = 2463534242u ^ (u32)(t->tid * 7919 + i * 104729 + 1);
        c->role = (i >= t->nclients && i < t->nclients + t->nsubs) ? 1 : 0;
        if (c->role == 1 || !strcmp(g_mode, "idlesub")) { c->role = 1; subscribe_all(t, c); }
        else if (idle) { if (send(c->fd, "*1\r\n$4\r\nping\r\n", 14, 0) != 14) exit(1); read_replies(c, 1); }
    }
    for (long i = 0; i < total; i++) {
        Conn *c = &t->c[i];
        fcntl(c->fd, F_SETFL, O_NONBLOCK);
        struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = c };
        epoll_ctl(ep, EPOLL_CTL_ADD, c->fd, &ev);
    }
    __sync_fetch_and_add(&g_ready_cnt, 1);
    pthread_barrier_wait(&g_bar);                        // all threads connected (and subscribed)
    pthread_barrier_wait(&g_bar);                        // main has set the measurement window (or finished holding the idle connections)
    if (idle) return NULL;
    if (g_rate == 0) for (long i = 0; i < total; i++) if (t->c[i].role == 0) send_batch(t, &t->c[i]);
    struct epoll_event evs[256];
    u64 interval = g_rate > 0 ? (u64)(1e9 * (double)(g_conns * g_pipe) / (double)g_rate) : 0;   // per connection, per batch
    for (long i = 0; i < total; i++) if (t->c[i].role == 0) t->c[i].due = now_ns() + (u64)i * interval / (u64)(t->nclients ? t->nclients : 1);
    while (now_ns() < g_t_end) {
        if (g_rate > 0) {
            u64 nw = now_ns();
            for (long i = 0; i < t->nclients; i++) { Conn *c = &t->c[i]; if (c->outstanding == 0 && c->due <= nw && nw < g_t_end) { c->t0 = c->due; send_batch(t, c); c->t0 = c->due; } }
        }
        int n = epoll_wait(ep, evs, 256, g_rate > 0 ? 0 : 20);
        for (int e = 0; e < n; e++) {
            Conn *c = evs[e].data.ptr;
            for (;;) {
                if (c->rlen == c->rcap) { c->rcap *= 2; c->rb = realloc(c->rb, c->rcap); }
                ssize_t k = recv(c->fd, c->rb + c->rlen, c->rcap - c->rlen, 0);
                if (k > 0) c->rlen += (size_t)k;
                else if (k == 0) { t->errs++; goto next_conn; }
                else if (errno == EINTR) continue;
                else break;
                // parse every complete reply
                size_t off = 0;
                for (;;) {
                    long r = reply_len(c->rb + off, c->rlen - off);
                    if (r < 0) { t->errs++; goto next_conn; }
                    if (r == 0) break;
                    u64 now = now_ns();
                    if (c->role == 1 && c->rb[off] != '*') { t->errs++; close(c->fd); c->fd = -1; goto next_conn; }   // e.g. "-ERR subscriber too slow": the server dropped us
                    if (c->role == 1) {                  // a delivery: ["message", channel, data]; data starts with the publish timestamp
                        const char *p = c->rb + off; const char *q = p;
                        for (int skip = 0; skip < 3; skip++) q = memchr(q, '\n', (size_t)(c->rb + c->rlen - q)) + 1;   // "*3", "$7", "message"
                        q = memchr(q, '\n', (size_t)(c->rb + c->rlen - q)) + 1;                                           // "$n" of the channel
                        q = memchr(q, '\n', (size_t)(c->rb + c->rlen - q)) + 1;                                           // the channel
                        q = memchr(q, '\n', (size_t)(c->rb + c->rlen - q)) + 1;                                           // "$m" of the data
                        if (now >= g_t_start && c->rb + c->rlen - q >= 16) {
                            char h[17]; memcpy(h, q, 16); h[16] = 0; u64 ts = strtoull(h, NULL, 16);
                            if (now > ts) record(t->dbins, &t->dover, &t->dmax, now - ts);
                            t->ndeliv++;
                        }
                    } else if (c->outstanding > 0 && --c->outstanding == 0) {
                        if (now >= g_t_start) { record(t->bins, &t->over, &t->maxlat, now - c->t0); t->nops += (u64)g_pipe; t->nbatches++; }
                        off += (size_t)r;
                        if (g_rate > 0) c->due = c->t0 + interval;       // paced: the next batch starts one interval after this one STARTED (no coordinated omission)
                        else if (now < g_t_end) send_batch(t, c);
                        continue;
                    }
                    off += (size_t)r;
                }
                if (off) { memmove(c->rb, c->rb + off, c->rlen - off); c->rlen -= off; }
            }
            next_conn:;
        }
    }
    return NULL;
}

static u64 pct(u32 *bins, u64 over, double p) {
    u64 tot = over; for (int i = 0; i < NBINS; i++) tot += bins[i];
    if (!tot) return 0;
    u64 want = (u64)(p * (double)tot), acc = 0;
    for (int i = 0; i < NBINS; i++) { acc += bins[i]; if (acc > want) return (u64)i; }
    return NBINS;
}

int main(int argc, char **argv) {
    g_port = atoi(arg(argc, argv, "port", "6379")); g_threads = atoi(arg(argc, argv, "threads", "1")); g_cpu0 = atoi(arg(argc, argv, "cpu0", "8"));
    g_srcips = atoi(arg(argc, argv, "srcips", "1")); g_srcbase = atoi(arg(argc, argv, "srcbase", "1")); g_rate = atoi(arg(argc, argv, "rate", "0")); g_pipe = atoi(arg(argc, argv, "pipe", "1")); g_vsize = atoi(arg(argc, argv, "vsize", "16"));
    g_readpct = atoi(arg(argc, argv, "readpct", "90")); g_secs = atoi(arg(argc, argv, "seconds", "5")); g_hold = atoi(arg(argc, argv, "hold", "0"));
    g_conns = atol(arg(argc, argv, "conns", "16")); g_subs = atol(arg(argc, argv, "subs", "0")); g_keys = atol(arg(argc, argv, "keys", "10000"));
    g_chans = atol(arg(argc, argv, "chans", "1")); g_mode = arg(argc, argv, "mode", "getset");
    int idle = !strcmp(g_mode, "idle") || !strcmp(g_mode, "idlesub");
    if (g_keys < 1) g_keys = 1;
    if (g_chans < 1) g_chans = 1;
    pthread_barrier_init(&g_bar, NULL, (unsigned)g_threads + 1);
    Th *ths = calloc((size_t)g_threads, sizeof(Th)); pthread_t pt[64];
    for (int i = 0; i < g_threads; i++) {
        ths[i].tid = i;
        long share = g_conns / g_threads + (i < g_conns % g_threads);
        if (idle) ths[i].nidle = share; else ths[i].nclients = share;
        ths[i].nsubs = g_subs / g_threads + (i < g_subs % g_threads);
        pthread_create(&pt[i], NULL, thread_main, &ths[i]);
    }
    pthread_barrier_wait(&g_bar);                        // everyone is connected
    printf("READY conns=%ld subs=%ld\n", idle ? g_conns : g_conns + g_subs, g_subs); fflush(stdout);
    if (idle) {
        sleep((unsigned)(g_hold ? g_hold : 1));
        pthread_barrier_wait(&g_bar);
        printf("idle done\n"); return 0;
    }
    g_t_start = now_ns() + 500000000ull;                 // half a second of warm-up is not counted
    g_t_end = g_t_start + (u64)g_secs * 1000000000ull;
    pthread_barrier_wait(&g_bar);
    for (int i = 0; i < g_threads; i++) pthread_join(pt[i], NULL);
    u32 *bins = calloc(NBINS, sizeof(u32)), *dbins = calloc(NBINS, sizeof(u32));
    u64 over = 0, dover = 0, ops = 0, deliv = 0, errs = 0, maxlat = 0, dmax = 0, batches = 0;
    for (int i = 0; i < g_threads; i++) {
        for (int b = 0; b < NBINS; b++) { bins[b] += ths[i].bins[b]; dbins[b] += ths[i].dbins[b]; }
        over += ths[i].over; dover += ths[i].dover; ops += ths[i].nops; deliv += ths[i].ndeliv; errs += ths[i].errs;
        if (ths[i].maxlat > maxlat) maxlat = ths[i].maxlat;
        if (ths[i].dmax > dmax) dmax = ths[i].dmax;
        batches += ths[i].nbatches;
    }
    printf("mode=%s conns=%ld subs=%ld pipe=%d vsize=%d threads=%d: ops/s=%.0f batch_lat_us p50=%llu p99=%llu max=%llu", g_mode, g_conns, g_subs, g_pipe, g_vsize, g_threads,
           (double)ops / g_secs, (unsigned long long)pct(bins, over, 0.5), (unsigned long long)pct(bins, over, 0.99), (unsigned long long)(maxlat / 1000));
    if (!strcmp(g_mode, "pubsub"))
        printf(" deliveries/s=%.0f deliver_lat_us p50=%llu p99=%llu max=%llu", (double)deliv / g_secs, (unsigned long long)pct(dbins, dover, 0.5),
               (unsigned long long)pct(dbins, dover, 0.99), (unsigned long long)(dmax / 1000));
    printf(" errors=%llu\n", (unsigned long long)errs);
    return errs ? 1 : 0;
}
