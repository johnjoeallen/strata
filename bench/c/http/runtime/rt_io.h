#ifndef RT_IO_H
#define RT_IO_H
// runtime/rt_io.h - socket I/O for tasks (C runtime; the Strata side is the leaves in runtime/leaves.stra and `writeAll` in runtime/io.stra).
//   * write_all: send everything, parking on EAGAIN (W_WR)
//   * lingering close: the connection a SERVER ended while the client may still be sending is handed to a small task that half-closes, then reads and
//     discards input (bounded by time and bytes, yielding like a body reader) before closing, so the client can read the response instead of getting a reset.
#include "rt.h"
#include <sys/ioctl.h>

static u32 g_linger_ms = 2000;                       // linger=      after the server closes first, keep reading and discarding for at most this long (0 = off)
static u64 g_linger_bytes = 256ull << 20;            // lingerbytes= ... and at most this many bytes
static u32 g_body_budget = 65536;                    // yield=       bytes of body one connection processes per turn before it yields to the others

// ---------------------------------------------------------------- write_all(conn, bytes) : may park
typedef struct { u32 st; const char *p; u32 left; u8 err; } WriteAll;
static int write_all_step(Task *t, WriteAll *f) {
    switch (f->st) {
    case 0:
        while (f->left) {
            ssize_t n = send(t->fd, f->p, f->left, MSG_NOSIGNAL);
            if (n > 0) { f->p += n; f->left -= (u32)n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) { f->st = 1; return park(t, W_WR); }
            f->err = 1; return DONE;
        }
        f->err = 0; return DONE;
    case 1:
        f->st = 0;
        return write_all_step(t, f);
    }
    return DONE;
}

// ================================================================ lingering close
// When the SERVER decides to end a connection (an error answer such as 413, 400 or 431, or `Connection: close`) the client may still be sending. Closing
// a socket that holds unread input makes the kernel send a RESET, which can destroy the response before the client reads it, or make the client's
// next write fail so that it never looks for an answer. So the connection is handed to a small lingering task instead: it half-closes the write side
// (the client sees the response and a clean end), then keeps reading and discarding input until the client closes, `linger=` milliseconds pass, or
// `lingerbytes=` bytes have been discarded, and only then closes. It yields like a body reader, so a hostile client cannot starve the worker, and its
// timeout is cancelled when it finishes early (an entry left in the timer heap could wake a recycled task slot).
typedef struct { Task t; u64 until, left; u32 hidx; } Linger;

static int linger_resume(Task *t) {
    Linger *l = (Linger *)t;
    Worker *w = t->w;
    char scratch[16384];
    u32 budget = g_body_budget ? g_body_budget : 65536;
    int finished = mono_ns() >= l->until;                  // the deadline passed (the timer woke us)
    while (!finished) {
        ssize_t n = recv(t->fd, scratch, sizeof scratch, 0);
        if (n > 0) {
            if ((u64)n >= l->left) { finished = 1; break; }   // it keeps sending: give up (the client gets a reset)
            l->left -= (u64)n;
            if ((u32)n < sizeof scratch) t->rd_ready = 0;
            if ((u32)n >= budget) return yield_now(t);         // fairness: go to the back of the run queue
            budget -= (u32)n;
            continue;
        }
        if (n == 0) { finished = 1; break; }                  // the client closed: the response was delivered
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            t->rd_ready = 0;
            if (l->hidx == TIMER_NONE) timer_add_c(w, t, l->until, &l->hidx);     // wake at the deadline
            return park(t, W_RD | W_TIMER);
        }
        finished = 1;                                         // reset or another error
    }
    timer_cancel(w, &l->hidx);
    close(t->fd);
    pool_free(&w->lingers, l);
    return DONE;
}

// When does the server linger? Policy (documented, not a heuristic hidden in the kernel): always after an ERROR answer (413, 400, 431, 501), because the
// client is probably still sending; after a plain `Connection: close` only if the socket still holds unread input, because only then (or if input arrives
// later) would closing send a reset. A client that has sent everything and waits, like `ab` without keep-alive, is closed directly and pays nothing.
static inline int sock_has_unread(int fd) { int n = 0; return ioctl(fd, FIONREAD, &n) == 0 && n > 0; }

// Takes over the connection of a finished task `old` (its frame is freed by the caller).
static void start_linger(Task *old) {
    Worker *w = old->w;
    int fd = old->fd;
    shutdown(fd, SHUT_WR);                                    // the response is out: send FIN after it
    Linger *l = pool_alloc(&w->lingers);
    memset(l, 0, sizeof *l);
    l->t.resume = linger_resume; l->t.w = w; l->t.fd = fd; l->t.rd_ready = 1;
    l->until = mono_ns() + (u64)g_linger_ms * 1000000ull; l->left = g_linger_bytes; l->hidx = TIMER_NONE;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = l };
    if (epoll_ctl(w->ep, EPOLL_CTL_MOD, fd, &ev) < 0) { close(fd); pool_free(&w->lingers, l); return; }   // re-point the event at the new task
    enqueue(w, &l->t);
}

#endif
