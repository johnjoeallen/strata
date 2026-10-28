#ifndef HTTP_MSG_H
#define HTTP_MSG_H
// http/http_msg.h - HTTP message pieces (C counterpart of http/types.stra, http/response.stra): the response type and the canned responses, the
// reason-phrase switch, the Connection: header scan, and the method check that makes POST the 'unsafe' method.
#include "rt.h"

typedef struct { char *p; u32 n; } Resp;

// ---------------------------------------------------------------- stdlib leaves (never park)
static Resp r200, r200_ka, r200_close, r404, r400, r413, r431, r500, r501;
// reasonPhrase(code) in server.stra: a switch on a plain value
static const char *reason_phrase(u32 code) {
    switch (code) {
    case 200: return "OK";
    case 301: case 302: case 307: return "Redirect";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default: return "Other";
    }
}
static void mk_resp(Resp *r, u32 code, const char *conn, const char *body) {      // makeResponse(code, connHdr, body, keepAlive)
    char tmp[512];
    int n = snprintf(tmp, sizeof tmp, "HTTP/1.1 %u %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n%s\r\n%s",
                     code, reason_phrase(code), strlen(body), conn, body);
    r->p = malloc(n); memcpy(r->p, tmp, n); r->n = n;
}
static void init_responses(void) {
    mk_resp(&r200, 200, "", "Hello, world\n");
    mk_resp(&r200_ka, 200, "Connection: keep-alive\r\n", "Hello, world\n");
    mk_resp(&r200_close, 200, "Connection: close\r\n", "Hello, world\n");
    mk_resp(&r404, 404, "Connection: close\r\n", "not found\n");
    mk_resp(&r400, 400, "Connection: close\r\n", "bad request\n");
    mk_resp(&r431, 431, "Connection: close\r\n", "headers too large\n");
    mk_resp(&r413, 413, "Connection: close\r\n", "payload too large\n");
    mk_resp(&r501, 501, "Connection: close\r\n", "request bodies need conc=1\n");
    mk_resp(&r500, 500, "", "internal error\n");                 // keep-alive: the connection survives a handler exception
}

// header scan: sets *close / *ka from a Connection: header (case-insensitive)
static void conn_header(const char *b, u32 n, int *close, int *ka) {
    *close = *ka = 0;
    const char *e = b + n;
    const char *l = memchr(b, '\n', n);
    while (l && l + 1 < e) {
        l++;
        if (e - l > 11 && strncasecmp(l, "connection:", 11) == 0) {
            const char *v = l + 11, *le = memchr(v, '\r', e - v);
            if (!le) le = e;
            for (const char *q = v; q + 5 <= le; q++) if (strncasecmp(q, "close", 5) == 0) *close = 1;
            for (const char *q = v; q + 10 <= le; q++) if (strncasecmp(q, "keep-alive", 10) == 0) *ka = 1;
            return;
        }
        l = memchr(l, '\n', e - l);
    }
}

static inline u32 method_len(const char *b, u32 n) {
    return n >= 5 && memcmp(b, "POST ", 5) == 0 ? 5 : (n >= 4 && memcmp(b, "GET ", 4) == 0 ? 4 : 0);
}
static inline int is_post(const char *b, u32 n) { return method_len(b, n) == 5; }       // the only 'unsafe' method here

#endif
