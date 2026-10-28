#ifndef REDIS_COMMAND_H
#define REDIS_COMMAND_H
// redis/command.h - C counterpart of redis/command.stra: the commands, as borrowed views into the connection's input buffer (valid until the frame is finished).
#include "resp.h"

enum { C_GET, C_SET, C_PUBLISH, C_SUBSCRIBE, C_UNSUBSCRIBE, C_PING, C_UNKNOWN };
typedef struct { int kind; const char *key; u32 klen; const char *val; u32 vlen; u64 expire_ms; u32 first; int has_msg; } Cmd;   // key/val: GET key, SET key value,
                                                                                                                              // PUBLISH channel message, PING message, UNKNOWN name

static int eq_nocase(const char *p, u32 n, const char *lit) {
    if (strlen(lit) != n) return 0;
    for (u32 i = 0; i < n; i++) { char c = p[i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != lit[i]) return 0; }
    return 1;
}
static int is_utf8(const u8 *s, u32 n) {
    u32 i = 0;
    while (i < n) {
        u8 c = s[i]; u32 k;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) k = 1;
        else if (c >= 0xE0 && c <= 0xEF) k = 2;
        else if (c >= 0xF0 && c <= 0xF4) k = 3;
        else return 0;
        if (i + k >= n) return 0;
        for (u32 j = 1; j <= k; j++) if ((s[i + j] & 0xC0) != 0x80) return 0;
        if ((c == 0xE0 && s[i + 1] < 0xA0) || (c == 0xED && s[i + 1] >= 0xA0) || (c == 0xF0 && s[i + 1] < 0x90) || (c == 0xF4 && s[i + 1] >= 0x90)) return 0;
        i += k + 1;
    }
    return 1;
}

typedef struct { const char *base; const Args *a; const char *err; } Frame;
static int arg_blob(Frame *f, u32 i, const char **p, u32 *n) {
    const Arg *x = &f->a->items[i];
    if (x->kind == AK_SIMPLE || x->kind == AK_BULK) { *p = f->base + x->start; *n = x->end - x->start; return 1; }
    f->err = "protocol error; expected simple frame or bulk frame"; return 0;
}
static int arg_text(Frame *f, u32 i, const char **p, u32 *n) {
    if (!arg_blob(f, i, p, n)) return 0;
    if (!is_utf8((const u8 *)*p, *n)) { f->err = "protocol error; invalid string"; return 0; }
    return 1;
}
static int arg_key(Frame *f, u32 i, const char **p, u32 *n) {
    if (!arg_text(f, i, p, n)) return 0;
    if (*n > L.maxKey) { f->err = "protocol error; key too long"; return 0; }
    return 1;
}
static int arg_number(Frame *f, u32 i, u64 *v) {
    const Arg *x = &f->a->items[i];
    if (x->kind == AK_INT) { *v = x->number; return 1; }
    if (x->kind == AK_NULL || !parse_decimal(f->base + x->start, x->end - x->start, v)) { f->err = "protocol error; invalid number"; return 0; }
    return 1;
}

// 0 ok, -1 protocol error (m->err)
static int parse_cmd(const char *base, const Args *a, Cmd *m, const char **err) {
    Frame f = { base, a, NULL };
    const char *name; u32 nl;
    memset(m, 0, sizeof *m);
    if (!arg_text(&f, 0, &name, &nl)) { *err = f.err; return -1; }
    if (eq_nocase(name, nl, "get")) {
        if (a->count != 2) { *err = "protocol error; wrong number of arguments"; return -1; }
        m->kind = C_GET;
        if (!arg_key(&f, 1, &m->key, &m->klen)) { *err = f.err; return -1; }
    } else if (eq_nocase(name, nl, "set")) {
        if (a->count != 3 && a->count != 5) { *err = "protocol error; wrong number of arguments for `SET`"; return -1; }
        m->kind = C_SET;
        if (!arg_key(&f, 1, &m->key, &m->klen) || !arg_blob(&f, 2, &m->val, &m->vlen)) { *err = f.err; return -1; }
        if (a->count == 5) {
            const char *o; u32 ol; u64 v;
            if (!arg_text(&f, 3, &o, &ol) || !arg_number(&f, 4, &v)) { *err = f.err; return -1; }
            if (eq_nocase(o, ol, "ex")) { if (v == 0 || v > L.maxExpireSecs) { *err = "invalid expire time in 'set' command"; return -1; } m->expire_ms = v * 1000; }
            else if (eq_nocase(o, ol, "px")) { if (v == 0 || v > L.maxExpireSecs * 1000) { *err = "invalid expire time in 'set' command"; return -1; } m->expire_ms = v; }
            else { *err = "currently `SET` only supports the expiration option"; return -1; }
        }
    } else if (eq_nocase(name, nl, "publish")) {
        if (a->count != 3) { *err = "protocol error; wrong number of arguments"; return -1; }
        m->kind = C_PUBLISH;
        if (!arg_key(&f, 1, &m->key, &m->klen) || !arg_blob(&f, 2, &m->val, &m->vlen)) { *err = f.err; return -1; }
    } else if (eq_nocase(name, nl, "subscribe")) {
        if (a->count < 2) { *err = "protocol error; SUBSCRIBE needs a channel"; return -1; }
        m->kind = C_SUBSCRIBE; m->first = 1;
        for (u32 i = 1; i < a->count; i++) { const char *p; u32 n; if (!arg_key(&f, i, &p, &n)) { *err = f.err; return -1; } }   // validate now
    } else if (eq_nocase(name, nl, "unsubscribe")) {
        m->kind = C_UNSUBSCRIBE; m->first = 1;
        for (u32 i = 1; i < a->count; i++) { const char *p; u32 n; if (!arg_key(&f, i, &p, &n)) { *err = f.err; return -1; } }
    } else if (eq_nocase(name, nl, "ping")) {
        if (a->count > 2) { *err = "protocol error; expected end of frame, but there was more"; return -1; }
        m->kind = C_PING;
        if (a->count == 2) { if (!arg_blob(&f, 1, &m->val, &m->vlen)) { *err = f.err; return -1; } m->has_msg = 1; }
    } else { m->kind = C_UNKNOWN; m->key = name; m->klen = nl; }
    return 0;
}
#endif
