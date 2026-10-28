#ifndef REDIS_DB_H
#define REDIS_DB_H
// redis/db.h - C counterpart of redis/db.stra: the shared database. A hash table (chained), a binary min-heap of expiries, a channel table, and the
// reaper queue, all behind ONE parking RwLock (runtime/rt_sync.h). Written by hand because C has no HashMap/BTreeSet; this is the cost the Strata
// standard library would hide. Tables are sized once at startup for the declared limits (maxKeys, maxTopics), so a lock hold never includes a resize:
// the arrays are calloc'd, which the OS maps lazily, so unused capacity costs address space, not memory.
// Callers hold the lock (read for get/publish, write for the rest); none of these functions parks.
#include "rt_sync.h"

typedef struct Limits {
    u32 maxConns, maxFrame, maxBulk, maxKey, maxKeys, maxTopics, maxSubs, subQueue;
    u64 maxExpireSecs;
    int lagDisconnect;                       // 1: disconnect a slow subscriber, 0: send ["lagged", N]
} Limits;
static Limits L = { 250, 4194304, 1048576, 512, 1000000, 10000, 100, 1024, 315360000ull, 1 };

#define PURGE_BATCH 1000

// ---- Value: immutable bytes, shared by atomic count (Strata: Ref<Value>) ----
typedef struct Value { u32 refs, len; char d[]; } Value;
static Value *value_new(const char *p, u32 len) {
    Value *v = malloc(sizeof *v + len);
    v->refs = 1; v->len = len; memcpy(v->d, p, len);
    return v;
}
static Value *value_ref(Value *v) { __atomic_add_fetch(&v->refs, 1, __ATOMIC_RELAXED); return v; }
static void value_unref(Value *v) { if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL) == 0) free(v); }
static void msg_drop(Msg m) { value_unref(m.a); value_unref(m.b); }

static u64 hash_bytes(const char *p, u32 n) {            // FNV-1a
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < n; i++) { h ^= (u8)p[i]; h *= 1099511628211ull; }
    return h;
}

typedef struct Entry { struct Entry *next; u64 hash; Value *key, *val; u64 expires; u32 hidx; } Entry;   // expires: ns on the monotonic clock, 0 = none
typedef struct Topic { struct Topic *next; u64 hash; Value *name; Mailbox **subs; u32 n, cap; } Topic;
typedef struct Reap { struct Reap *next; Value *name; Mailbox *mb; } Reap;

typedef struct {
    RwLock lock;
    Entry **tab; u64 mask, count;
    Entry **heap; u32 hn;
    Topic **ttab; u64 tmask, tcount;
    u8 rlk; Reap *rhead, *rtail;                         // reaper queue (its own spinlock: pushed from cleanup, which cannot take the main lock)
    Bell bell;                                           // purge task: an expiry moved earlier, or the reaper queue has work
} Db;
static Db g_db;

static u64 pow2_at_least(u64 n) { u64 p = 1024; while (p < n) p <<= 1; return p; }
static void db_init(void) {
    u64 n = pow2_at_least(L.maxKeys), t = pow2_at_least(L.maxTopics);
    g_db.tab = calloc(n, sizeof(Entry *)); g_db.mask = n - 1;
    g_db.heap = malloc((L.maxKeys + 1) * sizeof(Entry *));
    g_db.ttab = calloc(t, sizeof(Topic *)); g_db.tmask = t - 1;
}

// ---- expiry heap, ordered by `expires`; each entry knows its index so it can be removed ----
static void hset(u32 i, Entry *e) { g_db.heap[i] = e; e->hidx = i; }
static void hup(u32 i, Entry *e) {
    while (i > 0) { u32 p = (i - 1) / 2; if (g_db.heap[p]->expires <= e->expires) break; hset(i, g_db.heap[p]); i = p; }
    hset(i, e);
}
static void hdown(u32 i, Entry *e) {
    for (;;) {
        u32 c = 2 * i + 1;
        if (c >= g_db.hn) break;
        if (c + 1 < g_db.hn && g_db.heap[c + 1]->expires < g_db.heap[c]->expires) c++;
        if (g_db.heap[c]->expires >= e->expires) break;
        hset(i, g_db.heap[c]); i = c;
    }
    hset(i, e);
}
static void heap_insert(Entry *e) { u32 i = g_db.hn++; hup(i, e); }
static void heap_remove(Entry *e) {
    u32 i = e->hidx;
    Entry *last = g_db.heap[--g_db.hn];
    e->hidx = 0xFFFFFFFFu;
    if (last == e) return;
    if (i > 0 && g_db.heap[(i - 1) / 2]->expires > last->expires) hup(i, last); else hdown(i, last);
}

static Entry *db_find(const char *k, u32 n, u64 h, Entry ***link) {
    Entry **pp = &g_db.tab[h & g_db.mask];
    for (Entry *e = *pp; e; pp = &e->next, e = e->next)
        if (e->hash == h && e->key->len == n && !memcmp(e->key->d, k, n)) { if (link) *link = pp; return e; }
    return NULL;
}

// GET (read lock). The returned Value is a new reference that outlives the lock.
static Value *db_get(const char *k, u32 n) {
    Entry *e = db_find(k, n, hash_bytes(k, n), NULL);
    return e ? value_ref(e->val) : NULL;
}

// SET (write lock). `key` and `val` are new Values the caller allocated BEFORE taking the lock; ownership passes to the table on success (return 0) and
// stays with the caller on 1 (maxKeys reached). *earlier is set when the earliest expiry changed, so the caller rings the purge bell after unlocking.
static int db_set(Value *key, Value *val, u64 expires, int *earlier) {
    u64 h = hash_bytes(key->d, key->len);
    Entry *e = db_find(key->d, key->len, h, NULL);
    u64 first = g_db.hn ? g_db.heap[0]->expires : 0;
    if (e) {
        value_unref(e->val); e->val = val;
        value_unref(key);                                  // the table keeps its original key
        if (e->expires) heap_remove(e);
        e->expires = expires;
        if (expires) heap_insert(e);
    } else {
        if (g_db.count >= L.maxKeys) return 1;
        e = calloc(1, sizeof *e);
        e->hash = h; e->key = key; e->val = val; e->expires = expires; e->hidx = 0xFFFFFFFFu;
        e->next = g_db.tab[h & g_db.mask]; g_db.tab[h & g_db.mask] = e; g_db.count++;
        if (expires) heap_insert(e);
    }
    *earlier = expires && (!first || expires < first);
    return 0;
}
static void db_remove_entry(Entry *e) {
    Entry **link; db_find(e->key->d, e->key->len, e->hash, &link);
    *link = e->next;
    if (e->expires) heap_remove(e);
    value_unref(e->key); value_unref(e->val); free(e); g_db.count--;
}

// Removes expired entries, at most PURGE_BATCH per lock hold. *next: the earliest remaining expiry (0 = none). Returns 1 if the batch was full.
static int db_purge(u64 now, u64 *next) {
    u32 n = 0;
    while (n < PURGE_BATCH && g_db.hn && g_db.heap[0]->expires <= now) { db_remove_entry(g_db.heap[0]); n++; }
    *next = g_db.hn ? g_db.heap[0]->expires : 0;
    return n == PURGE_BATCH;
}

// ---- channels ----
static Topic *topic_find(const char *k, u32 n, u64 h, Topic ***link) {
    Topic **pp = &g_db.ttab[h & g_db.tmask];
    for (Topic *t = *pp; t; pp = &t->next, t = t->next)
        if (t->hash == h && t->name->len == n && !memcmp(t->name->d, k, n)) { if (link) *link = pp; return t; }
    return NULL;
}
// PUBLISH (read lock): offers the message to every subscriber; a full mailbox refuses it and counts it. Returns the subscriber count.
static u64 db_publish(const char *ch, u32 n, Value *data) {
    Topic *t = topic_find(ch, n, hash_bytes(ch, n), NULL);
    if (!t) return 0;
    for (u32 i = 0; i < t->n; i++) {
        Msg m = { value_ref(t->name), value_ref(data) };
        if (!mb_push(t->subs[i], m)) msg_drop(m);
    }
    return t->n;
}
// SUBSCRIBE (write lock). 1: maxTopics reached.
static int db_subscribe(const char *ch, u32 n, Mailbox *mb) {
    u64 h = hash_bytes(ch, n);
    Topic *t = topic_find(ch, n, h, NULL);
    if (!t) {
        if (g_db.tcount >= L.maxTopics) return 1;
        t = calloc(1, sizeof *t);
        t->hash = h; t->name = value_new(ch, n);
        t->next = g_db.ttab[h & g_db.tmask]; g_db.ttab[h & g_db.tmask] = t; g_db.tcount++;
    }
    for (u32 i = 0; i < t->n; i++) if (t->subs[i] == mb) return 0;
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 4; t->subs = realloc(t->subs, t->cap * sizeof(Mailbox *)); }
    t->subs[t->n++] = mb_ref(mb);
    return 0;
}
// UNSUBSCRIBE (write lock). A channel with no subscribers is removed (the original keeps it forever).
static void db_unsubscribe(const char *ch, u32 n, Mailbox *mb) {
    Topic **link;
    Topic *t = topic_find(ch, n, hash_bytes(ch, n), &link);
    if (!t) return;
    for (u32 i = 0; i < t->n; i++) if (t->subs[i] == mb) { t->subs[i] = t->subs[--t->n]; mb_unref(mb); break; }
    if (t->n == 0) { *link = t->next; value_unref(t->name); free(t->subs); free(t); g_db.tcount--; }
}

// ---- reaper queue: cleanup() cannot take the lock (entering it parks), so it hands the subscription over here and the purge task unsubscribes it ----
static void db_release(Value *name, Mailbox *mb) {       // never parks
    Reap *r = malloc(sizeof *r);
    r->name = value_ref(name); r->mb = mb_ref(mb); r->next = NULL;
    SPIN_LOCK(g_db.rlk);
    if (g_db.rtail) g_db.rtail->next = r; else g_db.rhead = r;
    g_db.rtail = r;
    SPIN_UNLOCK(g_db.rlk);
    bell_ring(&g_db.bell);
}
static Reap *db_reap_pop(void) {
    SPIN_LOCK(g_db.rlk);
    Reap *r = g_db.rhead;
    if (r) { g_db.rhead = r->next; if (!g_db.rhead) g_db.rtail = NULL; }
    SPIN_UNLOCK(g_db.rlk);
    return r;
}
#endif
