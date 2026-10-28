// server.c - the HTTP server itself: the route table and simulated handlers, the two connection loops (sequential and pipelined), the
// listener hooks and main(). C counterpart of server.stra; everything generic lives in the libraries it includes:
//   runtime/  rt.h (tasks, pools, scheduler, timers)  rt_buf.h (size-class buffers)  rt_io.h (write_all, lingering close)  rt_net.h (workers, accept loop)
//   http/     http_msg.h (responses)  http_reader.h (request heads)  http_body.h (request bodies)
//
// How the Strata constructs are emitted here (by hand, in step with the source):
//   - exceptions with `throws` lists: error-return codes. Every fallible step reports through an `err` field in its frame (ReadReq, WriteAll) and
//     the caller tests it after the call; `try`/`catch` becomes a branch. A handler that throws is modelled by GET /boom: the handler frame sets
//     `err` and the caller answers 500 (runHandler's catch) and keeps the connection.
//   - tagged enums (Input, Cost, Ending): small integer tags plus the payload fields in the frame; `switch` is a C switch.
//   - `T?`: a pointer that may be NULL (buf, ring, hc), with the null checks written out.
//   - Task.completed(resp): an inline request appends straight to the output when the ring is empty (no Req allocated); error answers use
//     push_done(), a completed Req in the ring.
//   - head.copy(): a spawned child gets its own copy of the request head (HEADCOPY-byte block, or a size-class block when larger). The handler
//     frame reads the head AFTER waiting, so the copy is exercised: -DBREAK_COPY makes children read the connection's buffer instead.
//   - the pipelined connection (conc=1) is conc_step: collect the completed prefix, send it, admit requests (barriers for unsafe and unbounded
//     routes), read bodies inline, wait. -DBREAK_ORDER / -DBREAK_BARRIER / -DBREAK_FAIR / -DBREAK_LIMIT / -DBREAK_OVERREAD are its negative controls.
//
// usage: server port=8080 workers=4 cpu0=0 stats=1 [conc=1 depth=16 maxhead=16384 maxbody=1048576 uploadmax=4294967296 tmpdir=/var/tmp
//                                                    yield=65536 linger=2000 lingerbytes=268435456 sleep_us=0 work=0 ...]
#include "runtime/rt.h"
#include "runtime/rt_buf.h"
#include "runtime/rt_io.h"
#include "runtime/rt_net.h"
#include "http/http_msg.h"
#include "http/http_reader.h"
#include "http/http_body.h"
#include <x86intrin.h>
#include <sys/stat.h>


// Opt-in response batching (library level, off by default): -DBATCH_N=n holds up to n finished responses while the
// next request is already fully buffered, then sends them with one write. Responses are never held across a park or
// while waiting for input, but the first response of a batch waits for the handlers of the rest.
#ifndef BATCH_N
#define BATCH_N 0
#endif
static u32 g_work;                                   // simulated handler compute (loop iterations), 0 = instant
static u32 g_conc, g_depth = 16, g_sleep_ns, g_inline = 1;   // concurrent-pipelining mode, depth, handler sleep (ns), inline fast path
static u64 g_hold_ticks;                             // max hold time in TSC ticks (0 = no age limit)
static u32 g_slow_iters = 1250000;                   // handler compute for GET /slow (~1 ms)
static int g_classify = 1;                           // 1: /slow is declared 'unbounded' so held responses go out before it runs
static double g_tpu;                                 // TSC ticks per microsecond
static inline void spin_work(u32 n) {
    u64 x = 1;
    for (u32 i = 0; i < n; i++) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    __asm__ volatile("" :: "r"(x));
}


static inline int is_slow(const char *b, u32 n) { return n >= 14 && memcmp(b, "GET /slow ", 10) == 0; }
static inline int is_boom(const char *b, u32 n) { return n >= 14 && memcmp(b, "GET /boom ", 10) == 0; }     // a handler that throws
static inline int is_upload(const char *b, u32 n) { return n >= 14 && memcmp(b, "POST /upload ", 13) == 0; }   // the upload reader (file-backed, large)
static inline int is_apibody(const char *b, u32 n) { return n >= 14 && memcmp(b, "POST /api ", 10) == 0; }       // the standard reader (in memory, small)

// route(req) -> response; also decides keep-alive. Non-parking.
static const Resp *route(const char *b, u32 n, int *keep) {
    *keep = 0;
    u32 off = method_len(b, n);
    if (n < 14 || !off) return &r400;
    const char *p = b + off, *sp = memchr(p, ' ', n - off);
    if (!sp) return &r400;
    int http11 = (u32)(sp - b) + 9 <= n && memcmp(sp + 1, "HTTP/1.1", 8) == 0;
    int close, ka;
    conn_header(b, n, &close, &ka);
    *keep = http11 ? !close : ka;
    long plen = sp - p;
    if ((plen == 1 && p[0] == '/') || (plen == 5 && (memcmp(p, "/slow", 5) == 0 || memcmp(p, "/boom", 5) == 0)) || (off == 5 && ((plen == 7 && memcmp(p, "/upload", 7) == 0) || (plen == 4 && memcmp(p, "/api", 4) == 0))) ||
        (plen > 6 && memcmp(p, "/echo/", 6) == 0))
        return !*keep ? &r200_close : http11 ? &r200 : &r200_ka;
    *keep = 0;
    return &r404;
}

// ---------------------------------------------------------------- handle(conn) : keep-alive loop
typedef struct {
    u32 st;
    u32 start;                                 // unconsumed input is buf[start, end)
    char *buf; u8 bcls;                        // input buffer (a size class), allocated only while there is unconsumed input
    u32 end, req_end;
    u8 keep, linger;                           // linger: the server ends the connection, so hand it to the lingering closer
#if BATCH_N
    Buf *out; u32 outlen; u8 npend;            // held responses; only live while input is buffered, never across a read park
    u64 t_first;                               // TSC when the oldest held response was completed
#endif
#ifdef INJECT_FAIL_AT
    u32 ninj;                                  // test only: simulate the handler failing on request number INJECT_FAIL_AT
#endif
    union { ReadReq rr; WriteAll wa; } u;      // callees never live together: overlaid
} Handle;

static int handle_step(Task *t, Handle *f) {
    const Resp *resp;
    int r, keep;
    switch (f->st) {
    case 0:
    idle:
        if (!t->rd_ready) { f->st = 1; return park(t, W_RD); }      // wait_readable: parked, holds no buffer
    case 1:
        f->buf = cbuf_get(t->w, 0); f->bcls = 0; f->start = f->end = 0;
    parse:
        f->u.rr.st = 0;
    case 2:
        r = read_request_step(t, &f->u.rr, &f->buf, &f->bcls, &f->start, &f->end);
        if (r == PARKED) { f->st = 2; return PARKED; }
        if (f->u.rr.err) {
            if (f->u.rr.err != 1) goto finish;                     // EOF or an I/O error: nothing to answer
            f->keep = 0; f->linger = 2; f->u.wa.st = 0; f->u.wa.p = r431.p; f->u.wa.left = r431.n;   // header block too large: 431, then close (always linger)
            goto send;
        }
        f->req_end = f->u.rr.hdr_end;
#if BATCH_N
        if (f->out && ((g_classify && is_slow(f->buf + f->start, f->req_end - f->start)) ||
                       (g_hold_ticks && __rdtsc() - f->t_first >= g_hold_ticks))) {
            // held responses must not wait behind this handler: it is declared unbounded, or the oldest is too old
            f->u.wa.st = 0; f->u.wa.p = f->out->data; f->u.wa.left = f->outlen;
    case 4:
            r = write_all_step(t, &f->u.wa);
            if (r == PARKED) { f->st = 4; return PARKED; }
            buf_put(t->w, f->out); f->out = NULL; f->outlen = 0; f->npend = 0;
            if (f->u.wa.err) goto finish;
        }
#endif
        resp = route(f->buf + f->start, f->req_end - f->start, &keep);
        if (is_boom(f->buf + f->start, f->req_end - f->start)) resp = &r500;     // the handler threw: runHandler answers 500
        if (is_upload(f->buf + f->start, f->req_end - f->start) || is_apibody(f->buf + f->start, f->req_end - f->start)) { resp = &r501; keep = 0; f->linger = 2; }   // request bodies: conc=1 only
        f->keep = (u8)keep;
        if (g_work) spin_work(g_work);
        if (is_slow(f->buf + f->start, f->req_end - f->start)) spin_work(g_slow_iters);
#ifdef SLEEP
        if (g_sleep_ns) { timer_add(t->w, t, mono_ns() + g_sleep_ns); f->st = 5; return park(t, W_TIMER); }
    case 5:
        resp = route(f->buf + f->start, f->req_end - f->start, &keep);      // pure: recomputed after the sleep
        if (is_boom(f->buf + f->start, f->req_end - f->start)) resp = &r500;
        if (is_upload(f->buf + f->start, f->req_end - f->start) || is_apibody(f->buf + f->start, f->req_end - f->start)) { resp = &r501; keep = 0; f->linger = 2; }
        f->keep = (u8)keep;
#endif
#ifdef INJECT_FAIL_AT
        if (++f->ninj == INJECT_FAIL_AT) goto finish;
#endif
#if BATCH_N
        if (f->keep && f->npend + 1 < BATCH_N && (f->outlen + 2 * resp->n) < OUTSZ &&
            memmem(f->buf + f->req_end, f->end - f->req_end, "\r\n\r\n", 4) &&      // next request is already here
            !(g_hold_ticks && f->out && __rdtsc() - f->t_first >= g_hold_ticks)) {         // and the oldest held one is not too old
            if (!f->out) { f->out = buf_get(t->w); f->outlen = 0; f->t_first = __rdtsc(); }
            memcpy(f->out->data + f->outlen, resp->p, resp->n); f->outlen += resp->n; f->npend++;
            f->start = f->req_end;
            goto parse;
        }
        if (f->out) {                                              // flush the held responses together with this one
            memcpy(f->out->data + f->outlen, resp->p, resp->n); f->outlen += resp->n;
            f->u.wa.st = 0; f->u.wa.p = f->out->data; f->u.wa.left = f->outlen;
        } else
#endif
        { f->u.wa.st = 0; f->u.wa.p = resp->p; f->u.wa.left = resp->n; }
    send:
    case 3:
        r = write_all_step(t, &f->u.wa);
        if (r == PARKED) { f->st = 3; return PARKED; }
        CNT(t->w->nreq);
#if BATCH_N
        if (f->out) { buf_put(t->w, f->out); f->out = NULL; f->outlen = 0; f->npend = 0; }
#endif
        if (f->u.wa.err) goto finish;
        if (!f->keep) { if (!f->linger) f->linger = 1; goto finish; }                  // the server closes first: linger so the client can read the response
        f->start = f->req_end;                                     // consume the finished request
        if (f->start < f->end) {                                   // pipelined input is already buffered
#ifdef EAGER_MOVE
            memmove(f->buf, f->buf + f->start, f->end - f->start);
            f->end -= f->start; f->start = 0;
#endif
            goto parse;
        }
        cbuf_put(t->w, f->bcls, f->buf); f->buf = NULL; f->start = f->end = 0;
        goto idle;
    }
finish:
#if BATCH_N
    if (f->out) {                                // connection ends with responses still held: send them (best effort)
#ifndef NO_CLOSE_FLUSH                           // (NO_CLOSE_FLUSH exists only as a negative control for the test)
        (void)send(t->fd, f->out->data, f->outlen, MSG_NOSIGNAL | MSG_DONTWAIT);     // cleanup never parks
#endif
        buf_put(t->w, f->out); f->out = NULL;
    }
#endif
    if (f->buf) { cbuf_put(t->w, f->bcls, f->buf); f->buf = NULL; }
    return DONE;
}

typedef struct { Task t; Handle h; } ConnTask;


static int conn_resume(Task *t) {
    int r = handle_step(t, &((ConnTask *)t)->h);
    if (r == DONE) {
        Worker *w = t->w;
        if (g_linger_ms && ((ConnTask *)t)->h.linger && (((ConnTask *)t)->h.linger == 2 || sock_has_unread(t->fd))) start_linger(t); else close(t->fd);       // close() also removes the fd from epoll
        pool_free(&w->tasks, t);
        w->live--; CNT(w->nclose);
    }
    return r;
}


// ================================================================ concurrent pipelined requests (conc=1)
// RFC 7230 6.3.2: a server MAY process pipelined requests in parallel if all are safe methods, but MUST send the
// responses in request order. Here every pipelined request runs as its own child task; the connection task admits
// requests (up to `depth` in flight), collects the completed PREFIX in request order and sends it as one write. A
// non-safe request (POST) is a barrier: it is admitted only when nothing is in flight. When exactly one request is
// buffered and nothing is in flight it runs inline in the connection frame (no spawn). The connection waits for all its
// children before it finishes (structured), so a child never outlives the frame it reports to.
#define MAXPIPE 32

static int parse_echo(const char *b, u32 n, u32 *num, u32 *sleep_us) {   // GET /echo/N[?s=US]
    u32 off = method_len(b, n);
    if (!off || n < off + 7 || memcmp(b + off, "/echo/", 6) != 0) return 0;
    const char *p = b + off + 6, *e = b + n;
    u32 v = 0;
    while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (u32)(*p++ - '0');
    *num = v; *sleep_us = 0;
    if (p + 3 <= e && p[0] == '?' && p[1] == 's' && p[2] == '=') {
        u32 x = 0; p += 3;
        while (p < e && *p >= '0' && *p <= '9') x = x * 10 + (u32)(*p++ - '0');
        *sleep_us = x;
    }
    return 1;
}
// The handler's own frame. Like echo() in server.stra it reads the request AFTER it has waited, which is why a spawned child
// needs its own copy of the head: the connection's buffer is consumed, compacted and reused while the child sleeps.
typedef struct {
    u32 st, spin, sleep_ns, hlen, echo_n;
    const char *head;                        // the request head: a view (inline) or the child's own copy (spawned)
    u8 slow, boom, err, echo;                // err: the handler "threw" (server.stra: runHandler turns it into a 500)
} HFrame;
static int hstep(Task *t, HFrame *h) {
    switch (h->st) {
    case 0:
        if (h->spin) spin_work(h->spin);
        if (h->slow) spin_work(g_slow_iters);
        if (h->sleep_ns) { h->st = 1; timer_add(t->w, t, mono_ns() + h->sleep_ns); return park(t, W_TIMER); }
    case 1:
        if (h->echo) { u32 us; parse_echo(h->head, h->hlen, &h->echo_n, &us); }     // reads the head after the wait
        if (h->boom) h->err = 1;
        break;
    }
    return DONE;
}


// The upload handler's response: what was stored and where (server.stra: the handler reads Body.length(), isInMemory(), digest()).
static u32 upload_resp(char *dst, const Body *b, int keep) {
    char body[96];
    int bl = snprintf(body, sizeof body, "stored %llu bytes in %s crc32=%08x\n", (unsigned long long)b->total, b->upload ? "file" : "memory", b->crc);
    return (u32)snprintf(dst, 256, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n%s\r\n%s", bl,
                         keep ? "" : "Connection: close\r\n", body);
}

typedef struct CConn CConn;
#define HEADCOPY 256                         /* a spawned child's copy of the head: this size from the heads pool, larger from the buffer pool */
typedef struct { Task t; CConn *parent; const Resp *resp; Resp own; HFrame hf; char *hc; u8 hc_cls, keep, done; } Req;   // hc_cls: 0xFF = heads pool, else an input size class
struct CConn {
    Task t;
    u32 st, start, end;
    char *buf; u8 bcls;                        // input buffer (a size class), allocated only while there is unconsumed input
    u32 head, count;                         // ring of in-flight children, oldest first
    Req **ring;                              // allocated from a pool only while requests are in flight
    Buf *out; u32 outlen;                    // completed prefix being sent
    WriteAll wa;
    HFrame inl; const Resp *inl_resp;
    u8 closing, eof, broken, inl_active, inl_keep;
    u8 errclose;                             // the connection ends with an ERROR answer: always linger (the client is probably still sending)
    u8 unsafe_busy;                          // a non-safe request is in flight: admit nothing behind it until it has finished
    Body *body; u8 body_keep;                // the request body being read (allocated from a pool only while a body is in progress)
    u32 bbud;                                // body bytes this connection may still process before it yields
};

static u32 echo_resp(char *dst, u32 num, int keep) {
    char body[16];
    int bl = snprintf(body, sizeof body, "%u\n", num);
    return (u32)snprintf(dst, 200, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n%s\r\n%s", bl,
                         keep ? "" : "Connection: close\r\n", body);
}

static int req_resume(Task *t) {
    Req *r = (Req *)t;
    if (hstep(t, &r->hf) == PARKED) return PARKED;
    if (r->hf.err) { r->resp = &r500; r->hf.echo = 0; }     // the handler threw: answer 500
    r->done = 1;                              // the parent frees this block when it collects the response
    CConn *p = r->parent;
    if (p->t.parked && (p->t.wait & W_CHILD)) wake(t->w, &p->t);
    return DONE;
}

// Task.completed(resp): an entry that is already finished (a 431 here). Inline requests skip the ring when it is empty and
// append straight to the output, which is the same thing without allocating a Req.
static void push_done(CConn *f, Worker *w, const Resp *resp, int keep) {
    Req *rq = pool_alloc(&w->reqs);
    memset(rq, 0, sizeof *rq);
    rq->parent = f; rq->resp = resp; rq->keep = (u8)keep; rq->done = 1;
    f->errclose = 1;                          // push_done is only used for error answers (431, 413, 400, 500 from a failed body)
    if (!f->ring) { f->ring = pool_alloc(&w->rings); f->head = 0; }
    f->ring[(f->head + f->count) % g_depth] = rq; f->count++;
}

// A completed entry whose response was rendered into a heads-pool block (freed with the entry when it is collected).
static void push_dyn(CConn *f, Worker *w, char *blk, u32 n, int keep) {
    Req *rq = pool_alloc(&w->reqs);
    memset(rq, 0, sizeof *rq);
    rq->parent = f; rq->hc = blk; rq->hc_cls = 0xFF; rq->own.p = blk; rq->own.n = n; rq->resp = &rq->own; rq->keep = (u8)keep; rq->done = 1;
    if (!f->ring) { f->ring = pool_alloc(&w->rings); f->head = 0; }
    f->ring[(f->head + f->count) % g_depth] = rq; f->count++;
}

static int conc_step(Task *t, CConn *f) {
    Worker *w = t->w;
    int r;
    switch (f->st) { case 1: goto loop; case 2: goto wr; case 3: goto inl; case 4: goto body; default: break; }
loop:
    for (;;) {
        int progress = 0;
        // (a) collect the completed prefix, in request order
        while (f->count > 0) {
            u32 k = 0;
#ifdef BREAK_ORDER                           // negative control only: take any completed request, not just the oldest
            while (k < f->count && !f->ring[(f->head + k) % g_depth]->done) k++;
            if (k == f->count) break;
#else
            if (!f->ring[f->head]->done) break;
#endif
            Req *rq = f->ring[(f->head + k) % g_depth];
            char tmp[200]; const char *p; u32 n;
            if (rq->hf.echo) { n = echo_resp(tmp, rq->hf.echo_n, rq->keep); p = tmp; } else { p = rq->resp->p; n = rq->resp->n; }
            if (f->out && f->outlen + n > OUTSZ) break;                      // output buffer full: send what we have first
            if (!f->out) { f->out = buf_get(w); f->outlen = 0; }
            if (!f->broken) { memcpy(f->out->data + f->outlen, p, n); f->outlen += n; CNT(w->nreq); }
            for (u32 j = k; j > 0; j--) f->ring[(f->head + j) % g_depth] = f->ring[(f->head + j - 1) % g_depth];
            f->head = (f->head + 1) % g_depth; f->count--;
            if (rq->hc) { if (rq->hc_cls != 0xFF) cbuf_put(w, rq->hc_cls, rq->hc); else pool_free(&w->heads, rq->hc); }
            pool_free(&w->reqs, rq);
            progress = 1;
        }
        if (f->count == 0 && f->ring) { pool_free(&w->rings, f->ring); f->ring = NULL; }
        // (b) send it
        if (f->out && f->outlen == 0) { buf_put(w, f->out); f->out = NULL; }
        if (f->out) {
            f->wa.st = 0; f->wa.p = f->out->data; f->wa.left = f->outlen;
    wr:
            r = write_all_step(t, &f->wa);
            if (r == PARKED) { f->st = 2; return PARKED; }
            buf_put(w, f->out); f->out = NULL; f->outlen = 0;
            if (f->wa.err) { f->broken = 1; f->closing = 1; }
            progress = 1;
        }
        // (c) admit new requests
        if (f->count == 0) f->unsafe_busy = 0;
        while (!f->closing && !f->broken && f->count < g_depth && !f->inl_active && !f->unsafe_busy) {
            char *e = (f->buf && f->end > f->start) ? memmem(f->buf + f->start, f->end - f->start, "\r\n\r\n", 4) : NULL;
            if (!e) {
                if (f->eof) break;
                if (!f->buf) { if (!t->rd_ready) break; f->buf = cbuf_get(w, 0); f->bcls = 0; f->start = f->end = 0; }
                if (!ibuf_make_room(w, &f->buf, &f->bcls, &f->start, &f->end)) {         // header block larger than the largest class
                    push_done(f, w, &r431, 0); f->closing = 1; progress = 1; break;      // 431, then close
                }
                u32 space = g_cls_size[f->bcls] - f->end;
                ssize_t n = recv(t->fd, f->buf + f->end, space, 0);
                if (n > 0) { f->end += (u32)n; if ((u32)n < space) t->rd_ready = 0; progress = 1; continue; }
                if (n == 0) { f->eof = 1; break; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN) { t->rd_ready = 0; break; }
                f->eof = 1; f->broken = 1; f->closing = 1; break;
            }
            u32 hdr_end = (u32)(e + 4 - f->buf);
            const char *hd = f->buf + f->start;
            u32 hn = hdr_end - f->start;
#ifndef BREAK_BARRIER                         // negative control only: let unsafe requests overlap earlier ones
            // barrier: earlier requests finish and are sent first. Applies to unsafe requests (POST), and to routes declared
            // unbounded (classify=1): a long handler cannot be interrupted, so what is already complete goes out before it starts.
            if ((is_post(hd, hn) || (g_classify && is_slow(hd, hn))) && f->count > 0) break;
#endif
            int keep;
            const Resp *resp = route(hd, hn, &keep);
            u32 en = 0, es = 0;
            int echo = parse_echo(hd, hn, &en, &es);
            HFrame hf = { .spin = g_work, .sleep_ns = es ? es * 1000u : g_sleep_ns, .hlen = hn, .head = hd,
                          .slow = (u8)is_slow(hd, hn), .boom = (u8)is_boom(hd, hn), .echo = (u8)echo };
            f->start = hdr_end;
            if (!keep) f->closing = 1;
#ifndef BREAK_BARRIER
            if (is_post(hd, hn)) f->unsafe_busy = 1;
#endif
            progress = 1;
            if (is_upload(hd, hn) || is_apibody(hd, hn)) {
                // A request with a body. It is unsafe, so the barrier above guarantees nothing else is in flight: this connection task
                // reads the body itself (parking while it waits for the client), then answers.
                int up = is_upload(hd, hn);                                                            // which reader: upload (file) or standard (memory)
                u64 limit = up ? g_uploadmax : g_maxbody;
                u64 cl;
                int fr = body_framing(hd, hn, &cl);
                if (fr <= 0) { push_done(f, w, &r400, 0); f->closing = 1; break; }                 // no framing, or malformed
#ifndef BREAK_LIMIT
                if (fr == 1 && cl > limit) { push_done(f, w, &r413, 0); f->closing = 1; break; }       // refused before reading any body byte
#endif
                if (f->end == f->start && expects_100(hd, hn))                                       // best effort: the client is waiting for it
                    (void)send(t->fd, "HTTP/1.1 100 Continue\r\n\r\n", 25, MSG_NOSIGNAL | MSG_DONTWAIT);
                f->body = pool_alloc(&w->bodies);
                memset(f->body, 0, sizeof *f->body);
                f->body->fd = -1; f->body->mode = (u8)fr; f->body->remaining = cl; f->body->upload = (u8)up; f->body->limit = limit;
                if (!up && fr == 1 && cl) { f->body->mem = malloc(cl); f->body->mcap = cl; }              // the standard reader knows the size up front
                if (!up && fr == 1 && cl && !f->body->mem) { push_done(f, w, &r500, 0); f->closing = 1; body_free(w, f->body); f->body = NULL; break; }
                if (fr == 1 && cl == 0) f->body->done = 1;
                f->body_keep = (u8)keep;
                ibuf_upgrade(w, &f->buf, &f->bcls, &f->start, &f->end, 2);
    body:
                f->bbud = g_body_budget;                          // a fresh budget each turn, so one upload cannot monopolise the worker
                for (;;) {
                    u32 avail = f->end - f->start;
                    if (avail && !f->body->done && !f->body->err) {
                        u32 used = body_feed(w, f->body, f->buf + f->start, avail);
                        f->start += used;
                        f->bbud = used >= f->bbud ? 0 : f->bbud - used;
                    }
#ifdef BREAK_OVERREAD                                     /* negative control only: swallow input that belongs to the next request */
                    if (f->body->done) f->start = f->end;
#endif
                    if (f->body->done || f->body->err) break;
#ifndef BREAK_FAIR                                        /* negative control only: never yield (the upload starves the worker's other connections) */
                    if (f->bbud == 0) { f->st = 4; return yield_now(t); }                           // go to the back of the run queue; resume at `body:`
#endif
                    if (f->start < f->end) continue;                                                // buffered input left: keep feeding it
                    f->start = f->end = 0;                                                          // everything buffered was consumed
                    ssize_t n = recv(t->fd, f->buf, g_cls_size[f->bcls], 0);
                    if (n > 0) { f->end = (u32)n; if ((u32)n < g_cls_size[f->bcls]) t->rd_ready = 0; continue; }
                    if (n == 0) { f->body->err = 3; f->eof = 1; break; }
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) { t->rd_ready = 0; f->st = 4; return park(t, W_RD); }
                    f->body->err = 3; f->broken = 1; break;
                }
                {
                    Body *b = f->body;
                    if (!b->err && b->upload && b->fd >= 0) { struct stat sb; if (fstat(b->fd, &sb) < 0 || (u64)sb.st_size != b->total) b->err = 4; }   // the spool file must hold exactly what was sent
                    if (!b->err) {
                        char *blk = pool_alloc(&w->heads);
                        push_dyn(f, w, blk, upload_resp(blk, b, f->body_keep), f->body_keep);
                    } else {
                        f->closing = 1;                                                              // the rest of the body is unread: the connection cannot continue
                        if (b->err == 1) push_done(f, w, &r413, 0);
                        else if (b->err == 2) push_done(f, w, &r400, 0);
                        else if (b->err == 4) push_done(f, w, &r500, 0);
                    }
                    body_free(w, b); f->body = NULL;
                }
                continue;
            }
            if (g_inline && f->count == 0 &&
                !(f->end > f->start && memmem(f->buf + f->start, f->end - f->start, "\r\n\r\n", 4))) {
                // the only request in flight and nothing pipelined behind it: run it here, no spawn
                f->inl = hf; f->inl_resp = resp; f->inl_keep = (u8)keep; f->inl_active = 1;      // inline: the head is a view, the handler finishes first
    inl:
                r = hstep(t, &f->inl);
                if (r == PARKED) { f->st = 3; return PARKED; }
                f->inl_active = 0;
                if (f->inl.err) { f->inl_resp = &r500; f->inl.echo = 0; }
                {
                    char tmp[200]; const char *p; u32 n;
                    if (f->inl.echo) { n = echo_resp(tmp, f->inl.echo_n, f->inl_keep); p = tmp; } else { p = f->inl_resp->p; n = f->inl_resp->n; }
                    if (!f->out) { f->out = buf_get(w); f->outlen = 0; }
                    memcpy(f->out->data + f->outlen, p, n); f->outlen += n; CNT(w->nreq);
                }
                continue;
            }
            Req *rq = pool_alloc(&w->reqs);
            memset(rq, 0, sizeof *rq);
            rq->t.resume = req_resume; rq->t.w = w; rq->t.fd = -1;
            rq->parent = f; rq->resp = resp; rq->hf = hf; rq->keep = (u8)keep;
#ifndef BREAK_COPY                            /* negative control only: the child reads the connection's buffer instead of its own copy */
            rq->hc_cls = hn > HEADCOPY ? (u8)cls_for(hn) : 0xFF;       // the child owns a copy of the head (head.copy() in server.stra)
            rq->hc = rq->hc_cls != 0xFF ? cbuf_get(w, rq->hc_cls) : (char *)pool_alloc(&w->heads);
            memcpy(rq->hc, hd, hn);
            rq->hf.head = rq->hc;
#endif
            if (!f->ring) { f->ring = pool_alloc(&w->rings); f->head = 0; }
            f->ring[(f->head + f->count) % g_depth] = rq; f->count++;
            enqueue(w, &rq->t);
        }
        if (f->buf && f->start == f->end) { cbuf_put(w, f->bcls, f->buf); f->buf = NULL; f->start = f->end = 0; }   // idle: no buffer
        if (progress) continue;
        // (d) finished, or wait for input and/or a child
        {
            int pending_req = f->buf && f->end > f->start && memmem(f->buf + f->start, f->end - f->start, "\r\n\r\n", 4);
            if (f->count == 0 && !f->inl_active && !f->out && (f->closing || f->broken || (f->eof && !pending_req))) goto done;
            u8 mask = 0;
            if (f->count > 0 || f->inl_active) mask |= W_CHILD;
            if (!f->closing && !f->eof && !f->broken) mask |= W_RD;
            if (!mask) mask = W_RD;
            f->st = 1;
            return park(t, mask);
        }
    }
done:
    if (f->body) { body_free(w, f->body); f->body = NULL; }
    if (f->out) { buf_put(w, f->out); f->out = NULL; }
    if (f->buf) { cbuf_put(w, f->bcls, f->buf); f->buf = NULL; }
    if (f->ring) { pool_free(&w->rings, f->ring); f->ring = NULL; }
    return DONE;
}

static int conc_resume(Task *t) {
    int r = conc_step(t, (CConn *)t);
    if (r == DONE) {
        Worker *w = t->w;
        CConn *f = (CConn *)t;
        if (g_linger_ms && f->closing && !f->eof && !f->broken && (f->errclose || sock_has_unread(t->fd))) start_linger(t); else close(t->fd);   // the server ended it while the client may still be sending
        pool_free(&w->tasks, t);
        w->live--; CNT(w->nclose);
    }
    return r;
}

// ---------------------------------------------------------------- accept loop : spawn handle(conn)
static void spawn_conn(Worker *w, int fd) {
    Task *c = pool_alloc(&w->tasks);
    memset(c, 0, g_conc ? sizeof(CConn) : sizeof(ConnTask));
    c->resume = g_conc ? conc_resume : conn_resume; c->w = w; c->fd = fd;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = c };
    if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &ev) < 0) { perror("epoll_ctl add"); exit(1); }
    w->live++; CNT(w->nacc);
    enqueue(w, c);
}

// ---------------------------------------------------------------- main
static int g_stats = 0;

// The application's per-worker setup (the hook rt_worker_init): the pools for its own types.
static void app_worker_init(Worker *w) {
    pool_init(&w->tasks, g_conc ? sizeof(CConn) : sizeof(ConnTask));
    pool_init(&w->reqs, sizeof(Req));
    pool_init(&w->rings, MAXPIPE * sizeof(Req *));
    pool_init(&w->heads, HEADCOPY);
    pool_init(&w->bodies, sizeof(Body));
    pool_init(&w->lingers, sizeof(Linger));
    pool_init(&w->bufs, sizeof(Buf));                      // output buffers
    for (int i = 0; i < NCLS; i++) pool_init(&w->cls[i], g_cls_size[i]);
}

int main(int argc, char **argv) {
    rt_port = atoi(arg(argc, argv, "port", "8080"));
    rt_nworkers = atoi(arg(argc, argv, "workers", "4"));
    rt_cpu0 = atoi(arg(argc, argv, "cpu0", "0"));
    g_stats = atoi(arg(argc, argv, "stats", "0"));
    g_work = (u32)atoi(arg(argc, argv, "work", "0"));
    g_slow_iters = (u32)atoi(arg(argc, argv, "slow", "1250000"));
    {   // maxhead=N: the largest input class a header block may grow to (the largest class whose size is <= N)
        u32 mh = (u32)atoi(arg(argc, argv, "maxhead", "16384"));
        g_maxcls = 0;
        for (int i = 0; i < NCLS; i++) if (g_cls_size[i] <= mh) g_maxcls = i;
    }
    g_classify = atoi(arg(argc, argv, "classify", "1"));
    g_conc = (u32)atoi(arg(argc, argv, "conc", "0"));
    g_depth = (u32)atoi(arg(argc, argv, "depth", "16")); if (g_depth > MAXPIPE) g_depth = MAXPIPE; if (!g_depth) g_depth = 1;
    g_sleep_ns = (u32)atoi(arg(argc, argv, "sleep_us", "0")) * 1000u;
    g_inline = (u32)atoi(arg(argc, argv, "inline", "1"));
    g_maxbody = strtoull(arg(argc, argv, "maxbody", "1048576"), NULL, 10);
    if (g_maxbody > (1ull << 30)) g_maxbody = 1ull << 30;                          // the standard reader holds the whole body in memory
    g_uploadmax = strtoull(arg(argc, argv, "uploadmax", "4294967296"), NULL, 10);
    g_tmpdir = arg(argc, argv, "tmpdir", "/var/tmp");
    g_linger_ms = (u32)atoi(arg(argc, argv, "linger", "2000"));
    g_linger_bytes = strtoull(arg(argc, argv, "lingerbytes", "268435456"), NULL, 10);
    g_body_budget = (u32)atoi(arg(argc, argv, "yield", arg(argc, argv, "bodybudget", "65536")));      // yield= (bodybudget= is the old name)
    crc_init();
    {   // calibrate the TSC against the monotonic clock
        double t0 = now_s(); u64 c0 = __rdtsc(); usleep(30000); u64 c1 = __rdtsc(); double t1 = now_s();
        g_tpu = (c1 - c0) / ((t1 - t0) * 1e6);
        g_hold_ticks = (u64)(atof(arg(argc, argv, "hold_us", "0")) * g_tpu);
    }
    init_responses();
    signal(SIGPIPE, SIG_IGN);
    rt_on_accept = spawn_conn; rt_worker_init = app_worker_init;
    rt_start_workers();
    fprintf(stderr, "server: port=%d workers=%d conc=%u depth=%u sizeof(ConnTask)=%zu sizeof(CConn)=%zu sizeof(Req)=%zu out_buf=%zu in_classes=512/2K/8K/16K maxhead=%u\n", rt_port, rt_nworkers, g_conc, g_depth, sizeof(ConnTask), sizeof(CConn), sizeof(Req), sizeof(Buf), g_cls_size[g_maxcls]);
    u64 last_req = 0; double last_t = now_s();
    for (;;) {
        sleep(1);
        if (!g_stats) continue;
        u64 live = 0, req = 0, acc = 0, tb = 0, bb = 0;
        for (int i = 0; i < rt_nworkers; i++) {
            live += LOAD(rt_workers[i].live); req += LOAD(rt_workers[i].nreq); acc += LOAD(rt_workers[i].nacc);
            tb += LOAD(rt_workers[i].tasks.carved); bb += LOAD(rt_workers[i].bufs.carved); for (int c = 0; c < NCLS; c++) bb += LOAD(rt_workers[i].cls[c].carved);
        }
        double t = now_s();
        printf("live=%lu accepted=%lu rps=%.0f rss=%ld MB task_arena=%lu KB buf_arena=%lu KB\n", (unsigned long)live,
               (unsigned long)acc, (req - last_req) / (t - last_t), rss_kb() / 1024, (unsigned long)(tb >> 10), (unsigned long)(bb >> 10));
        fflush(stdout);
        last_req = req; last_t = t;
    }
}
