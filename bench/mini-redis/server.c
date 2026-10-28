// server.c - the mini-redis server itself, as the C a Strata compiler would emit for server.stra: the accept hook, the per-connection state machine
// (commandLoop, subscribeLoop), the purge task, shutdown, main. Everything else is in runtime/ (rt_sync.h) and redis/ (db.h, resp.h, command.h), on top of
// the HTTP runtime (../c/http/runtime/rt*.h) which is used unchanged.
//
// A connection is one task: a compiler-sized frame plus a header. Each `park` point of the Strata source is a state here; on resume the state re-checks its
// condition (a wake is a hint), so spurious wakes are harmless.
//   ST_CMD     read the next frame and start its command            (waitAny(readable, shutdown))
//   ST_DB      take the database lock (parks while another worker holds it), run the command, release
//   ST_FLUSH   send the reply, parking while the socket is full       (writer.flush())
//   ST_SUB     subscribed: deliver messages, read SUBSCRIBE/UNSUBSCRIBE (waitAny(readable, mailbox, shutdown))
//   ST_SUBCMD  one channel of a SUBSCRIBE/UNSUBSCRIBE at a time, each taking the lock and answering
#include "rt.h"
#include "rt_buf.h"
#include "rt_io.h"
#include "rt_net.h"
#include "redis/command.h"

#define PER_TURN 64                      // commands one connection runs, or messages it delivers, before it yields; stated, not tuned
enum { ST_CMD, ST_DB, ST_FLUSH, ST_AFTER, ST_SUB, ST_SUBCMD, ST_END, ST_LINGER };
enum { NF_FRAME, NF_IDLE, NF_CLOSED, NF_IOERR, NF_PROTO, NF_TOOBIG };

typedef struct ConnTask {
    Task t;
    struct ConnTask *lprev, *lnext;      // the worker's list of connections (shutdown wakes them all)
    u8 st, after, waiting, sub, icls;
    char *ib; u32 istart, iend, flen;    // input buffer: unconsumed bytes are ib[istart..iend)
    Args *args; Scanner sc;
    const char *errmsg;
    Out out;
    Cmd cmd;
    LockWait lw;
    Value *k, *v;                        // allocated before the lock is taken (SET key and value, PUBLISH message)
    Mailbox *mb; Value **names; u32 nnames, ncap, ci, sent, burst;
} ConnTask;

static Limiter g_conns;
static ConnTask *g_clist[MAXW];
static int g_stats = 0;

// ---------------------------------------------------------------- input buffer (Strata: Reader.buf, a runtime growable u8[])
// Size classes 512 B .. 16 KB come from the worker's pools (rt_buf.h); larger ones, up to maxFrame, are malloc'd (x4 each step).
static inline u32 icap(u8 cls) { return cls < NCLS ? g_cls_size[cls] : 16384u << (2 * (cls - (NCLS - 1))); }
static char *ib_alloc(Worker *w, u8 cls) { return cls < NCLS ? cbuf_get(w, cls) : malloc(icap(cls)); }
static void ib_free(Worker *w, u8 cls, char *p) { if (cls < NCLS) cbuf_put(w, cls, p); else free(p); }

// Room for `needed` bytes counted from istart: compact, then grow class by class. 0: over maxFrame.
static int ib_room(ConnTask *c, u32 needed) {
    Worker *w = c->t.w;
    if (needed > L.maxFrame) return 0;
    if (c->istart > 0 && (c->iend == icap(c->icls) || needed > icap(c->icls))) {
        memmove(c->ib, c->ib + c->istart, c->iend - c->istart);
        c->iend -= c->istart; c->istart = 0;
    }
    while (icap(c->icls) < needed) {
        u8 nc = (u8)(c->icls + 1);
        char *nb = ib_alloc(w, nc);
        memcpy(nb, c->ib, c->iend);
        ib_free(w, c->icls, c->ib);
        c->ib = nb; c->icls = nc;
    }
    return 1;
}

// The next complete frame, if one can be had without waiting (Strata: Reader.tryNextFrame). The buffer exists only while there is unconsumed input.
static int next_frame(ConnTask *c) {
    Worker *w = c->t.w;
    for (;;) {
        if (c->ib && c->iend > c->istart) {
            if (!c->args) c->args = pool_alloc(&w->reqs);
            u32 n = 0; const char *msg = NULL;
            int r = scan_advance(&c->sc, c->ib + c->istart, c->iend - c->istart, c->args, &n, &msg);
            if (r == SC_DONE) { c->flen = n; return NF_FRAME; }
            if (r == SC_PROTO) { c->errmsg = msg; return NF_PROTO; }
            if (r == SC_TOOBIG) return NF_TOOBIG;
            if (!ib_room(c, n)) return NF_TOOBIG;
            if (!c->t.rd_ready) return NF_IDLE;                   // everything the kernel had is already here
        } else {
            if (!c->t.rd_ready) {
                if (c->ib) { ib_free(w, c->icls, c->ib); c->ib = NULL; c->istart = c->iend = 0; }
                return NF_IDLE;                                    // idle: hold no buffer
            }
            if (!c->ib) { c->ib = cbuf_get(w, 0); c->icls = 0; c->istart = c->iend = 0; }
        }
        u32 space = icap(c->icls) - c->iend;
        ssize_t k = recv(c->t.fd, c->ib + c->iend, space, 0);
        if (k > 0) { c->iend += (u32)k; if ((u32)k < space) c->t.rd_ready = 0; continue; }
        if (k == 0) return c->iend > c->istart ? NF_IOERR : NF_CLOSED;     // EOF in the middle of a frame is an error
        if (errno == EINTR) continue;
        if (errno == EAGAIN) { c->t.rd_ready = 0; continue; }
        return NF_IOERR;
    }
}

// The frame is handled: drop its bytes (no copy). Every argument view is dead after this.
static void finish(ConnTask *c) {
    Worker *w = c->t.w;
    c->istart += c->flen;
    memset(&c->sc, 0, sizeof c->sc);
    if (c->args) { pool_free(&w->reqs, c->args); c->args = NULL; }
    if (c->istart == c->iend && c->ib) { ib_free(w, c->icls, c->ib); c->ib = NULL; c->istart = c->iend = 0; }
}

// ---------------------------------------------------------------- the lock (Strata: sync/mutate; entering parks)
static inline int lock_enter(LockWait *lw, u8 *waiting, Task *t, int write) {   // 1: held. 0: park (W_SIG) and call again on resume
    if (*waiting) { if (!__atomic_load_n(&lw->granted, __ATOMIC_ACQUIRE)) return 0; *waiting = 0; return 1; }
    if (rw_acquire(&g_db.lock, lw, t, write)) return 1;
    *waiting = 1;
    return 0;
}

// ---------------------------------------------------------------- ending a connection (Strata: scope exit + cleanup())
static int conn_end(ConnTask *c, int linger) {
    Worker *w = c->t.w;
    if (c->mb) {
        for (u32 i = 0; i < c->nnames; i++) {
#ifndef BREAK_REAP                                           /* negative control: forget to hand the subscriptions to the reaper */
            db_release(c->names[i], c->mb);                   // cleanup(): never parks, never locks
#endif
            value_unref(c->names[i]);
        }
        free(c->names);
        c->mb->owner = NULL;                                  // from now on a wake for this mailbox finds no task
        mb_unref(c->mb);
    }
    if (c->k) value_unref(c->k);
    if (c->v) value_unref(c->v);
    out_reset(&c->out, w);
    if (c->args) pool_free(&w->reqs, c->args);
    if (c->ib) ib_free(w, c->icls, c->ib);
    if (c->lprev) c->lprev->lnext = c->lnext; else g_clist[w->id] = c->lnext;
    if (c->lnext) c->lnext->lprev = c->lprev;
    if (linger) start_linger(&c->t); else close(c->t.fd);     // lingering close takes over the fd (the client may still be sending)
    LIVE_ADD(w, -1);
    lim_release(&g_conns);
    pool_free(&w->tasks, c);
    return DONE;
}

static inline void begin(ConnTask *c, u8 after) { c->after = after; c->st = ST_FLUSH; }
// Protocol or frame error: answer "-ERR <msg>", then close with lingering (the client may still be sending).
static void fail(ConnTask *c, const char *msg) {
    Worker *w = c->t.w;
    out_reset(&c->out, w);
    out_str(&c->out, w, "-ERR "); out_str(&c->out, w, msg); out_str(&c->out, w, "\r\n");
    begin(c, ST_LINGER);
}

static int name_index(ConnTask *c, const char *p, u32 n) {
    for (u32 i = 0; i < c->nnames; i++) if (c->names[i]->len == n && !memcmp(c->names[i]->d, p, n)) return (int)i;
    return -1;
}
static void reply_sub(ConnTask *c, const char *verb, const char *nm, u32 nl, u32 count) {
    Worker *w = c->t.w;
    out_array(&c->out, w, 3); out_bulk(&c->out, w, verb, (u32)strlen(verb), NULL); out_bulk(&c->out, w, nm, nl, NULL); out_int(&c->out, w, count);
}
static void reply_unknown(ConnTask *c, const char *name, u32 nl) {
    char t[256]; u32 n = nl < 200 ? nl : 200;
    int len = snprintf(t, sizeof t, "ERR unknown command '");
    for (u32 i = 0; i < n; i++) t[len++] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    t[len++] = '\''; t[len] = 0;
    out_error(&c->out, c->t.w, t);
}
static const char *const kind_name[] = { "get", "set", "publish", "subscribe", "unsubscribe", "ping" };

// ---------------------------------------------------------------- the connection task
static int conn_resume(Task *t) {
    ConnTask *c = (ConnTask *)t;
    Worker *w = t->w;
    Out *o = &c->out;
    for (;;) switch (c->st) {

    case ST_CMD: {                                            // commandLoop
        if (SHUTTING()) return conn_end(c, 0);
        int r = next_frame(c);
        if (r == NF_IDLE) return park(t, W_RD);               // shutdown reaches us through the worker's connection list
        if (r == NF_CLOSED || r == NF_IOERR) return conn_end(c, 0);
        if (r == NF_PROTO) { fail(c, c->errmsg); continue; }
        if (r == NF_TOOBIG) { fail(c, "frame too large"); continue; }
        const char *err;
        if (parse_cmd(c->ib + c->istart, c->args, &c->cmd, &err) < 0) { fail(c, err); continue; }
        CNT(w->nreq);
        Cmd *m = &c->cmd;
        switch (m->kind) {
        case C_PING:
            if (m->has_msg) out_bulk(o, w, m->val, m->vlen, NULL); else out_simple(o, w, "PONG");
            begin(c, ST_AFTER); continue;
        case C_UNKNOWN: reply_unknown(c, m->key, m->klen); begin(c, ST_AFTER); continue;
        case C_UNSUBSCRIBE: out_error(o, w, "ERR `Unsubscribe` is unsupported in this context"); begin(c, ST_AFTER); continue;
        case C_SUBSCRIBE:
            c->sub = 1; c->mb = mb_new(L.subQueue, t, w); c->ci = m->first; c->st = ST_SUBCMD; continue;
        case C_SET: c->k = value_new(m->key, m->klen); c->v = value_new(m->val, m->vlen); break;        // allocated BEFORE the lock
        case C_PUBLISH: c->v = value_new(m->val, m->vlen); break;
        default: break;
        }
        c->st = ST_DB; continue;
    }

    case ST_DB: {
        Cmd *m = &c->cmd;
        if (!lock_enter(&c->lw, &c->waiting, t, m->kind == C_SET)) return park(t, W_SIG);
        if (m->kind == C_GET) {
            Value *v = db_get(m->key, m->klen);               // the reply is written after the lock is released
            rw_release(&g_db.lock, 0);
            if (v) out_bulk(o, w, v->d, v->len, v); else out_null(o, w);
        } else if (m->kind == C_SET) {
            int earlier = 0;
            u64 exp = m->expire_ms ? mono_ns() + m->expire_ms * 1000000ull : 0;
            int full = db_set(c->k, c->v, exp, &earlier);
            rw_release(&g_db.lock, 1);
            if (full) { value_unref(c->k); value_unref(c->v); out_error(o, w, "ERR max keys"); }
            else { out_simple(o, w, "OK"); if (earlier) bell_ring(&g_db.bell); }
            c->k = c->v = NULL;
        } else {                                              // C_PUBLISH
            u64 n = db_publish(m->key, m->klen, c->v);
            rw_release(&g_db.lock, 0);
            value_unref(c->v); c->v = NULL;
            out_int(o, w, n);
        }
        begin(c, ST_AFTER); continue;
    }

    case ST_FLUSH: {                                          // writer.flush()
        if (o->ovf) return conn_end(c, 0);
        int r = flush_step(t->fd, o);
        if (r == 1) return park(t, W_WR);
        if (r < 0) return conn_end(c, 0);
        out_reset(o, w);
        c->st = c->after; continue;
    }

    case ST_AFTER:
        finish(c);
        if (c->sub) { c->st = ST_SUB; continue; }
        c->st = ST_CMD;
#ifndef BREAK_FAIR                                           /* negative control: never yield, so a pipelining publisher floods the subscribers */
        if (++c->burst >= PER_TURN) { c->burst = 0; return yield_now(t); }      // a pipelining client cannot starve the worker (and its subscribers)
#endif
        continue;
    case ST_END: return conn_end(c, 0);
    case ST_LINGER: return conn_end(c, 1);

    case ST_SUB: {                                            // subscribeLoop
        if (SHUTTING()) return conn_end(c, 0);
        if (c->sent < PER_TURN) {
            Msg m;
            if (mb_pop(c->mb, &m)) {
                c->sent++;
                Value *ch = m.a, *d = m.b;
                out_array(o, w, 3); out_bulk(o, w, "message", 7, NULL); out_bulk(o, w, ch->d, ch->len, NULL);
                value_unref(ch);
                out_bulk(o, w, d->d, d->len, d);              // the data Value is held until the reply is sent
                begin(c, ST_SUB); continue;
            }
        }
        u32 lost = mb_take_lost(c->mb);
        if (lost) {
            if (L.lagDisconnect) {
                char t2[96]; snprintf(t2, sizeof t2, "ERR subscriber too slow; %u messages lost", lost);
                out_error(o, w, t2); begin(c, ST_END); continue;
            }
            out_array(o, w, 2); out_bulk(o, w, "lagged", 6, NULL); out_int(o, w, lost);
            begin(c, ST_SUB); continue;
        }
        int busy = c->sent >= PER_TURN;
        int r = next_frame(c);
        c->sent = 0;
        if (r == NF_IDLE) {
            if (busy) return yield_now(t);                    // more messages may be waiting: yield, having looked at the socket
            if (mb_arm(c->mb)) continue;                      // a message arrived meanwhile
            return park(t, W_RD | W_SIG);                     // waitAny(readable, mailbox, shutdown)
        }
        if (r == NF_CLOSED || r == NF_IOERR) return conn_end(c, 0);
        if (r == NF_PROTO) { fail(c, c->errmsg); continue; }
        if (r == NF_TOOBIG) { fail(c, "frame too large"); continue; }
        const char *err;
        if (parse_cmd(c->ib + c->istart, c->args, &c->cmd, &err) < 0) { fail(c, err); continue; }
        CNT(w->nreq);
        if (c->cmd.kind == C_SUBSCRIBE || c->cmd.kind == C_UNSUBSCRIBE) { c->ci = c->cmd.first; c->st = ST_SUBCMD; continue; }
        if (c->cmd.kind == C_UNKNOWN) reply_unknown(c, c->cmd.key, c->cmd.klen);
        else { char t2[64]; snprintf(t2, sizeof t2, "ERR unknown command '%s'", kind_name[c->cmd.kind]); out_error(o, w, t2); }
        begin(c, ST_AFTER); continue;
    }

    case ST_SUBCMD: {                                         // one channel per step: lock, change the table, answer
        Cmd *m = &c->cmd;
        u32 cnt = c->args->count;
        const char *nm; u32 nl; Value *taken = NULL;
        if (m->kind == C_SUBSCRIBE) {
            if (c->ci >= cnt) { c->st = ST_AFTER; continue; }
            const Arg *x = &c->args->items[c->ci];
            nm = c->ib + c->istart + x->start; nl = x->end - x->start;
            int again = name_index(c, nm, nl) >= 0;           // subscribing twice changes nothing (the original replaces the stream)
            if (!again && c->nnames >= L.maxSubs) { out_error(o, w, "ERR max subscriptions"); c->ci++; begin(c, ST_SUBCMD); continue; }
            if (!lock_enter(&c->lw, &c->waiting, t, 1)) return park(t, W_SIG);
            int full = db_subscribe(nm, nl, c->mb);
            rw_release(&g_db.lock, 1);
            if (full) out_error(o, w, "ERR max channels");
            else {
                if (!again) {
                    if (c->nnames == c->ncap) { c->ncap = c->ncap ? c->ncap * 2 : 4; c->names = realloc(c->names, c->ncap * sizeof(Value *)); }
                    c->names[c->nnames++] = value_new(nm, nl);
                }
                reply_sub(c, "subscribe", nm, nl, c->nnames);
            }
            c->ci++; begin(c, ST_SUBCMD); continue;
        }
        // UNSUBSCRIBE: the named channels, or all of them when none are named (the original's rule)
        if (cnt == m->first) {
            if (c->nnames == 0) { c->st = ST_AFTER; continue; }
            taken = c->names[c->nnames - 1]; nm = taken->d; nl = taken->len;
        } else {
            if (c->ci >= cnt) { c->st = ST_AFTER; continue; }
            const Arg *x = &c->args->items[c->ci];
            nm = c->ib + c->istart + x->start; nl = x->end - x->start;
        }
        if (!lock_enter(&c->lw, &c->waiting, t, 1)) return park(t, W_SIG);
        db_unsubscribe(nm, nl, c->mb);
        rw_release(&g_db.lock, 1);
        int idx = name_index(c, nm, nl);
        if (idx >= 0) { Value *gone = c->names[idx]; c->names[idx] = c->names[--c->nnames]; reply_sub(c, "unsubscribe", nm, nl, c->nnames); value_unref(gone); }
        else reply_sub(c, "unsubscribe", nm, nl, c->nnames);
        if (cnt != m->first) c->ci++;
        begin(c, ST_SUBCMD); continue;
    }
    }
}

// ---------------------------------------------------------------- the purge task (Strata: Db.purgeLoop): expired keys, and the reaper queue
typedef struct { Task t; u8 st, waiting; LockWait lw; Reap *cur; u64 next; u32 hidx; } PurgeTask;
static PurgeTask *g_purge;                                     // set once by worker 0, read by every worker at shutdown (atomic)

static int purge_resume(Task *t) {
    PurgeTask *p = (PurgeTask *)t;
    Worker *w = t->w;
    for (;;) switch (p->st) {
    case 0:                                                   // top of the loop
        timer_cancel(w, &p->hidx);
        if (SHUTTING()) return DONE;
        bell_take(&g_db.bell);
        p->cur = db_reap_pop();
        p->st = p->cur ? 1 : 2; continue;
    case 1:                                                   // drainReaper: unsubscribe a finished connection's channels
        if (!lock_enter(&p->lw, &p->waiting, t, 1)) return park(t, W_SIG);
        db_unsubscribe(p->cur->name->d, p->cur->name->len, p->cur->mb);
        rw_release(&g_db.lock, 1);
        value_unref(p->cur->name); mb_unref(p->cur->mb); free(p->cur); p->cur = NULL;
        p->st = 0; continue;
    case 2: {                                                 // purgeExpired: at most PURGE_BATCH keys per lock hold
        if (!lock_enter(&p->lw, &p->waiting, t, 1)) return park(t, W_SIG);
        u64 next; int full = db_purge(mono_ns(), &next);
        rw_release(&g_db.lock, 1);
        p->next = next;
        if (full) { p->st = 0; return yield_now(t); }
        p->st = 3; continue;
    }
    case 3:                                                   // waitAny(timer at `next`, bell, shutdown)
        if (bell_arm(&g_db.bell)) { p->st = 0; continue; }
        if (p->next) timer_add_c(w, t, p->next, &p->hidx);
        p->st = 0;
        return park(t, W_TIMER | W_SIG);
    }
}

// ---------------------------------------------------------------- accept hook, shutdown, main
static void refuse(Worker *w, int fd) {                       // over maxConns: answer, then linger so the client can read it
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.ptr = NULL };
    if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &ev) < 0) { close(fd); return; }
    if (send(fd, "-ERR max clients\r\n", 18, MSG_NOSIGNAL) < 0) { /* the client is gone */ }
    Task old = { .w = w, .fd = fd };
    start_linger(&old);
}
static void spawn_conn(Worker *w, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (SHUTTING()) { close(fd); return; }
    if (!lim_acquire(&g_conns)) { refuse(w, fd); return; }
    ConnTask *c = pool_alloc(&w->tasks);
    memset(c, 0, sizeof *c);
    c->t.resume = conn_resume; c->t.w = w; c->t.fd = fd; c->t.rd_ready = 1;
    struct epoll_event ev = { .events = EPOLLIN | EPOLLOUT | EPOLLET, .data.ptr = c };
    if (epoll_ctl(w->ep, EPOLL_CTL_ADD, fd, &ev) < 0) { perror("epoll_ctl add"); exit(1); }
    c->lnext = g_clist[w->id]; if (c->lnext) c->lnext->lprev = c; g_clist[w->id] = c;
    LIVE_ADD(w, 1); CNT(w->nacc);
    enqueue(w, &c->t);
}
static void on_shutdown(Worker *w) {                          // runs on the worker: every parked connection (and the purge task) notices the flag
    for (ConnTask *c = g_clist[w->id]; c; c = c->lnext) if (c->t.parked) wake(w, &c->t);
    PurgeTask *p = __atomic_load_n(&g_purge, __ATOMIC_ACQUIRE);
    if (p && p->t.w == w && p->t.parked) wake(w, &p->t);
}
static void app_worker_init(Worker *w) {
    pool_init(&w->tasks, sizeof(ConnTask));
    pool_init(&w->reqs, sizeof(Args));
    pool_init(&w->lingers, sizeof(Linger));
    pool_init(&w->bufs, sizeof(Buf));                         // output units
    for (int i = 0; i < NCLS; i++) pool_init(&w->cls[i], g_cls_size[i]);
    sync_worker_init(w);
    if (w->id == 0) {                                         // one purge task, on worker 0
        PurgeTask *p = calloc(1, sizeof *p);
        p->t.resume = purge_resume; p->t.w = w; p->hidx = TIMER_NONE;
        g_db.bell.t = &p->t; g_db.bell.w = w;
        __atomic_store_n(&g_purge, p, __ATOMIC_RELEASE);
        enqueue(w, &p->t);
    }
}
static void on_signal(int s) { (void)s; __atomic_store_n(&g_shutdown, 1, __ATOMIC_RELAXED); }

int main(int argc, char **argv) {
    rt_port = atoi(arg(argc, argv, "port", "6379"));
    rt_nworkers = atoi(arg(argc, argv, "workers", "1"));
    rt_cpu0 = atoi(arg(argc, argv, "cpu0", "0"));
    g_stats = atoi(arg(argc, argv, "stats", "0"));
    L.maxConns = (u32)atoi(arg(argc, argv, "maxconns", "250"));
    L.maxFrame = (u32)strtoul(arg(argc, argv, "maxframe", "4194304"), NULL, 10);
    L.maxBulk = (u32)strtoul(arg(argc, argv, "maxbulk", "1048576"), NULL, 10);
    L.maxKey = (u32)atoi(arg(argc, argv, "maxkey", "512"));
    L.maxKeys = (u32)atoi(arg(argc, argv, "maxkeys", "1000000"));
    L.maxTopics = (u32)atoi(arg(argc, argv, "maxtopics", "10000"));
    L.maxSubs = (u32)atoi(arg(argc, argv, "maxsubs", "100"));
    L.subQueue = (u32)atoi(arg(argc, argv, "subqueue", "1024"));
    L.maxExpireSecs = strtoull(arg(argc, argv, "maxexpire", "315360000"), NULL, 10);
    L.lagDisconnect = strcmp(arg(argc, argv, "lag", "disconnect"), "notice") != 0;
    g_linger_ms = (u32)atoi(arg(argc, argv, "linger", "2000"));
    g_conns.limit = L.maxConns;
    if (L.maxBulk > L.maxFrame) L.maxBulk = L.maxFrame;
    if (rt_nworkers > MAXW) rt_nworkers = MAXW;
    db_init();
    g_msg_drop = msg_drop;
    rt_on_shutdown = on_shutdown;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    rt_on_accept = spawn_conn; rt_worker_init = app_worker_init;
    rt_start_workers();
    fprintf(stderr, "mini-redis: port=%d workers=%d sizeof(ConnTask)=%zu maxconns=%u maxframe=%u maxbulk=%u maxkeys=%u lag=%s\n", rt_port, rt_nworkers,
            sizeof(ConnTask), L.maxConns, L.maxFrame, L.maxBulk, L.maxKeys, L.lagDisconnect ? "disconnect" : "notice");
    u64 last_req = 0; double last_t = now_s(); int ticks = 0;
    while (!SHUTTING()) {
        usleep(10000);
        if (!g_stats || ++ticks < 100) continue;
        ticks = 0;
        u64 live = 0, req = 0;
        for (int i = 0; i < rt_nworkers; i++) { live += LOAD(rt_workers[i].live); req += LOAD(rt_workers[i].nreq); }
        double t = now_s();
        printf("live=%lu rps=%.0f keys=%lu topics=%lu lock_contended=%lu rss=%ld MB\n", (unsigned long)live, (req - last_req) / (t - last_t),
               (unsigned long)g_db.count, (unsigned long)g_db.tcount, (unsigned long)g_db.lock.contended, rss_kb() / 1024);
        fflush(stdout); last_req = req; last_t = t;
    }
    // graceful shutdown: stop accepting (spawn_conn closes new sockets), tell every worker, wait for the connections to finish
    for (int i = 0; i < rt_nworkers; i++) wake_shutdown(&rt_workers[i]);
    for (int i = 0; i < 500; i++) {
        u64 live = 0;
        for (int j = 0; j < rt_nworkers; j++) live += LOAD(rt_workers[j].live);
        if (!live) break;
        usleep(10000);
    }
    fprintf(stderr, "mini-redis: shutdown complete\n");
    return 0;
}
