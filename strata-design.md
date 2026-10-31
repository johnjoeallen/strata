# Strata — Programming Language Design Specification

**Status:** exploratory design draft (not an implementation or language standard)  
**Origin:** independent language design informed by Goose’s compiler-managed stacks and virtual-memory techniques, Rust’s ownership guarantees, and virtual-thread runtimes  
**Primary goal:** memory-safe, high-performance concurrent server programming with a simpler programmer-facing ownership model than Rust.

> Strata is an independent language proposal, not a Goose fork or a Rust dialect. All Strata examples are illustrative and not yet implemented. Its compiler and runtime may reuse ideas or components from other projects where technically and legally appropriate.

## 1. Motivation

Goose offers a compelling model: compiler-managed lifetimes, native C generation, and large virtual-memory reservations that permit selected arrays to grow in place. Its strict thread isolation and limited dynamic allocation, however, constrain typical server applications.

Strata explores retaining those ideas where useful while supporting:

- Dynamically created objects and graphs with independent lifetimes.
- Many lightweight virtual threads executing ordinary sequential code.
- Efficient shared read-heavy state, including caches.
- Deterministic cleanup of files, sockets, locks, and other resources.
- Static reference-safety analysis without programmer-written lifetime names such as Rust's `'a`.

**Non-goal:** feature parity with Rust or support for every application architecture. Strata should prioritize understandable semantics over zero runtime overhead at all costs.

## 2. Design principles

1. Ordinary values should remain inexpensive and need no reference-count header.
2. Programmer-visible reference kinds express ownership; the compiler infers borrowing lifetimes.
3. Dynamic allocation is explicit through `new`, and produces a strong owning `Ref<T>`.
4. `WeakRef<T>` breaks strong-reference cycles by not keeping the object alive.
5. Synchronization is lexical and compiler-enforced; higher-level libraries hide it from application code.
6. Virtual-thread execution and memory allocation are separate concerns.
7. Native Strata APIs communicate compile-time lifetime contracts via module metadata.
8. Foreign calls without verified contracts remain explicitly unsafe.
9. Optimize only after establishing memory safety and observable behaviour.
10. **No magic or hidden behaviour, except virtual threads and parking.** What the code says is what happens:
    - Allocation (`new`), copying, locking (`sync`/`mutate`), and I/O are visible in the source. Nothing buffers,
      defers, batches, retries, or reorders on the programmer's behalf. `conn.write` sends now; buffering is an
      explicit type with an explicit `flush`.
    - Optimizations that change timing or cost, such as response batching or write coalescing, are library features
      that are opt-in, state their bounds (for example `maxBatch` and `maxHold`), and document what they delay.
      The runtime and compiler may not introduce them silently. A time bound checked between handlers cannot
      interrupt a running handler (there is no preemption), so such features also require handlers to declare
      whether they are bounded before anything is held behind them.
    - The compiler may optimize freely only where behaviour is unobservable (see 4.4); it must preserve identity,
      destructor timing and ordering.
    - **The one exception is parking.** Parking includes yielding at compiler-inserted slice checks, but the compiler inserts those only in a function marked `[yield]` (or in a type or package marked so); with no attribute it inserts nothing. A call that may wait looks like an ordinary blocking call: no `async`/`await`,
      no function colouring. The compiler infers which functions may park (the may-park effect) and sizes their
      frames. Because this is hidden by design, tooling must make it visible (hover, lints, and error messages that
      name the call chain), and parking inside `sync`/`mutate` is a compile error.

11. **Every input that can be unbounded has a declared limit, enforced before the data is held.** A header block, a request body, and a
    parsed document's depth and size all have limits stated by the program (or a documented default). Large payloads are streamed, or
    spooled to a file and mapped explicitly; they are never parsed into values whole. Exceeding a limit is an error the caller can see
    (431, 413), not a larger allocation.

## 3. Types and ownership

| Type / construct | Meaning | Lifetime / reclamation |
|---|---|---|
| `T` | Ordinary value | Compiler-managed scope, stack, or task storage |
| `T&` | Borrowed reference | Inferred, statically checked; cannot outlive its referent |
| `Ref<T>` | Owning reference to a dynamic object | Strong count; destroy object at zero |
| `WeakRef<T>` | Non-owning observation of a dynamic object | Weak count; control block survives until no weak owners remain |
| `string` | Immutable string value | Implementation-managed storage, reclaimed when unreferenced |
| `T[]` | Growable array | Compiler-managed or explicitly allocated backing storage; exact allocation policy TBD |

### 3.1 Dynamic objects

```cpp
Ref<Parent> parent = new Parent(...);
Ref<Parent> copy = Ref<Parent>(parent);
WeakRef<Parent> weak = WeakRef<Parent>(parent);
Ref<Parent> promoted = Ref<Parent>(weak); // throws if expired
```

`new T(...)` allocates a new independently managed object, creates its control block and returns `Ref<T>`. Copying a `Ref<T>` increments the strong count rather than copying the object. Converting `WeakRef<T>` to `Ref<T>` atomically increments the strong count *only if it is nonzero*; otherwise it raises `ExpiredReference`.

Direct `WeakRef<T>` access may optionally use implicit temporary promotion and raise `ExpiredReference` when expired; this convenience requires specification, especially for compound expressions. `weak.valid()` may check current liveness but **cannot guarantee later access** across threads. Safe multi-operation use requires promotion to a strong reference.

### 3.2 Parent–child relationships

```cpp
// app/parent.stra
package app

struct Parent {
    children: Ref<Child>[]
}

struct Child {
    parent: WeakRef<Parent>
}
```

The parent strongly owns children; children refer weakly to their parent. This avoids the usual parent–child strong-reference cycle. Reference counting alone does **not** collect arbitrary strong-reference cycles, so weak-edge design remains necessary.

### 3.3 Control blocks

Conceptual layout for a dynamically allocated `Parent`:

```text
Parent arena slot
+------------------------------+
| atomic strong count          |
| atomic weak count            |
| control / lifetime metadata  |
+------------------------------+
| Parent fields                |
+------------------------------+
```

Strong count reaching zero triggers object destruction and resource cleanup. The slot/control block remains until the final weak owner has been released; implementations can use an implicit weak owner while strong owners exist. Promotion must use an atomic conditional increment, not a separate validity check followed by increment. Count overflow and destruction races must be handled.

`Ref<T>[]` is an array of handles, **not** inline `T` objects. Its elements point to objects in the `T` arena; removing an element releases its strong ownership. The array's backing storage is separate.

## 4. Memory allocation

### 4.1 Distinct storage classes

| Storage | Allocation approach | Reclamation |
|---|---|---|
| Simple locals | compiler-managed data stacks inspired by Goose | Scope exit |
| Task temporaries | Virtual-thread-local arenas, with optional nested scopes | Task or sub-scope exit |
| `new T` | Shared, type-specific slab/arena for fixed-size objects | Strong/weak control-block lifecycle |
| Variable-size buffers | Size-class allocator or other general variable-size allocator | Owner/buffer lifetime |
| Growable arrays | Dedicated virtual address regions where worthwhile; alternative managed buffers for many small arrays | Array lifetime |
| String literals | Static read-only storage | Process lifetime |
| Runtime strings | Shared compact allocations or inline representation | Last owner / value lifetime |

All allocators may exploit large virtual-address reservations, committing memory pages only as needed. **Virtual-address reservation is a technique, not an ownership model.** Large reservations per object are inappropriate for millions of small strings or arrays.

### 4.2 Per-type arenas

A `new Parent(...)` allocation comes from a shared `Parent` arena, regardless of which virtual thread requested it. The arena is runtime-owned, not task-owned. Each slot contains the fixed-size object and reference-counting metadata. A type arena may lazily allocate slabs and maintain per-OS-worker free-slot caches; cross-thread deallocation must be supported.

Variable-sized fields such as strings and array buffers need their own backing allocations; they do not automatically fit in fixed-size parent slots.

### 4.3 Strings

Proposed default: immutable `string` values with compiler-understood semantics but runtime-provided storage and operations.

- Literals may reside in static read-only data.
- Short runtime strings may be inline (small-string optimization).
- Larger strings may share immutable reference-counted backing data.
- Builders provide mutable construction before producing an immutable string.
- A size-class allocator is preferable to reserving a huge region per string.
- Strings in arrays and structs require correct type-aware cleanup when removed or destroyed.

The exact string representation, encoding, equality and substring ownership semantics remain to be specified.

### 4.4 Escape analysis

The compiler may place objects in task-local storage only if it proves they cannot outlive the task or be observed through escaping strong/weak references. It must preserve identity, destructor timing, weak-reference semantics and synchronization behaviour; it must not optimize by silently invalidating handles. Start with independently reclaimable allocations for all `new` objects and treat placement optimizations as later work.

## 5. Reference and lifetime analysis

Lifetime parameters are **not** written as named generics. Borrowing is expressed with `T&`; the compiler infers which parameters, object fields or static objects the returned borrow depends on.

```cpp
// app/choose.stra
package app

fn choose(a: Customer&, b: Customer&, first: bool): Customer& {
    if first { return a; }
    return b;
}
```

The inferred contract records that a returned reference may originate from either input. The caller must not retain it beyond the safe common lifetime. This information is needed only by the compiler; no runtime lifetime query facility is required.

### 5.1 Separate compilation

A compiled native Strata library exports an interface artifact containing type definitions, generic information, borrow origins, relevant mutation/escape effects, and lifetime constraints. For example:

```text
choose(Customer&, Customer&, bool) -> Customer&
return origins: parameter 0 | parameter 1
```

Such metadata is a **compile-time dependency**. The executable need not contain it. Changing an inferred contract may be an API compatibility change even when the source signature stays the same.

The contract also records each function's **effect summary** (may-park, `throws`, foreign blocking) and is carried in a compiled module format with retained IR, so that downstream modules and the application link can be checked and optimised without the original source. See section 14 and `docs/architecture/modules.md`.

### 5.2 C interoperability

C declarations contain no trustworthy ownership or lifetime semantics. Bindings must supply explicit contracts for returned or retained pointers, transferred ownership, deallocation, and invalidation; otherwise pointer-based FFI operations are unsafe. A wrapper can make a copy or acquire ownership when the C API's documented guarantee permits it. Strata must not infer guarantees from an opaque C function body it cannot inspect.

## 6. Virtual threads

Strata aims for Java-virtual-thread-style programming: thousands of logical tasks running ordinary blocking-looking code on a limited number of OS workers.

```cpp
// app/serve.stra
package app

fn serve(listener: Listener&) {
    loop {
        let socket = listener.accept();
        vthreadSpawn(handleClient, socket);
    }
}
```

The **programming model** is virtual threads with stackful-like semantics: locals live across a wait, `try` and cleanup span it, a cancelled task unwinds. The **implementation model** is separate (6.1). No language feature may depend on which implementation is used.

### 6.1 Implementation: compiler-generated resumable frames

*Direction; supersedes the earlier stackful-fiber plan. Rationale, lowering and limits: `docs/architecture/suspension.md`.*

- The compiler infers the **may-park effect** (principle 10) transitively over the call graph. A function that cannot park compiles to an ordinary C function. A function that may park is lowered to a state machine whose frame holds exactly the values live across its park points.
- Preferred shape: a native-stack fast path, with frames created only while a real park unwinds, and the scheduler resuming the innermost frame directly. Eager and embedded frames are alternatives chosen per function. Frames never move.
- No `async`/`await`: suspension capability propagates through the call graph without syntax at each level.
- **Slice checks (decided, opt-in):** a function marked `[yield]`, or any method of a type written `struct T[yield]` (and later any function in a package so marked; the innermost level wins), gets a cheap check on its loop back-edges and recursive calls; when the slice runs out the task yields, and the check is also a cancellation point. `[noyield]` overrides an enclosing default. `[nopark]` asserts the function never parks, so any park source in it is a compile error. With no attribute the compiler inserts nothing, and loops add no effect. Code in `sync`/`mutate` bodies, cleanups and C callbacks may call only `Never` functions. A lint warns about unbounded and long loops (`loops` class in the module summary). Overhead of a check is unmeasured (`docs/architecture/benchmarks.md`, B13).
- Scheduling: an OS-thread worker pool of configurable size, nonblocking sockets plus `epoll` on Linux, scheduler-aware `read`, `write`, `accept`, timers and waits. Synchronization waits **park virtual threads**, not carrier threads. Task-local globals and arenas belong to the vthread. Start with thread-affine tasks; migration and work stealing come later.
- Blocking native calls are explicit: a plain `extern fn` is a promise of a short, nonblocking call; a call that may block is declared as such and is run by the runtime on a blocking pool (`docs/architecture/dispatch-and-ffi.md`). A blocking call must never silently stall a worker.
- Conservative contracts: calls through interfaces and function values compile against a declared effect bound (default may-park), so a later change of an implementation cannot make a compiled caller unsafe.
- The earlier **stackful fibers** (independently managed stacks, guard pages, ABI-correct switching) remain as the reference implementation, as the memory baseline, and as the synchronous driver for foreign callbacks on pool threads.
- Measured so far (hand-written C only, not compiler output): an idle parked task is about 137 B with no state against a 4 KB stackful floor; a parked call costs about 1 ns beyond an ordinary call; waking the innermost frame stays flat with depth. No performance superiority over Java virtual threads or Rust async is claimed; the evaluation plan is `docs/architecture/benchmarks.md`.

Relative to Java virtual threads: no stack copying, no JVM, exact frames; but foreign frames cannot be suspended, dynamic code is harder, and stack traces need maps. Relative to Rust async: no `async` colouring, no `Pin`, uniform dynamic dispatch; but suspension is invisible in source (tooling must show it) and the compiler carries more analysis.

### 6.2 Scope and lifetime

A virtual thread's **execution stack**, **task temporary arena**, and **reference-counted dynamic objects** are distinct. Task exit may reclaim the first two, but it cannot destroy `Ref<T>` allocations still owned by other tasks.

Cancellation is cooperative: wake the task, unwind/execute cleanup safely, then reclaim its execution and task-local storage. A suspended task must not simply have its memory discarded.

### 6.3 Per-request child vthreads (proposal, prototyped in C)

A connection may run pipelined safe requests as child vthreads in a scope that waits for them, and send the completed
prefix of responses in request order (RFC 7230 6.3.2). This makes response batching a consequence of the structure (a
response waits only behind an unfinished earlier one) instead of a library hold with timers. Non-safe requests and
routes declared unbounded act as barriers; depth is bounded. It overlaps handlers only where they park: a CPU-bound
handler on a pinned worker is not helped. Measurements: `bench/results/http.md`.

### 6.4 Suspension effects at module boundaries

*Direction.* Every exported function's contract includes `park: Never | May | Cond(P)`, its `throws` set and whether it reaches a blocking foreign call. The effect is inferred, exported, and checked at link time. Widening an effect (`Never` to `May`) is a breaking change even when the signature text is unchanged; tooling reports the contract diff. `[nopark]` asserts a function never parks; it turns an accidental widening into a compile error. The summary also carries an advisory `loops` class (none, bounded, long, unbounded) for loop warnings; it is not part of the contract hash. Cancellation is delivered at park points as a non-catchable unwind that runs cleanups. Details: `docs/architecture/suspension.md` and `docs/architecture/modules.md`.

## 7. Shared state and synchronization

Two constructs control access to shared mutable objects:

```cpp
sync(cache) {
    // Concurrent read-only access.
}

mutate(cache) {
    // Exclusive read/write access.
}
```

The compiler rejects mutation inside `sync`, unsynchronized access to objects that may be concurrently mutated (precisely defined in 7.3: an object that is not provably local, through a mutable field), and borrowed references that escape a protected block. References used *within* the block may be borrowed; values crossing the boundary require an independent lifetime (e.g., copy/move or a safely acquired owning reference). Deep copies must be explicit in the semantic specification for compound values, even if the source syntax elides the operation.

Locks are automatically released on every exit path, including errors, exceptions and any supported non-local returns.

### 7.1 Admission and fairness

Requested ordering policy: fair FIFO admission of access requests, batching consecutive readers, with each writer exclusive. New readers arriving behind a waiting writer cannot overtake it; later writers cannot indefinitely overtake waiting readers. This avoids starvation caused solely by admission policy, though a non-terminating holder can still block progress.

**Waiting parks the vthread.** Entering `sync`/`mutate` is a parking operation: if the lock is held, the vthread joins the lock's FIFO queue and parks, and the worker runs other vthreads. Unlock wakes the next waiter(s) on whichever worker they belong to. An implementation may spin for a bounded time before parking, because hold times are short (the body cannot park); the contract is unchanged and only the cost differs. This matters: with FIFO fairness, one queued writer makes every following reader queue behind it, so a pure park lost 55% of throughput at a 10% write mix and 75% at 50% writes (4 workers, one lock), and 2000 rounds of spinning removed the loss (`bench/results/miniredis.md`). The body of the block still cannot park (compile error), so a holder never yields the lock to the scheduler, two vthreads on one worker never contend, and hold time is bounded by the code in the block. Because compiler-inserted slice checks are park points (6.1), the body may call only functions whose effect is `Never` (inferred, or asserted with `[nopark]`), and no slice check is inserted in it. Allowed and rejected calls, the reasons a body may not park, and the loop warning (an unbounded loop under a lock always warns) are in `docs/architecture/suspension.md`, sections 1.2 and 1.3. Consequences: a function containing `sync`/`mutate` is inferred to park (tooling shows it at the block); a `cleanup()` cannot enter one (cleanup never parks), so cleanup uses non-parking operations (atomic flags, `tryPush`) and leaves the locked work to a later, parking pass.

**Multi-object blocks.** `mutate(a, b)` and `sync(a, b)` (any mix, e.g. `mutate(a) sync(b)` written as one head) acquire all their objects before the body runs or not at all, so mis-ordered blocks elsewhere (`mutate(b, a)`, `sync(b, a)`) cannot deadlock with them. The runtime tries the locks in a fixed global order: the address of each object's lock word, or `(arena id, slot index)` for an object in an arena whose storage can move. The order only has to be consistent among live locks being acquired together (the acquirer holds a `Ref` to each, so none can be freed or reused meanwhile), so no per-allocation sequence number exists. Arenas do not compact, and a chunk is released only when it is empty, so a live object's address never changes; an object has a lock word only once it is shared (7.3), and an object that is never shared pays nothing for it. If one is busy, it releases everything it took, parks on THAT lock's queue, and retries from the start when woken. Waiting on the blocker's queue replaces a timed backoff: there is no sleep, no spinning and no livelock, and a retrying acquirer keeps its original arrival ticket, so it is served ahead of later arrivals (no starvation by repeated losing races). Retrying is safe only because no body code has run yet. The same reason is why **nested** acquisition (`mutate(a) { mutate(b) { } }`) is a compile error when the inner object is not already in the outer head: a failed inner acquire could not undo what the outer body did. Same lock twice in one head or nested is also an error (no reentrancy). Upgrading read to write is not supported: release and acquire as `mutate`, and recheck.

### 7.2 Encapsulation in libraries

Application code should not need to write synchronization blocks for each cache operation:

```cpp
// app/cache.stra
package app

fn cacheGet(cache: Ref<Cache>, key: i64): i64 {
    sync(cache) {
        return cache.entries.get(key); // scalar copied before unlock; the block is required because cache is incoming and entries is mutable (7.3)
    }
}

fn cachePut(cache: Ref<Cache>, key: i64, value: i64) {
    mutate(cache) {
        cache.entries.insert(key, value);
    }
}
```

Callers use `cacheGet` and `cachePut` like ordinary functions. Read-modify-write actions require their own atomic API operation, not a separate get followed by put. Encapsulation also requires module visibility/access-control rules to prevent bypassing protected operations.

`Ref<T>` governs object *lifetime*; `sync` and `mutate` govern *concurrent access*. Neither substitutes for the other.

### 7.3 Sharing and access checking

*Direction; full rules and realistic examples in `docs/sharing.md`. Supersedes the earlier open choice between `shared var` and a shareable `Ref<T>`.*

There is one reference kind, `Ref<T>`, and no declaration of sharedness. The compiler decides where a block is needed:

- Access through a `Ref<T>` needs a `sync`/`mutate` block naming it when the object is **not provably local** (not allocated in this function, or a reference to it has escaped) **and** the accessed field is **mutable** (written after construction, directly or by a method that writes its receiver). Fields never written after construction, and types made only of them, are read without a block.
- `sync` is read-only. Calling a function whose summary writes the argument inside `sync` is a compile error naming the chain (`needs mutate(x), have sync(x)`), checked from the callee's contract, so it works across separately compiled modules.
- Passing a local object to `spawn`, a message, a global, a non-local object or an escaping closure **publishes** it: the compiler treats it as non-local from that point, and at run time its shared flag is set before the handoff. The flag only goes from clear to set.
- On an unshared object `sync`/`mutate` test the flag once and do nothing. Behaviour never depends on whether an object is shared, only cost does.
- Borrows taken inside a block cannot leave it.

Module-level variables stay vthread-local by default. A global that must be process-wide holds a `Ref<T>` that has been published; there is no `shared var` keyword.

Open: publishing a graph, where the lock word lives, per-instance reference-count mode, write-once fields across modules, interface access modes (`docs/sharing.md`, Open).

## 8. Messaging

Consider typed `qput` / `qget` message passing inspired by Goose. A possible incremental extension adds addressed messages and request/reply correlation:

```cpp
let pending = qput(cacheOwner, CacheRequest { key: 100 });
let reply = pending.response<CacheReply>();

// Owner side:
let message = qget<CacheRequest>();
message.respond(CacheReply { value: 42 });
```

The runtime attaches sender/destination identity and a unique request token; replying should be single-use. This solves the type-wide queue routing problem where unrelated consumers can take each other's replies. It retains a shared-nothing option for applications that prefer owner-worker messaging.

A single cache owner serializes all lookups; sharding by key provides parallelism across partitions but not concurrent reads of the same hot partition. A shared-memory cache with `sync`/`mutate` better serves some read-heavy workloads. Both mechanisms can coexist.

## 9. Deterministic resource cleanup

Memory reclamation and resource cleanup are separate concerns.

- File and socket handles close when their owning scope ends.
- DB connections return to pools on scope exit.
- `sync`/`mutate` permissions release automatically.
- A resource must not be implicitly copied if that causes double-close.
- Cleanup must run on normal return, errors, exceptions, cancellation and non-local returns.
- Task-level cleanup occurs **before** task arena/stack reclamation.
- Cleanup never parks: it runs to completion without waiting. A type that holds unsent data (for example a batching
  response writer) makes one best-effort, non-blocking attempt when dropped; code that needs the data to be sent calls
  an explicit `flush()` (which may park) before the value goes out of scope.
- Ending a network connection from the server side is a hand-off, not a wait: `Conn.lingerClose(time, maxBytes)` consumes the connection and returns at once; the
  runtime half-closes it, discards the client's remaining input for a bounded time and byte count, and then closes it. A program uses it when the server ends the
  connection while the client may still be sending (always after an error answer; after a normal `Connection: close` only if input is unread), because closing a socket
  that holds unread input makes the kernel reset the connection and the client can lose the response. The policy lives in the program (`runConnection`), the mechanism in the runtime.

A resource type may define an automatically invoked cleanup method. The exact ownership-transfer syntax is not yet chosen. Cleanup must be deterministic, must run exactly once, and must not depend on a tracing garbage collector.

## 10. Key unresolved questions

1. **Compiler implementation:** Build a fresh compiler or adapt a mature front-end/backend, and which Goose virtual-memory techniques are independently worth reproducing?
2. **Ref thread safety:** Direction (7.3): sharing is per instance, set when a reference is published across a vthread boundary; compiler-proven local objects use plain counts. Open: the cost of a per-reference flag test against always-atomic for non-local references (`docs/sharing.md`).
3. **Access model:** Direction (7.3): a block is required where the object is not provably local and the field is mutable; shareability is inferred, never declared. Open: write-once fields across modules and interface access modes.
4. **Arrays:** When should a growable array use a dedicated virtual region rather than a relocatable managed buffer? How are thousands of independently sized arrays represented?
   *Direction chosen for small bounded buffers (prototyped in `bench/http`, `stra/http`, `c/generated/http`):* the runtime provides `u8[]` as a pooled size-class array
   (512 B, 2 KB, 8 KB, 16 KB in the prototype). It starts in the smallest class, compacts when full, and `grow()` copies it into the
   next class; the source states no size, only a limit where one matters (a maximum header block). Code that needs a stable
   address must use offsets (a request is a pair of offsets), so moving is safe. Large and unbounded arrays (bodies, big
   collections) are partly decided for request bodies (prototyped), and the decision is a runtime-library feature, not a language one: two readers.
   The standard reader reads a body into memory up to a small configurable maximum (1 MB by default, for typical API servers) and needs no files.
   A route opts into the upload reader for file uploads and other large bodies; it streams the body into an unnamed temporary file under its own, much
   larger limit and hands the handler the finished file to link, read or map explicitly. Both refuse an oversize body with 413 before it is held, and
   nothing parses a body of that size into a value; large documents get a streaming parser over the file. All the limits, the upload directory and the
   fairness quantum (how many body bytes a connection processes before it yields) are runtime configuration passed in by the host.
Dedicated virtual reservations, as in Goose, remain the candidate for large in-memory arrays and are not built or measured.
5. **Strings:** Exact representation, slicing semantics, UTF-8 indexing, allocation and value-copy costs.
6. **Reference cycles:** Document weak-reference discipline; consider optional diagnostics, not automatic cycle collection initially.
7. **Exception model:** Interaction among thrown errors, normal returns, non-local returns, destructors and task cancellation.
   *Direction chosen:* errors are exceptions (`throw`, `try`/`catch`), not result values with a propagation marker. A function declares
   what can escape it with `throws A, B`; the compiler checks the list, so throwing is visible in signatures (which also answers
   the tension with principle 10: exceptions are control flow, but declared control flow). No `throws` clause means nothing
   escapes. A throw runs the cleanups of every scope it leaves; cleanup methods never park and never throw. `spawn f(...)`
   requires f to declare no `throws`, so a vthread's root function handles its own errors. Open: how a throw interacts with a
   scope that still has child vthreads (the prototype source assumes the scope waits for them first), and whether a handler
   type such as `fn(u8[]&): Resp throws Exception` is the right way to say "may throw anything".
   *Syntax direction (illustrative, used in `stra/http/server.stra`):* nullable `T?` with null-check narrowing instead of an
   Option type; methods declared inside the type, with `self` available but optional (needed only to disambiguate a member from
   a parameter and to refer to the value itself); tagged enums with an exhaustive `switch` whose arms are `Pattern -> { ... }` (no `case` keyword, no fallthrough, commas to list several
   values, `default` required for non-enum switches; ranges, guards, bindings and destructuring deferred) (`enum Cost { Bounded(Duration) Unbounded }`);
   `let`/`var`; camelCase function names.
8. **Virtual-thread stacks:** Largely superseded by 6.1 (resumable frames). Remains for the stackful baseline and for fast-path native-stack depth in recursive functions. Related open items (lazy frames with borrowed locals, FFI, fusion): `docs/architecture/open-decisions.md`.
9. **Synchronized callbacks and escaping values:** How to handle callbacks, function pointers, composite references and nested transactions safely.
10. **API stability:** Direction: contract hashes per symbol, format/ABI versions, contract diffs and a rule that widening an effect is breaking (`docs/architecture/modules.md`). Open: the enforcement policy and the spelling of the optional effect assertion.
11. **Compilation architecture:** where continuation lowering happens, the minimum IR for cross-module optimisation, fusion across modules, debugging maps, dynamic loading (`docs/architecture/open-decisions.md`, D1 to D14).

## 11. Suggested implementation sequence

### Phase 0 — Feasibility spike

Prototype a standalone Strata front-end and minimal C emitter. Study Goose’s compiler-managed storage and test borrowed-return analysis and independently allocated type slots. Select reusable implementation ideas based on measured feasibility, without assuming source-level compatibility.

### Phase 1 — Ownership foundations

Implement `Ref<T>`, `WeakRef<T>`, `new`, control blocks, typed arenas, destructor generation, weak promotion and negative tests for stale references. Use atomic counts initially.

### Phase 2 — Strings and dynamic collections

Implement a variable-size allocator, immutable strings, array-of-Ref cleanup and a stress test of many small arrays and object graphs. Verify no unbounded arena retention.

### Phase 3 — Scoped shared access

Implement `sync` and `mutate`, a scheduler-compatible reader/writer admission queue, copy/escape rules and generated cleanup on every control-flow exit. Add a shared cache benchmark.

### Phase 4 — Virtual-thread runtime

Implement the cooperative scheduler, timers, `epoll`-based I/O and a bounded native carrier-thread pool, with tasks as sized resumable frames (hand-written C stands in for compiler output until Phase 5). Keep the stackful runtime as a reference for behavioural tests. Add cancellation and structured cleanup. Benchmark tens of thousands of mostly idle connections.

### Phase 5 — Library interfaces and FFI

Export inferred lifetime/effect contracts at compilation, in the module format of section 14. Support separately compiled libraries and safe C-binding wrappers with explicit contracts. Order: typed IR; local effect inference; Level-0 lowering to C per module; module files with Tier A contracts; incremental builds; only then retained IR and optimised application compilation (inlining, devirtualisation, fusion), each justified by a measurement.

### Phase 6 — Real application validation

Build an HTTP service with dynamic request objects, a shared cache and file/socket lifetimes. Measure latency, throughput, peak resident memory, virtual memory consumption, contention and allocator behaviour against a comparable Rust/Tokio or Java virtual-thread implementation, following `docs/architecture/benchmarks.md`: the target is the same performance class with less source ceremony, not superiority.

## 12. Success criteria

Strata should demonstrate that an ordinary concurrent server can be written with:

- Readable procedural code and no named lifetime annotations.
- Memory safety without a tracing garbage collector.
- Safe shared ownership through `Ref<T>` / `WeakRef<T>`.
- Compiler-enforced, encapsulated synchronization.
- Scalable logical tasks with straightforward blocking-style I/O.
- Predictable resource cleanup and measured memory overhead.
- Performance in the same class as hand-tuned Rust async or Java virtual-thread code, with less source ceremony. Superiority is not a goal and is not claimed without evidence.

Strata remains an independent language regardless of the compiler implementation chosen. Reuse Goose-inspired virtual-memory techniques only where benchmarking and simplicity justify them.

## 13. Naming and implementation identity

- **Language:** Strata
- **Proposed command:** `strata`
- **Proposed source extension:** `.stra`
- **Suggested repository name:** `strata-lang` (availability not verified)
- **Suggested compiler backend for initial prototype:** generated C; not a permanent ABI commitment. C remains the native code-generation backend under the module architecture of section 14.
- **Compiled module extension (placeholder):** `.smod`; bundle `.sar`.

The name and syntax are working choices. No availability, trademark or namespace clearance has been completed.

## 14. Compilation architecture

*Direction. Full text: `docs/architecture/`. This section is a summary and the specification binds only what is marked Decided there.*

```text
Strata source -> parse/semantic analysis -> typed Strata IR
   -> compiled module (.smod): contract + retained IR + effect summaries + call graph + optional native cache
   -> application link: module resolution, cross-module effect and lifetime checks,
      [optimised mode] devirtualise, inline, fuse frames
   -> continuation lowering -> generated C -> C compiler and linker -> executable
```

0. **Packages.** Java folder convention: a package is a folder, named by its path under the source root (`http.styles.cpp`), and every file in it starts with the same `package` line. A package compiles to one `.smod`; declarations are visible across the package with no import, and `import other` names another package. The package line may carry `[yield]`, `[noyield]` or `[nopark]` as the default for its functions, repeated identically in every file, overridden by a type and then by a function. Placeholder syntax; details in `docs/architecture/open-decisions.md` (D14).
1. **Module contents come in tiers.** Tier A (contract) is required for correctness; Tier B (generic and exposed bodies) for instantiation; Tier C (the rest of the IR, call-graph edges, park-point tables) only for optimisation. A program links correctly with Tier A and the Level-0 ABI alone.
2. **Level-0 ABI** is the stable boundary: calling convention, `DONE/THROWN/PARKED` status, the frame header, vtable layout, exported type layout. A function's frame *body* is private, so changing it never breaks callers.
3. **Two build modes.** Incremental: each module compiled and lowered independently against imported contracts; unchanged contract hashes stop the rebuild. Optimised: whole-application analysis over retained IR; re-lowers only chains it changes.
4. **Recompilation** is triggered by the module's own change, a changed contract hash of a symbol it uses, a compiler/ABI/flag change, or (optimised mode) a changed inlined body.
5. **Calls that cannot be resolved** (interfaces, function values, generics across modules, foreign code, loaded modules) compile against declared bounds, never against current implementations.
6. **Performance goal:** excellent, not superior; the programming model is the point.
