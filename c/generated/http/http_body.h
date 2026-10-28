#ifndef HTTP_BODY_H
#define HTTP_BODY_H
// http/http_body.h - request bodies (C counterpart of http/body.stra and Reader.readBody / readUpload in http/reader.stra).
#include "rt.h"
#include "rt_buf.h"

// ================================================================ request bodies (conc mode)
// Two readers, both part of the runtime library (not the language), sharing one stream decoder (Content-Length or chunked, decoded
// through the connection's input buffer, ending exactly at the end of the body):
//   - the STANDARD reader (POST /api): the body is read into memory, limited by maxbody= (1 MB by default: a typical API server should not
//     accept more). It needs no files, and the handler always has all of it.
//   - the UPLOAD reader (POST /upload): for file uploads and other large bodies. A route opts in explicitly. The body is written to an
//     unnamed temporary file (O_TMPFILE: no name, deleted when closed) as it arrives, so memory use does not grow with its size, and it is
//     limited by uploadmax= (4 GB by default), separately from maxbody.
// A Content-Length over the limit is refused (413) before a single body byte is read; a chunked body as soon as a chunk would take it over.
// The digest (CRC-32) lets a test check a 200 MB upload byte for byte. Blocking file writes stay on the worker here (see results/http.md).
static u64 g_maxbody = 1ull << 20;                   // maxbody=    limit of the standard (in-memory) reader
static u64 g_uploadmax = 4ull << 30;                 // uploadmax=  limit of the upload (file-backed) reader
static const char *g_tmpdir = "/var/tmp";            // tmpdir=     where uploads are spooled

static u32 crc_tab[256];
static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
}
static inline u32 crc_update(u32 crc, const unsigned char *p, u32 n) {
    crc = ~crc;
    for (u32 i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

enum { BS_SIZE, BS_EXT, BS_SIZE_LF, BS_DATA, BS_DATA_CR, BS_DATA_LF, BS_TR_START, BS_TR_LINE, BS_TR_END_LF };
typedef struct {
    u64 total, remaining, csize, limit;
    char *mem; size_t mlen, mcap;                      // standard reader: the body, in malloc'd memory (at most `limit` bytes)
    u32 crc, trailer;
    int fd;                                            // upload reader: the unnamed temporary file, opened at the first byte
    u8 upload, mode, bs, digits, done, err;            // mode: 1 Content-Length, 2 chunked.  err: 0 ok, 1 too large (413), 2 malformed (400), 3 connection lost, 4 storage failure (500)
} Body;

// Request framing: 0 none, 1 Content-Length (*cl), 2 chunked, -1 malformed (both, a bad or repeated Content-Length).
static int body_framing(const char *b, u32 n, u64 *cl) {
    int have_cl = 0, chunked = 0;
    *cl = 0;
    const char *e = b + n, *l = memchr(b, '\n', n);
    while (l && l + 1 < e) {
        l++;
        if (e - l > 15 && strncasecmp(l, "content-length:", 15) == 0) {
            if (have_cl) return -1;
            have_cl = 1;
            const char *p = l + 15;
            while (p < e && (*p == ' ' || *p == '\t')) p++;
            if (p >= e || *p < '0' || *p > '9') return -1;
            u64 v = 0;
            while (p < e && *p >= '0' && *p <= '9') { if (v > (UINT64_MAX - 9) / 10) return -1; v = v * 10 + (u64)(*p++ - '0'); }
            while (p < e && (*p == ' ' || *p == '\t')) p++;
            if (p < e && *p != '\r') return -1;
            *cl = v;
        } else if (e - l > 18 && strncasecmp(l, "transfer-encoding:", 18) == 0) {
            const char *le = memchr(l, '\r', e - l);
            if (!le) le = e;
            for (const char *q = l + 18; q + 7 <= le; q++) if (strncasecmp(q, "chunked", 7) == 0) chunked = 1;
        }
        l = memchr(l, '\n', e - l);
    }
    if (have_cl && chunked) return -1;
    return chunked ? 2 : have_cl ? 1 : 0;
}
static int expects_100(const char *b, u32 n) {
    const char *e = b + n, *l = memchr(b, '\n', n);
    while (l && l + 1 < e) {
        l++;
        if (e - l > 7 && strncasecmp(l, "expect:", 7) == 0) {
            const char *le = memchr(l, '\r', e - l);
            if (!le) le = e;
            for (const char *q = l + 7; q + 12 <= le; q++) if (strncasecmp(q, "100-continue", 12) == 0) return 1;
        }
        l = memchr(l, '\n', e - l);
    }
    return 0;
}

static int write_all_fd(int fd, const char *p, u64 n) {
    while (n) {
        ssize_t k = write(fd, p, n > (1u << 20) ? (1u << 20) : n);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        p += k; n -= (u64)k;
    }
    return 0;
}
static int sink_write(Worker *w, Body *b, const char *p, u32 n) {
    (void)w;
    b->crc = crc_update(b->crc, (const unsigned char *)p, n);
    b->total += n;
    if (!b->upload) {                                  // standard reader: into memory (bounded by `limit`, which the decoder enforces)
        if (b->mlen + n > b->mcap) {
            size_t nc = b->mcap ? b->mcap * 2 : 4096;
            while (nc < b->mlen + n) nc *= 2;
            char *nm = realloc(b->mem, nc);
            if (!nm) return -1;
            b->mem = nm; b->mcap = nc;
        }
        memcpy(b->mem + b->mlen, p, n); b->mlen += n;
        return 0;
    }
    if (b->fd < 0) {                                   // upload reader: into an unnamed temporary file, opened at the first byte
        b->fd = open(g_tmpdir, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        if (b->fd < 0) return -1;
    }
#ifdef BREAK_SPILL                                     /* negative control only: drop the first write to the file */
    if (b->total == n) return 0;
#endif
    return write_all_fd(b->fd, p, n);
}
static void body_free(Worker *w, Body *b) {
    free(b->mem);
    if (b->fd >= 0) close(b->fd);                         // an unnamed file: closing it deletes it
    pool_free(&w->bodies, b);
}
static inline int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// Feed the next `n` bytes of the connection's input to the decoder. Returns how many it consumed (it stops at the end of the body, so
// pipelined requests behind it are left alone); b->done / b->err say what happened.
static u32 body_feed(Worker *w, Body *b, const char *p, u32 n) {
    u32 i = 0;
    if (b->mode == 1) {
        u64 take = n < b->remaining ? n : b->remaining;
        if (take) {
            if (sink_write(w, b, p, (u32)take) < 0) { b->err = 4; return 0; }
            b->remaining -= take; i = (u32)take;
        }
        if (b->remaining == 0) b->done = 1;
        return i;
    }
    while (i < n && !b->done && !b->err) {
        unsigned char c = (unsigned char)p[i];
        switch (b->bs) {
        case BS_SIZE: {
            int h = hexv(c);
            if (h >= 0) { if (b->digits >= 16) { b->err = 2; break; } b->csize = b->csize * 16 + (u64)h; b->digits++; i++; }
            else if (c == ';' && b->digits) { b->bs = BS_EXT; i++; }
            else if (c == '\r' && b->digits) { b->bs = BS_SIZE_LF; i++; }
            else b->err = 2;
            break; }
        case BS_EXT:
            if (c == '\r') b->bs = BS_SIZE_LF; else if (c == '\n') { b->err = 2; break; }
            i++; break;
        case BS_SIZE_LF:
            if (c != '\n') { b->err = 2; break; }
            i++;
            if (b->csize == 0) { b->bs = BS_TR_START; break; }
#ifndef BREAK_LIMIT                                       /* negative control only: do not enforce the body limit */
            if (b->csize > b->limit || b->total + b->csize > b->limit) { b->err = 1; break; }
#endif
            b->remaining = b->csize; b->bs = BS_DATA;
            break;
        case BS_DATA: {
            u64 take = (u64)(n - i) < b->remaining ? (u64)(n - i) : b->remaining;
            if (sink_write(w, b, p + i, (u32)take) < 0) { b->err = 4; break; }
            b->remaining -= take; i += (u32)take;
            if (b->remaining == 0) b->bs = BS_DATA_CR;
            break; }
        case BS_DATA_CR: if (c != '\r') { b->err = 2; break; } i++; b->bs = BS_DATA_LF; break;
        case BS_DATA_LF: if (c != '\n') { b->err = 2; break; } i++; b->bs = BS_SIZE; b->csize = 0; b->digits = 0; break;
        case BS_TR_START: if (c == '\r') { b->bs = BS_TR_END_LF; i++; } else b->bs = BS_TR_LINE; break;
        case BS_TR_LINE: if (++b->trailer > 8192) { b->err = 2; break; } if (c == '\n') b->bs = BS_TR_START; i++; break;
        case BS_TR_END_LF: if (c != '\n') { b->err = 2; break; } i++; b->done = 1; break;
        }
    }
    return i;
}

#endif
