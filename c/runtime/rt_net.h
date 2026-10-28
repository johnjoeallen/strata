#ifndef RT_NET_H
#define RT_NET_H
// runtime/rt_net.h - the thread-per-core skeleton (C runtime): one worker per core, each with its own epoll, run queue, arenas and SO_REUSEPORT listener, and an
// accept loop that hands every new socket to the application. The application supplies two hooks before rt_start_workers():
//   rt_worker_init(w)    per-worker setup: the pools for its own types
//   rt_on_accept(w, fd)  a connection arrived: create its task and enqueue it
#include "rt.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

static int rt_port = 8080, rt_cpu0 = 0, rt_nworkers = 4;
static Worker *rt_workers;
static void (*rt_on_accept)(Worker *w, int fd);
static void (*rt_worker_init)(Worker *w);

static int accept_resume(Task *t) {
    for (int i = 0; i < 128; i++) {
        int fd = accept4(t->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EAGAIN) { t->rd_ready = 0; return park(t, W_RD); }
            if (errno == EINTR || errno == ECONNABORTED) continue;
            fprintf(stderr, "accept4: %s (worker %d, live=%lu)\n", strerror(errno), t->w->id, (unsigned long)t->w->live);
            exit(1);
        }
        rt_on_accept(t->w, fd);
    }
    return yield_now(t);                    // let ready tasks run, then keep draining the backlog
}

static void *rt_worker_main(void *arg) {
    Worker *w = arg;
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(rt_cpu0 + w->id, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    w->ep = epoll_create1(EPOLL_CLOEXEC);
    if (rt_worker_init) rt_worker_init(w);

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(rt_port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); exit(1); }
    if (listen(lfd, 4096) < 0) { perror("listen"); exit(1); }

    Task *acc = calloc(1, sizeof *acc);
    acc->resume = accept_resume; acc->w = w; acc->fd = lfd;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = acc };
    epoll_ctl(w->ep, EPOLL_CTL_ADD, lfd, &ev);
    enqueue(w, acc);
    worker_loop(w);
    return NULL;
}

static void rt_start_workers(void) {
    rt_workers = calloc(rt_nworkers, sizeof(Worker));
    pthread_t th[64];
    for (int i = 0; i < rt_nworkers; i++) { rt_workers[i].id = i; pthread_create(&th[i], NULL, rt_worker_main, &rt_workers[i]); }
}

static const char *arg(int argc, char **argv, const char *key, const char *def) {
    size_t k = strlen(key);
    for (int i = 1; i < argc; i++) if (!strncmp(argv[i], key, k) && argv[i][k] == '=') return argv[i] + k + 1;
    return def;
}

#endif
