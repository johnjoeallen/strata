#ifndef HTTP_READER_H
#define HTTP_READER_H
// http/http_reader.h - read one request head into a size-class input buffer, parking while the client is slow (C counterpart of http/reader.stra,
// Reader.nextRequest). The concurrent server reads inline in its connection loop instead (Reader.tryNextRequest).
#include "rt_buf.h"

// ---------------------------------------------------------------- reader.next_request() : may park
// The buffer holds unconsumed input in [start, end). A request is consumed by advancing `start`; bytes are
// compacted to the front only when the tail is full and more input is needed (the only copy). With
// -DEAGER_MOVE the old behaviour is restored: handle() moves the remainder to the front after every request.
typedef struct { u32 st; u32 hdr_end; u8 err; } ReadReq;      // hdr_end is an absolute offset into the buffer
static int read_request_step(Task *t, ReadReq *f, char **bufp, u8 *cls, u32 *start, u32 *end) {
    switch (f->st) {
    case 0:
        for (;;) {
            if (*end > *start) {
                char *e = memmem(*bufp + *start, *end - *start, "\r\n\r\n", 4);
                if (e) { f->hdr_end = (u32)(e + 4 - *bufp); f->err = 0; return DONE; }
            }
            if (!ibuf_make_room(t->w, bufp, cls, start, end)) { f->err = 1; return DONE; }   // header block larger than the largest class
            {
                u32 space = g_cls_size[*cls] - *end;
                ssize_t n = recv(t->fd, *bufp + *end, space, 0);
                if (n > 0) {
                    *end += (u32)n;
                    if ((u32)n < space) t->rd_ready = 0;       // short read: socket drained, next edge re-arms
                    continue;
                }
                if (n == 0) { f->err = 2; return DONE; }
                if (errno == EINTR) continue;
                if (errno == EAGAIN) { t->rd_ready = 0; f->st = 1; return park(t, W_RD); }
                f->err = 3; return DONE;
            }
    case 1:
            continue;
        }
    }
    return DONE;
}

#endif
