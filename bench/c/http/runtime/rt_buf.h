#ifndef RT_BUF_H
#define RT_BUF_H
// runtime/rt_buf.h - buffers for connections (C runtime; the Strata side is the growable `u8[]` in runtime/leaves.stra).
//   * output buffers: one fixed unit per write (OUTSZ), from a per-worker pool
//   * input buffers: size classes (512 B .. 16 KB). A connection takes the smallest when its first byte arrives, compacts when the tail is full, and moves to
//     the next class (copying the unconsumed bytes) only when full of one unfinished header block. Requests are offsets, so growth invalidates nothing.
//     maxhead= caps the largest class (g_maxcls). -DBREAK_GROW is the negative control for the `bigheader` test.
#include "rt.h"

// Output buffers (a completed prefix, held responses) are one fixed unit: the size of a single write.
#define OUTSZ 2048
typedef struct { char data[OUTSZ]; } Buf;

// Input buffers are size classes (server.stra: Reader.buf is a runtime-provided growable u8[]). A connection takes the smallest
// class when the first byte arrives, compacts when the tail is full, and moves to the next class (copying the unconsumed bytes)
// only when the buffer is full of one unfinished header block; the old block returns to its pool. Requests are offsets into the
// buffer, so a move never invalidates one. Nothing is held while the connection is idle. maxhead= caps the largest class.
static const u32 g_cls_size[NCLS] = { 512, 2048, 8192, 16384 };
static int g_maxcls = NCLS - 1;
static inline char *cbuf_get(Worker *w, int cls) { return pool_alloc(&w->cls[cls]); }
static inline void cbuf_put(Worker *w, int cls, char *p) { pool_free(&w->cls[cls], p); }
static inline int cls_for(u32 n) { for (int i = 0; i < NCLS; i++) if (n <= g_cls_size[i]) return i; return NCLS - 1; }
// Free space at the tail for the next read. 1 = there is room, 0 = the header block does not fit in the largest class.
static int ibuf_make_room(Worker *w, char **buf, u8 *cls, u32 *start, u32 *end) {
    if (*end < g_cls_size[*cls]) return 1;                                  // room at the tail
    if (*start > 0) {                                                       // compact
        memmove(*buf, *buf + *start, *end - *start);
        *end -= *start; *start = 0;
        return 1;
    }
    if (*cls >= g_maxcls) return 0;                                         // full of one unfinished header block, at the limit
    char *nb = cbuf_get(w, *cls + 1);                                       // grow: next class
#ifndef BREAK_GROW                                                          /* negative control only: forget to copy the unconsumed bytes */
    memcpy(nb, *buf, *end);
#endif
    cbuf_put(w, *cls, *buf);
    *buf = nb; (*cls)++;
    return 1;
}

static inline Buf *buf_get(Worker *w) { return pool_alloc(&w->bufs); }
static inline void buf_put(Worker *w, Buf *b) { pool_free(&w->bufs, b); }

// Body reads stream through the input buffer; give it a bigger class (copying the unconsumed bytes) so a large upload is not read in
// 512-byte pieces.
static void ibuf_upgrade(Worker *w, char **buf, u8 *cls, u32 *start, u32 *end, int want) {
    if (*cls >= want) return;
    char *nb = cbuf_get(w, want);
    memcpy(nb, *buf + *start, *end - *start);
    cbuf_put(w, *cls, *buf);
    *end -= *start; *start = 0; *buf = nb; *cls = (u8)want;
}

#endif
