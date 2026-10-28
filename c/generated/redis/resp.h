#ifndef REDIS_RESP_H
#define REDIS_RESP_H
// redis/resp.h - C counterpart of redis/frame.stra (the single-pass resumable request scanner) and redis/reply.stra (reply writer).
// A request is an array of simple/bulk/integer elements, scanned into offsets (relative to the frame's first byte) in one pass. Limits are checked
// from the length lines, before the bytes they announce are read. Replies go into ONE pooled output unit; a bulk too big for it is sent from where it is.
#include "db.h"
#include "rt_buf.h"

#define MAX_ARGS 64
#define MAX_LINE 32
enum { AK_SIMPLE, AK_BULK, AK_INT, AK_NULL };
typedef struct { u8 kind; u32 start, end; u64 number; } Arg;
typedef struct { Arg items[MAX_ARGS]; u32 count; } Args;
typedef struct { u32 pos, left; u8 started; } Scanner;
enum { SC_DONE, SC_MORE, SC_PROTO, SC_TOOBIG };

static int read_line(const char *d, u32 len, u32 from, u32 *end, u32 *next) {
    const char *p = d + from, *e = d + len;
    while (p < e && (p = memchr(p, '\r', (size_t)(e - p))) && p + 1 < e) {
        if (p[1] == '\n') { *end = (u32)(p - d); *next = *end + 2; return 1; }
        p++;
    }
    return 0;
}
static int parse_decimal(const char *p, u32 n, u64 *out) {
    if (n == 0 || n > 20) return 0;
    u64 v = 0;
    for (u32 i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        u64 d = (u64)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    *out = v;
    return 1;
}

// Continues the scan over d[0..len). SC_DONE: *n = frame length. SC_MORE: *n = bytes (from the frame start) needed before progress is possible.
static int scan_advance(Scanner *s, const char *d, u32 len, Args *a, u32 *n, const char **msg) {
    u32 end, next; u64 v;
    if (!s->started) {
        if (!read_line(d, len, s->pos, &end, &next)) {
            if (len - s->pos > MAX_LINE) { *msg = "protocol error; expected array"; return SC_PROTO; }
            *n = len + 1; return SC_MORE;
        }
        if (d[s->pos] != '*') { *msg = "protocol error; expected array"; return SC_PROTO; }
        if (!parse_decimal(d + s->pos + 1, end - s->pos - 1, &v)) { *msg = "protocol error; invalid number"; return SC_PROTO; }
        if (v == 0) { *msg = "protocol error; empty command"; return SC_PROTO; }
        if (v > MAX_ARGS) { *msg = "protocol error; too many arguments"; return SC_PROTO; }
        s->left = (u32)v; s->pos = next; s->started = 1; a->count = 0;
    }
    while (s->left > 0) {
        if (s->pos >= len) { *n = s->pos + 1; return SC_MORE; }
        if (!read_line(d, len, s->pos, &end, &next)) {
            u32 most = d[s->pos] == '+' ? L.maxBulk : MAX_LINE;
            if (len - s->pos > most) return SC_TOOBIG;
            *n = len + 1; return SC_MORE;
        }
        Arg *x = &a->items[a->count];
        switch (d[s->pos]) {
        case '+':
            if (end - s->pos > L.maxBulk) return SC_TOOBIG;
            *x = (Arg){ AK_SIMPLE, s->pos + 1, end, 0 }; s->pos = next; break;
        case ':':
            if (!parse_decimal(d + s->pos + 1, end - s->pos - 1, &v)) { *msg = "protocol error; invalid number"; return SC_PROTO; }
            *x = (Arg){ AK_INT, 0, 0, v }; s->pos = next; break;
        case '$':
            if (end - s->pos - 1 == 2 && d[s->pos + 1] == '-' && d[s->pos + 2] == '1') { *x = (Arg){ AK_NULL, 0, 0, 0 }; s->pos = next; break; }
            if (!parse_decimal(d + s->pos + 1, end - s->pos - 1, &v)) { *msg = "protocol error; invalid frame format"; return SC_PROTO; }
#ifndef BREAK_LIMIT                                          /* negative control: limits not enforced */
            if (v > L.maxBulk) return SC_TOOBIG;                          // refused before a single data byte is read
            if ((u64)next + v + 2 > L.maxFrame) return SC_TOOBIG;
#endif
            { u32 total = next + (u32)v + 2;
              if (len < total) { *n = total; return SC_MORE; }
              if (d[total - 2] != '\r' || d[total - 1] != '\n') { *msg = "protocol error; invalid frame format"; return SC_PROTO; }
              *x = (Arg){ AK_BULK, next, next + (u32)v, 0 }; s->pos = total; }
            break;
        default: *msg = "protocol error; invalid element type"; return SC_PROTO;
        }
        a->count++; s->left--;
    }
    *n = s->pos;
    return SC_DONE;
}

// ---------------------------------------------------------------- replies
// seg0 = buffer[0..used), then (optionally) one big bulk body, then "\r\n". flush_step sends what is left, parking on EAGAIN.
typedef struct { Buf *b; u32 used, blen, sent; const char *body; Value *hold; u8 ovf; } Out;

static void out_put(Out *o, Worker *w, const char *p, u32 n) {
    if (!o->b) o->b = buf_get(w);
    if (o->used + n > OUTSZ || o->body) { o->ovf = 1; return; }       // cannot happen with the declared limits; the connection is closed if it does
    memcpy(o->b->data + o->used, p, n); o->used += n;
}
static void out_str(Out *o, Worker *w, const char *s) { out_put(o, w, s, (u32)strlen(s)); }
static void out_dec(Out *o, Worker *w, u64 v) {
    char t[24]; int i = 24; t[--i] = '\n'; t[--i] = '\r';
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out_put(o, w, t + i, (u32)(24 - i));
}
static void out_simple(Out *o, Worker *w, const char *s) { out_str(o, w, "+"); out_str(o, w, s); out_str(o, w, "\r\n"); }
static void out_error(Out *o, Worker *w, const char *s) { out_str(o, w, "-"); out_str(o, w, s); out_str(o, w, "\r\n"); }
static void out_int(Out *o, Worker *w, u64 v) { out_str(o, w, ":"); out_dec(o, w, v); }
static void out_null(Out *o, Worker *w) { out_str(o, w, "$-1\r\n"); }
static void out_array(Out *o, Worker *w, u64 n) { out_str(o, w, "*"); out_dec(o, w, n); }
// A bulk string. Small ones are copied into the unit; a big one becomes the body (only the LAST bulk of a reply may be big). `hold`, if given, is a
// Value reference kept until the reply is sent (the body points into it).
static void out_bulk(Out *o, Worker *w, const char *p, u32 n, Value *hold) {
    if (!o->b) o->b = buf_get(w);
    out_str(o, w, "$"); out_dec(o, w, n);
    if (o->used + n + 2 <= OUTSZ) { out_put(o, w, p, n); out_str(o, w, "\r\n"); if (hold) value_unref(hold); }
    else { o->body = p; o->blen = n; if (o->hold) o->ovf = 1; o->hold = hold; }
}
// 0 sent everything, 1 would block (park W_WR), -1 error
static int flush_step(int fd, Out *o) {
    u32 total = o->used + o->blen + (o->blen ? 2 : 0);
    while (o->sent < total) {
        struct iovec iov[3]; int n = 0; u32 skip = o->sent;
        if (skip < o->used) { iov[n].iov_base = o->b->data + skip; iov[n].iov_len = o->used - skip; n++; skip = 0; } else skip -= o->used;
        if (o->blen) {
            if (skip < o->blen) { iov[n].iov_base = (void *)(o->body + skip); iov[n].iov_len = o->blen - skip; n++; skip = 0; } else skip -= o->blen;
            iov[n].iov_base = (void *)("\r\n" + skip); iov[n].iov_len = 2 - skip; n++;
        }
        struct msghdr mh = { .msg_iov = iov, .msg_iovlen = (size_t)n };
        ssize_t r = sendmsg(fd, &mh, MSG_NOSIGNAL);
        if (r > 0) { o->sent += (u32)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && errno == EAGAIN) return 1;
        return -1;
    }
    return 0;
}
static void out_reset(Out *o, Worker *w) {                          // after a flush, or when the connection ends
    if (o->b) { buf_put(w, o->b); o->b = NULL; }
    if (o->hold) { value_unref(o->hold); o->hold = NULL; }
    o->used = o->blen = o->sent = 0; o->body = NULL; o->ovf = 0;
}
#endif
