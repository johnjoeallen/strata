# Sharing and access checking

One reference kind, `Ref<T>`. Whether an object is shared is not written anywhere. The compiler works out where a `sync` or `mutate` block is required, reports a missing or too-weak one as a compile error, and the blocks cost almost nothing on an object that was never shared. All examples are taken or adapted from `stra/redis/db.stra`.

Status: **Direction**. Replaces the open questions about "how shareability is proven or declared" (spec 10.2, 10.3, 7.3). Nothing here is implemented; the C beside the `.stra` is hand-written.

## The rules

**R1. When is a block required?** Reading or writing through a `Ref<T>` (or a borrow derived from it) needs a block naming that reference when **both** hold:

1. the object is not *provably local*, and
2. the accessed field, or anything stored by value inside it, is *mutable*.

**R2. Provably local.** The object was allocated in this function with `new`, and no reference to it has escaped so far on any path: it has not been passed to a function whose summary says it retains or publishes the argument, stored into a non-local object, captured by a closure, or given to `spawn` or a message. Everything else is *incoming*: a parameter, a global, a field of a non-local object, or a call result not marked fresh (below).

**R3. Mutable.** A field is mutable if anything outside object construction writes it, directly or through a method whose access summary says it writes its receiver (`entries.insert(...)` mutates `entries`). A field never written after construction is write-once, and reading it needs no block. A type whose fields are all write-once is immutable, and it needs no block anywhere.

**R4. Modes.** `sync` is read-only; `mutate` is exclusive. Writing needs `mutate`. A call made inside `sync` to a function whose summary writes the argument is an error: `needs mutate(store), have sync(store)`. This is checked even for an object that turns out never to be shared, so behaviour does not depend on sharing.

**R5. Publishing.** A reference to a local object crosses a vthread boundary when it is passed to `spawn`, sent in a message, stored in a global or in a non-local object, or captured by a closure that is. From that point the object is *shared*: the compiler treats the reference as no longer local on every later path, so R1 applies after it. At run time the object's shared flag is set before the handoff, and everything reachable from it is set too (or the compiler refuses the publish; see Open).

**R6. Cost on an unshared object.** `sync`/`mutate` test the flag once at entry. If it is clear, they do nothing. The flag only ever goes from clear to set, so the test at exit agrees with the one at entry.

**R7. Borrows.** A borrow taken inside a block cannot leave it (return, store, capture). A `T&` parameter needs no block in the callee: the caller either owns the object locally or holds the block (R4 checks its mode against the callee's summary).

**R8. Runtime types.** A type declared `extern` (Signal, Flag, Limiter, Mailbox, Wakeup) synchronises itself. Its methods need no block, whoever calls them and whether or not the object is shared; the `extern` declaration is a promise, as it is for parking. Only Strata-defined types follow R1 to R7.

**Fresh results.** A function whose result is a new allocation that nothing else refers to is marked `fresh` in its summary, so `let db = Db.make(lim)` is local in the caller until it escapes. Without `fresh` the caller must treat the result as incoming.

Mutability, escape (retains/publishes), access mode per parameter, and `fresh` are Tier A contract data ([modules](architecture/modules.md#access-and-escape-summaries)). Changing a field from write-once to mutable is a breaking change.

## Example 1: local use, no blocks

A test or a command-line tool builds a store and reads it back in one vthread. Nothing escapes, so there is nothing to lock.

```cpp
// tools/load.stra
package tools

fn loadAndCount(path: string&): usize {
    let store = new Store {}              // local
    for line in readLines(path) {
        store.entries.insert(Bytes.copyOf(line.key), Entry { value: new Value { bytes: line.value.copy() }, expiresAt: null })
    }
    return store.entries.count            // local: no sync, no mutate
}
```

Under the earlier `SharedRef` idea this would have needed a different type or a separate shared variant. Here it is the same `Store` and the same code as the server uses.

## Example 2: what `Db` needs and what it does not

```cpp
// redis/db.stra
package redis

struct Db {                               // every field is set in make() and never written again: write-once
    store: Ref<Store>
    purgeWake: Signal
    closed: Flag
    reaper: Ref<Mailbox<Reap>>
    lim: Limits
    // ...
    fn get(key: u8[]&): Ref<Value>? {
        sync(store) {                     // R1: store is incoming and Store.entries is mutable
            let e = store.entries.get(key)
            return e == null ? null : e.value
        }
    }

    fn[nopark] release(channel: u8[]&, mb: Ref<Mailbox<Message>>) {
        reaper.tryPush(Reap { ... })      // reaper is write-once and Mailbox is an extern handle (R8): no block; [nopark] lets cleanup() call it
    }
}
```
`stra/redis/db.stra` is written this way, with these notes in its header.

- `db.store` and `db.lim` are read with no block, because those fields never change after `make`. This is why the existing code already writes `sync(store)` and not `sync(db)`.
- `store.entries` is mutated by `set`, `purgeExpired` and others, so reads and writes of it go through `sync`/`mutate`.
- `Value` is "immutable once built". After `get` returns the `Ref<Value>` the caller reads `v.bytes` with no block, which is exactly what the comment in `db.stra` relies on when it writes the reply after the lock is released.

## Example 3: errors the compiler reports

```cpp
// redis/stats.stra
package redis

fn size(db: Db&): usize {
    return db.store.entries.count         // error: store.entries is mutable and db is incoming
}                                         //   fix: sync(db.store) { return db.store.entries.count }
```

```cpp
// redis/trim.stra
package redis

fn trim(store: Ref<Store>) {
    sync(store) {
        store.entries.remove(oldest(store))   // error: entries.remove writes its receiver
    }                                         //   needs mutate(store), have sync(store)
}
```

```cpp
// redis/peek.stra
package redis

fn peekKeys(store: Ref<Store>): Map<Bytes, Entry>& {
    sync(store) {
        return store.entries              // error: a borrow taken in a sync block cannot leave it
    }
}
```

The messages name the chain and the fix. A missing block on a reference that turns out never to be shared is still reported: that is the over-approximation in R2, and the cost is one extra block that does nothing at run time.

## Example 4: publishing, and what changes after it

```cpp
// redis/main.stra
package redis

fn main(args: Args&) {
    let db = Db.make(lim)                 // fresh: local so far
    db.purgeWake.notify()                 // Signal is a runtime handle, not db's mutable state
    spawn db.purgeLoop()                  // R5: db is published here
    let n = db.store.entries.count        // error: db is shared from the spawn on; entries is mutable
    serve(listener, db, lim, shutdown, live)
}
```

Before the `spawn`, `db` is local and could be inspected directly. After it, every access to mutable state follows R1. Moving the line above the `spawn` makes it legal, and correct, since no other vthread yet exists.

## Example 5: modes across a call

```cpp
// redis/expire.stra
package redis

fn expireAndCount(store: Ref<Store>): usize {
    mutate(store) {
        purgeBatch(store)                 // summary: purgeBatch writes param 0 -> mutate required: ok
        return store.entries.count
    }
}

fn statsOnly(store: Ref<Store>): usize {
    sync(store) {
        purgeBatch(store)                 // error: purgeBatch writes its argument; have sync(store)
        return store.entries.count
    }
}
```

The check uses `purgeBatch`'s summary, so it works when `purgeBatch` is in another module and its source is not available.

## What the module file carries

```
Db.get      self.store: read via sync, result: not fresh, throws: none, park: May
Db.make     result: fresh, retains lim
Store       entries: mutable   expirations: mutable   topics: mutable
Value       bytes: write-once  (type is immutable)
Db          store, purgeWake, closed, reaper, lim: write-once
purgeBatch  param 0: write
```

A consumer of `db.smod` can run every check above with no source. An implementation change that makes `Db.lim` mutable changes the contract hash and is reported as a breaking change, because every reader of `lim` now needs a block.

## Generated C for a block

```c
// sync(store) { ... } where store is incoming
bool sh = obj_is_shared(store);                  // one load of the control-block flag
if (sh) { st_t s = rwlock_read_enter(t, &store->lock); /* may park; PARKED unwinds as usual */ }
/* body: no parking inside, no slice checks */
if (sh) rwlock_read_exit(&store->lock);          // on every exit path, including throws
```

Reference counts follow the same split. An object the compiler proves local uses plain counts. A reference to a non-local object tests the flag: plain count if clear, atomic if set. That test per copy and release is a real cost, and the alternative (always atomic for non-local) is simpler and slower; `bench/c/refcount_bench.c` is where it gets measured (Open).

## Open

- **Publishing a graph.** Setting the flag on everything reachable costs a walk at the publish. Alternatives: refuse to publish an object whose graph has local-only parts that are not themselves being published; or make `spawn` and message sends take only values whose reachable graph is already shared or immutable.
- **Where the lock word lives.** In every control block (costs bytes on every `new`), or allocated at the first publish (costs an indirection).
- **Counts per instance.** A flag test in each count operation, or atomic always for non-local references. Needs `refcount_bench` on realistic access patterns.
- **Write-once across modules.** A field is write-once if no code in its defining module writes it. Public fields that other modules may write need either a mutability declaration or a rule that they are mutable by default.
- **Immutable types.** Inferred from R3 as above; whether to also let the programmer assert it (for a compile error if a later change breaks it) is undecided.
- **Runtime types that are not thread-safe.** R8 treats every `extern` type as self-synchronising. Whether an `extern` declaration may say otherwise (a handle that one vthread must own) is undecided.
- **Interfaces and function values.** A call through one needs a declared access mode per parameter, as it needs an effect bound. Default is "writes", the safe direction for the caller.
- **Lint.** A value read in a `sync` block and used to decide a write in a separate `mutate` block on the same object (the get-then-put race of spec 7.2). A warning, not an error.
