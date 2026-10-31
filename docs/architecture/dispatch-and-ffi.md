# Dynamic dispatch and external code

Rule for everything on this page: **a call whose target cannot be fixed at compile time is compiled against a declared bound, never against what the implementation happens to do today.** Later releases of an implementation may then change freely inside the bound, and a program that compiled keeps being valid.

## Interface calls

An interface method has a declared **effect bound**: the widest effect any implementation may have.

- **Default bound: `May`** (Direction). Safe, and cheap because a `May` call that does not park costs about a call plus a status test (Measured, toy). A bound is written only when the author wants to narrow it, using the same optional `noparks` assertion as for functions (placeholder syntax; [D4](open-decisions.md#d4-representing-effects-in-public-interfaces)).
- **Implementations are checked against the bound** when they are compiled. An implementation can be narrower than the bound, never wider. An implementation of a `noparks` method that calls a `May` function is a compile error in the implementing module.
- **Callers** compile the call against the bound. A caller of a `May`-bound method uses the suspendable form regardless of the implementation.
- **Vtable entries** for `May` methods hold the `f_call`/`f_resume` pair. An implementation that is `Never` registers a thunk that returns `DONE`, so the calling code is the same.
- **At link time**, if the closed-world call graph shows one reachable implementation, the call may be devirtualised and the thunk and the status test removed. If several implementations exist, the bound still holds; any narrowing is an optimisation of the call site (a switch over known targets), never a change of contract.

This is the answer to "previously valid programs becoming unsafe when implementations change": the check that matters happens in the implementing module against a bound the caller already relied on.

## Function values and callbacks

A function-typed value (`fn(u8[]&): Resp throws Exception`) carries a bound the same way. Unannotated, it is `May`. A value that can be shown `Never` at the point where it is created (a lambda with no parking calls) can be passed where a `May` bound is expected. The reverse needs the assertion.

Functions that take function-typed parameters have a conditional summary `Cond(P)`: `forEach(list, f)` parks iff `f` may. At a call site where `f` is statically known, the condition collapses and the call binds to the plain or the suspendable form. At one where it is not, the parameter's declared bound is used.

Callbacks that the **runtime** invokes (a handler, a timer) are `May` by contract; the runtime calls the `f_call`/`f_resume` pair.

## Generic functions

**Direction:** monomorphise (one copy per type argument set, as in C++ and Rust), because it gives each instance exact frame layouts and an exact effect. It needs Tier B IR from the defining module, and instances land in the using module's object (or a link-time instance cache).

A generic function's summary is symbolic in the type arguments: `Cond({T})` if it calls a method of `T`, whose bound comes from the constraint. Resolved at instantiation. The generic body's IR is part of its contract hash, so editing a generic body forces dependants that instantiate it to recompile.

**Open (D6):** shared generic code with a dictionary, which avoids code growth and makes generics independent of IR retention but costs speed and requires uniform frame layouts for generic locals.

## Separately compiled libraries

Libraries are compiled modules. Their interfaces are Tier A. The rules in [modules](modules.md) cover: contract hash checks at link; widening an effect or layout is breaking; implementations behind interfaces change freely within the bound.

A library that is only available as a native object and header (no `.smod`) is a **foreign** library and takes the rules below.

## Native C interoperability

C declarations carry no ownership or effect information (spec 5.2). A Strata binding must state it:

```
extern fn crc32(data: u8[]&) -> u32                  // plain: Never. Assumed short and non-blocking.
extern parks fn sleep(d: Duration)                   // existing: a runtime primitive that registers and parks
extern blocking fn pqExec(c: PgConn&, sql: string&) // proposed: may block the OS thread (see below)
```

An `extern fn` without a modifier is a **promise** that it returns quickly and does not block. The compiler cannot verify the promise and nothing in the runtime enforces it. A violation stalls one worker for the length of the call (the same failure mode as a Java thread stuck while pinned); a watchdog may report it (see the table).

`extern blocking` is the one proposed new keyword in this update (**Open**; placeholder). It states that the call may block the thread, and is the only way a binding can say so. It carries `foreign = blocking` and `park = May` in the summary: the Strata caller sees an ordinary suspending call.

## Blocking foreign calls

A foreign C function is native stack that cannot be captured, so it cannot be suspended in the middle. Strategies, in order of preference:

| Strategy | What happens | Cost and limits |
|---|---|---|
| **1. Nonblocking binding** (best) | Use a readiness/async API of the library (nonblocking sockets, `epoll`, `io_uring`); the Strata wrapper calls it and parks on the descriptor. | Library must offer it. No extra thread. |
| **2. Offload to a blocking pool** (**Direction** for `extern blocking`) | The call is a park point: the vthread parks, a pool thread runs the call, and a completion wakes the vthread. | A thread hop per call (some microseconds); a bounded pool, so a full pool queues work (and that wait is itself a park). Arguments must outlive the park: borrowed arguments must be **frame-resident**, not on the native stack (same rule as [here](suspension.md#borrows-and-locals-that-cannot-be-moved)). Thread-affine libraries need a pinned thread (strategy 3). |
| **3. Dedicated thread** | A thread owns a library (a connection, a context) and Strata sends it work and awaits replies as messages. | Serialises that library's calls; good for thread-affine code. |
| **4. Run on the worker** | Plain `extern fn`: the call blocks the worker. | Only right for calls that are truly short. Stalls everything on that worker. |
| **5. Compensating worker** | A monitor notices a worker stuck in foreign code past a threshold and starts another worker so the scheduler keeps a core busy. | Does not fix the stuck call. Useful as a backstop and as a diagnostic, not a design. |

Recommendation: 1 where available, 2 as the default for anything declared `extern blocking`, 3 for thread-affine libraries, and a watchdog (diagnostic only, 5's detection without its remedy) for promises broken under 4. All are **Open** until measured: cost of the thread hop is exactly the kind of figure the [evaluation plan](benchmarks.md) must record.

## Foreign callbacks

C code calls a Strata function (a `qsort` comparator, a library event callback). The callback runs on whatever native stack C is using.

- **Default (Direction):** a callback is compiled as `Never`. Calling a `May` function from it is a compile error. Because a callback is invoked from C, it is `f_call`-only: nothing underneath it can be captured.
- **Exception: callbacks that must park.** Declared as parking callbacks, they run on a **blocking-pool thread only**, under a **synchronous driver**: the wrapper calls `f_call`; on `PARKED` it blocks the OS thread on a wake-up object, and on the wake calls `f_resume` until `DONE`. Parking then blocks that thread, not a scheduler worker. This is the `block_on` of Strata. It is forbidden on scheduler workers (compile error if the call is made from a worker context that is not a pool thread; at minimum, a runtime check).
- Exceptions cannot cross a C frame. The wrapper catches `THROWN` and converts it to the library's error convention or aborts. The binding says which.

## Dynamically loaded modules

**Direction:** not supported in the first compiler. It must stay possible, so the contract design assumes it:

- A loadable module is a native shared object plus its `.smod` interface. At load, the runtime checks: format and ABI versions, imported contract hashes against what is already loaded, and that any interface implementations it registers satisfy the declared bounds.
- A loaded module can be `Never`, `Cond` or `May` per function like any other; its callers are compiled against bounds, so no reanalysis is needed. This is the benefit of bound-based dispatch.
- **Tension with whole-program optimisation:** devirtualisation and fusion assume the set of implementations is closed. A plugin can add implementations. The application's build configuration therefore names the **open interfaces** (those plugins may implement); calls through them are never devirtualised and keep the thunks. Everything else stays closed-world. The configuration, not new source syntax, carries this (**Open**, [D9](open-decisions.md#d9-dynamic-module-loading-and-whole-program-optimisation)).
- Unloading is out of scope: frames and vtables may point into a module.
