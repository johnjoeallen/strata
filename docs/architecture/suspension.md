# Suspension and continuations

## 1. The may-park effect

Every function has an inferred **effect summary**. The part that matters for suspension is the park effect:

```
Park(f) = Never          f cannot park, on any path
        | May            f may park
        | Cond(P)        f parks only if an argument bound to a member of P may park
                         (P = function-typed parameters, interface-typed parameters, type parameters)
```

with the order `Never < Cond(P) < May`. The complete summary, which also records `throws` and foreign blocking, is in [modules](modules.md#effect-summaries).

### Sources of parking

A function's own body can park only through these, and the analysis starts from them:

1. A call to an `extern parks fn` (`sleep`, `yield`, `waitAny`, `Conn.recv`, `Conn.send`, ...). Already in `stra/runtime/leaves.stra`.
2. Entering `sync`/`mutate` (it queues FIFO, see spec 7.1). The body of the block can never park, so a lock is never held across a suspension.
3. A call to a function whose summary is `May`, or `Cond(P)` with a member of `P` bound to something that may park.
4. A call through an interface or function value whose declared bound permits parking (see [dispatch](dispatch-and-ffi.md#interface-calls)).
5. A **compiler-inserted budget check**, only in a function that asks for one with `[yield]`, on the function itself or through its type or package (section 1.1). The check sits on loop back-edges and recursive-call entries of that function's own body; when the budget is exhausted the task yields, which is an ordinary park point. Without `[yield]` the compiler inserts nothing.
6. A call to a blocking foreign function that the runtime offloads (see [blocking FFI](dispatch-and-ffi.md#blocking-foreign-calls)).

Cleanup methods are `Never` by rule (spec section 9). A cleanup that tries to call a `May` function is a compile error, which is already the behaviour for `sync`/`mutate` bodies.

### Propagation

The effect propagates transitively over the call graph. Within a module it is computed as a fixed point over strongly connected components of the call graph:

```
for each SCC in reverse topological order:
    repeat until no change:
        for f in SCC:
            f.park = join(own park sources of f,
                          Park(g) for each direct callee g,    // imported g: read from its summary
                          bound(i) for each indirect call i)   // see dispatch
```

Mutually recursive functions share one SCC and end up with the same effect. A recursive function with no park source anywhere in its SCC stays `Never`.

Across modules the same computation runs on summaries (a few words per function) and never needs the callee's code. That is what lets an application be checked without anyone's source.

### Programmer-facing consequences

- **No colouring in source.** Waiting (I/O, locks, sleeping) is inferred, never written. This is the one exception to principle 10 ("no hidden behaviour"), and it is unchanged.
- **Nothing is inserted unless the code says so.** The compiler adds a yield only inside a function marked `[yield]`. This keeps principle 10 intact for scheduling: what the code says is what happens.
- **Visibility is tooling's job** (Decided, principle 10): hover, lints and errors that name the chain `handle -> loadUser -> pool.acquire -> Conn.recv`.

### 1.1 Attributes: `[yield]`, `[noyield]`, `[nopark]`

Three attributes on a function (`fn[yield] name(...)`). Spellings are the author's; the rest is Direction.

| Attribute | Budget checks | May wait on I/O or a lock | Meaning |
|---|---|---|---|
| none | none, unless an enclosing type or package says `[yield]` | inferred | The default: nothing inserted |
| `[yield]` | inserted in this function's own loops and recursion | inferred | "The compiler may yield for me here" |
| `[noyield]` | none, even if an enclosing type or package says `[yield]` | inferred | Overrides an enclosing default |
| `[nopark]` | none | no | Assertion: any park source is a compile error. Implies `[noyield]` |

- **Where an attribute can be written, and precedence.** The innermost level wins, and the same three attributes work at every level:

  | Level | Written as | Applies to |
  |---|---|---|
  | Function | `fn[yield] parse()` | its own body |
  | Type | `struct Parser[yield] { ... }` | the methods declared in it |
  | Package | `package[yield] imaging`, in one file of the package | every function in the package |
  | Default | nothing written | nothing inserted |

  A CPU-heavy type marks itself once and tags its hot, bounded helpers `[noyield]`. A `[nopark]` type asserts that none of its methods can park; a method can still override it, and the override is then reported at the method. Files are not a level: a file is a layout accident.
- **Package.** Every source file starts with `package name`; the files that share a name form one package, which compiles to one `.smod` named after it (placeholder syntax; the rules are in [D14](open-decisions.md#d14-packages-and-imports)). The package default is written on the package declaration, `package[yield] imaging`, in exactly one file of the package (by convention `package.stra`); every other file writes a plain `package imaging`, and an attribute there is an error. Source files therefore carry no conflicting settings.
- **Resolved at definition.** The compiler resolves each function's effective attribute when it compiles the defining package, and the IR carries the resolved value. A generic instantiated in another package keeps the setting of the package that defined it; settings are never inherited by importers. The package default is part of the module's input hash: changing it recompiles the package and its effect changes ripple as usual.
- **Tooling must show the effective setting and its source** (function, type or package) in hover, because it is no longer visible at the function itself. That is the cost of having levels.
- **Spelling.** The attribute follows `fn` on a function and the name on a type, which differ; whether to unify them (`fn f[yield]`) is **Open**.
- **Own body only.** `[yield]` does not make callees yield. It is shorthand for a `yield()` every N iterations of the function's own loops. Its callers become `May` by ordinary inference.
- **The effect widens only if a check is inserted.** A `[yield]` function with no loop or recursion stays `Never`; tagging it causes no ripple.
- **What a check is:** a counter decrement and a rarely taken branch. On exhaustion the task goes to the back of the run queue and resumes in its turn: a park point, so live values are spilled to a frame as for any park. A check is also a cancellation point, so a task in a `[yield]` loop can be cancelled (section 4).
- **Cost control (Direction):** strip-mine loops so the counter is tested once per block of iterations; omit the check for loops with a small constant bound. Neither is guaranteed. The overhead is a measurement to take, not assume (benchmarks B13).
- **`[nopark]` is the strict form.** It is the only form that guarantees the function is `Never`. `[noyield]` still permits a call to `recv`. It is an optional assertion, never required, and never mentioned by callers; it turns a silent effect change into a compile error at a library boundary.
- **Lambdas** inherit the setting of the enclosing function (**Open**).
- **`[yield]` is not the `yield()` call.** `yield()` is an `extern parks fn` the program calls itself. The attribute only lets the compiler insert yields.
- **Limits:** the budget bounds how long a *checked* region runs. It does not bound a function without `[yield]`, a plain `extern fn`, or a single indivisible library operation. Such code stalls its worker and cannot be cancelled until it returns. The loop warnings in 1.3 exist to find it.

### 1.2 Calls allowed in `sync`/`mutate` bodies, cleanups and C callbacks

These regions may not park, so they may call only functions whose effect is `Never`.

Allowed:

- A function inferred `Never`: no park source anywhere below it. Looping helpers qualify, since without `[yield]` loops add no effect.
- A function tagged `[nopark]`.
- A `Cond(P)` function whose arguments are statically non-parking.
- A plain `extern fn` (a promise of a short, non-blocking call; unverifiable).
- Operations that do not park: `new`, field access, `throw`, `Ref` copies.

Rejected, with the call chain in the message:

- Any `May` function. That includes one that is `May` only because it is marked `[yield]` and has a loop.
- Anything that parks: `sleep`, `recv`, `send`, `yield()`.
- Entering another `sync`/`mutate`, unless that object is in the same head.
- A call through an interface or function value whose declared bound is not `Never` (the default bound is `May`).

Inside such a region a `[yield]` marking is ignored for the region's own loops: no check is inserted there, and a type- or package-level `[yield]` does not apply.

**Why a body may not park** (the rule is a design choice, not a technical limit; frames could hold a lock):

1. *Hold time stays bounded by the code in the block.* A holder parked on a socket keeps the lock while every other vthread queues; with FIFO fairness one waiting writer also blocks the readers behind it.
2. *Two vthreads on one worker never contend.* A body that cannot park runs to completion before anything else runs on that worker, so contention is only across workers, where the runtime can spin briefly before parking. That spin removed a 55 to 75 percent throughput loss (`bench/results/miniredis.md`).
3. *Multi-object blocks retry safely.* `mutate(a, b)` takes all its locks before the body runs, or none, and backs off if one is busy. This is safe only because no body code has run. It is also why nested acquisition is an error.
4. *No frame ever owns a lock.* Cancellation, errors and unwinding never have to release a lock on behalf of a suspended task.
5. *A class of deadlocks cannot be written:* a holder waiting for something that another vthread can provide only after taking the same lock.

What it costs: no I/O under a lock (copy out, release, then act, as `Db.get` and `Db.set` do), and long work under a lock must be chunked (`purgeExpired` removes at most `PURGE_BATCH` keys per hold).

### 1.3 Loop warnings

A lint, not a proof. Termination is undecidable, so these are warnings and never errors.

**Classification.** Each loop gets a class, and each function gets a `loops` class in its summary, composed over its callees by the same rules as nested loops. The class describes *uninterrupted* work: a loop whose body contains a park point (a call to any `May` function, a `[yield]` check, `yield()`, a lock entry) contributes `none`, because every iteration gives the scheduler a chance. `purgeExpired` in `stra/redis/db.stra` is the example: its outer loop yields, and the inner loop under the lock is bounded by a counter against a constant.

| `loops` | Meaning |
|---|---|
| `none` | No loop or recursion |
| `bounded` | Every loop advances a counter or iterator toward a constant bound under the threshold |
| `long` | A constant bound above the threshold; a bound that is loop-invariant but not constant (a configured limit such as `lim.maxBulk`); iteration over a collection of unstated size; or nested loops whose product exceeds the threshold |
| `unbounded` | No proven progress toward an exit: `loop {}`, a `while` where nothing moves the condition, or unbounded recursion |

A loop that advances a counter toward a loop-invariant bound terminates even when the bound is only known at run time, so the compiler can see it without any annotation. A stated limit that the program enforces with an ordinary `if`/`throw` (principle 11) is visible the same way.

Calls through interfaces, function values and foreign code contribute nothing at normal levels (a lint is not a proof) and are listed as `unknown` at the strictest level.

**What warns.** Within `[noyield]`, `[nopark]` and `sync`/`mutate` bodies:

- `unbounded` own loop or recursion;
- a call to a function whose summary says `unbounded` (the chain is printed: `tight -> parse -> scanForever`);
- `long`, at the stricter levels.

**When it runs.** Off by default in ordinary builds, except for one case: **`unbounded` inside a `sync`/`mutate` body always warns**, because spec 7.1 relies on hold time being bounded by the block, and nothing else enforces it. A flag selects levels (placeholder `--warn-loops=off|marked|reachable|all`): `marked` covers `[noyield]` and `[nopark]` functions; `reachable` adds unmarked functions reachable from `spawn` roots; `all` adds everything. Editor and CI use it freely.

**Suppression.** An allow-attribute on the function or loop (placeholder). It acknowledges the warning and makes no claim about the loop. A loop-level bound annotation was considered and dropped: if the compiler can see a counter or constant it is redundant, if not it is an unchecked claim, yielding at the bound is just `[yield]`, and silently stopping at the bound hides behaviour. The only coherent form is an ordinary `if`/`throw` on a limit, which the program can already write.

**What the module exposes.** The `loops` class is part of each exported function's summary, in a separate *advisory* section that is **not** part of the contract hash, so a change in loop structure never forces dependants to recompile. A caller's lint reads the callee's current `.smod`, and the warning can lag until relink ([modules](modules.md#effect-summaries)).

**Known limits.** It will miss unbounded work inside loops that look bounded, flag some bounded loops it cannot prove bounded, and sees foreign and indirect calls only as stated.

### Example: attributes, `sync` and loop warnings

Illustrative Strata source and C. Neither is compiler output.

```cpp
package http

// Opted in: the compiler inserts a budget check in this loop. May, yields only.
fn[yield] parseHeaders(buf: u8[]&): Headers throws BadRequest {
    var h = Headers();
    var i = 0;
    while i < buf.len {              // back-edge: budget check inserted here
        let line = nextLine(buf, i);
        h.add(line);
        i = line.end;
    }
    return h;
}

// No attribute: nothing inserted. No loop effect, so Never: usable under a lock.
fn hash(key: u8[]&): u64 {
    var x: u64 = 14695981039346656037;
    for b in key { x = (x ^ b) * 1099511628211; }
    return x;
}

fn lookup(cache: Ref<Cache>, key: u8[]&): i64? {
    let h = hash(key);               // Never: fine
    sync(cache) {                    // the body may call only Never functions
        return cache.entries.get(h); // get() is Never unless it is [yield] or parks
    }
}

fn[nopark] broken(k: u8[]&) {
    sleep(1ms);                      // error: park source in a nopark function
                                     //   broken -> sleep (extern parks)
}

fn[nopark] spin(flag: Flag&) {
    while !flag.isSet() { }          // warning (loops): unbounded loop in a nopark function
}

fn tight(cache: Ref<Cache>, r: Reader&) {
    mutate(cache) {
        scanForever(r);              // warning (always on in a lock body): scanForever loops unbounded
    }                                //   tight -> scanForever
}
```

A type that opts in as a whole, and a package that sets the default for its files (`package.stra` is the one file that may carry package attributes):

```cpp
// imaging/package.stra
package[noyield] imaging         // the package default; here it restates that nothing is inserted

// imaging/renderer.stra
package imaging

struct Renderer[yield] {
    fn render(page: Page&): string { for b in page.blocks { ... } }   // gets checks: the type says [yield]
    fn[noyield] crc(data: u8[]&): u32 { ... }                          // override: plain C
    fn[nopark] width(c: u32): u32 { return c < 128 ? 1 : 2 }          // asserts it never parks
}
```

Inferred effects and summary fields:

```
hash          Never   loops=bounded   (no attribute, no checks)
nextLine      Never   loops=none
parseHeaders  May     yields=true  loops=long
lookup        May     (acquiring the lock can park)
spin          Never   loops=unbounded (nopark, warns)
```

C for `parseHeaders`:

```c
st_t parseHeaders_call(task_t *t, u8buf *buf, Headers *out) {
    Headers h = headers_new();
    size_t i = 0;
    while (i < buf->len) {
        if (--t->worker->budget <= 0) {              // budget check
            // a yield is a real park: spill live values into a frame
            parse_frame *f = frame_new(t, parseHeaders_resume, /*state*/ 1);
            f->buf = buf; f->h = h; f->i = i;        // buf is a borrowed parameter, see D2
            run_queue_push_back(t);
            return PARKED;
        }
        Line line = nextLine(buf, i);                // plain call
        headers_add(&h, line);
        i = line.end;
    }
    *out = h;
    return DONE;
}
// parseHeaders_resume re-enters the loop with f->h and f->i restored, or on CANCELLED
// runs the cleanups for h, frees the frame and returns CANCELLED.
```

- `hash` has no counter and no status, so the C compiler can inline it freely.
- On the common path a check costs a decrement and a branch per iteration. Strip-mining would test once per block. Neither cost nor strip-mining is measured yet (B13).
- If `buf` pointed at a native-stack local in the caller, the caller would need a frame-resident local, or the yield would leave a dangling borrow ([D2](open-decisions.md#d2-borrowed-locals-and-the-lazy-shape)).

## 2. Lowering to resumable frames

### The three shapes of a function

| Park(f) | Generated form |
|---|---|
| `Never` | An ordinary C function. No frame, no status codes beyond `throws`. |
| `Cond(P)` | Compiled as two entries. A call whose `P` arguments are `Never` binds to the plain form; otherwise to the suspendable form. |
| `May` | The suspendable form below. |

The key property for performance, and the reason conservatism is affordable: **a `May` function that does not park on a given call costs about the same as an ordinary call**. This was measured on the hand-written C (`bench/results/lazy.md`): about 0.7 to 1.5 ns per extra parking call in a chain, against ordinary calls, and 3.4 ns for the whole four-deep request. It is a microbenchmark with hot frames, so it supports "conservative contracts are cheap enough to be the default" and not much more.

### The suspendable form: native-stack fast path, frames made on demand (Direction)

Each `May` function is compiled to a pair:

```
f_call(task, args..., out*) -> status       runs on the worker's native stack
f_resume(task, frame*)      -> status       state-machine entry used after a park

status = DONE | THROWN | PARKED
```

`f_call` is the function as written, as straight C. Calls to other `May` functions check the returned status. If the callee returns `PARKED`, `f_call` does **not** park by itself. It allocates a frame, copies out the locals that are live across that call, records its state number, links the frame to the callee's frame, and returns `PARKED` to its own caller. The next function up does the same. A chain of N frames is therefore built from the leaf upward, during one return, and only when the park really happens.

When the event arrives, the scheduler calls the *innermost* frame's `f_resume`. When that finishes, it resumes its parent frame directly, in turn. This is the `lazy` strategy plus `direct` resume from the experiments. Two measured reasons it is the first choice:

- Parks are rare relative to calls in typical handlers, so the common path allocates nothing (`lazy.md`: allocations per request = p x (D+1)).
- Resuming only the innermost frame, rather than re-entering every frame from the root as Rust async does, kept wake cost flat with depth: 6.3 ns against 26.2 ns at depth 16 (`bench/results/frames.txt`).

The alternatives stay on the table because `lazy` needs two code versions per function that must agree on state numbering, and was 1.2x the code size of `eager` in the toy:

| Strategy | Allocations when no park | Code | When it wins |
|---|---|---|---|
| Eager: a frame per `May` call | D+1 | smallest | Frame-resident locals (below); debug builds |
| Embedded: callee frames inside caller's frame | 0 or 1 per task | medium | Chains that nearly always park (p near 1) |
| Lazy: frames made while unwinding | p x (D+1) | largest | Rare parks; **default** |

The compiler may choose per function (**Direction**). The choice is invisible to callers because they only see `status`; that is what makes it a module-level decision and not an ABI one.

### Frame layout

A frame is:

```
+------------------------------+
| resume function pointer      |   header, identical for every function
| state number                 |   (the stable part of the ABI)
| parent frame link            |
| task pointer                 |
| result/out slot binding      |
+------------------------------+
| live slots                   |   private to the function
+------------------------------+
```

Live slots are the values live at *some* suspension point. Slots that are never live at the same time share storage, by the usual register-colouring over park points. Temporaries live across a park (an argument already evaluated for a call that has not happened, a pending `Ref` promotion) are slots like any others.

Frames come from a per-function arena, sized exactly. This matches the existing per-type arena rule and the `frames_bench` model; idle parked task cost there was 137 B with no state and 393 B at depth 4, against a 4 KB floor for a stackful fiber (Measured, hand-written).

**Frames never move** (Direction). A frame address is stable from creation to destruction. This removes the `Pin` problem Rust async has, and lets other code hold pointers into a frame.

### Pseudocode

```
package db                                  // compiled separately         Park(query) = May   (calls Conn.recv)
fn query(sql: string&, id: i64): Row throws DbError {
    let c = pool.acquire();                 // May: waits for a free connection
    c.send(encode(sql, id));                // May
    return decode(c.recv());                // May
}

package app
import db
fn handle(req: Request&): Response throws DbError {
    let row = db.query("select ...", req.id);   // inferred May; nothing written
    return render(row);                         // Never
}
```

Generated C for `handle` (illustrative; names and layout are not fixed):

```c
typedef struct { frame_hdr h; Request *req; } handle_frame;   // row is not live across a park, so not a slot

st_t app_handle_call(task_t *t, Request *req, Response *out) {
    Row row;
    st_t s = db_query_call(t, "select ...", req->id, &row);
    if (s == PARKED) {                              // a real park, happening now
        handle_frame *f = frame_new(t, app_handle_resume, /*state*/ 1);
        f->req = req;
        t->innermost->parent = f;                   // link callee's frame to ours
        t->innermost->out = &f->h.result;           // the result now lands in the frame, not on our stack
        return PARKED;
    }
    if (s == THROWN) return THROWN;                 // no cleanups pending in this function
    *out = render(&row);                            // Never: a plain call
    return DONE;
}

st_t app_handle_resume(task_t *t, handle_frame *f) {
    switch (f->h.state) {
    case 1: {                                       // db.query finished
        if (t->wake == THROWN) { frame_free(f); return THROWN; }
        Response r = render((Row *)&f->h.result);
        frame_free(f);
        return finish(t, &r);                       // resume parent directly or complete the task
    }}
}
```

One real hazard shows in `t->innermost->out = ...`: the callee was given a pointer into the caller's native stack. Pointers to locals that are live across a park are the subject of the next subsection.

### Borrows and locals that cannot be moved

The lazy form copies locals into a frame only when a park occurs. A copy changes the address. Two things can hold the old address:

1. A **borrow** (`T&`) to a local, given to a callee that is part of the park chain, or kept in another local.
2. A **result or out pointer** (above), which the unwinding step can rebind because it is compiler-generated.

Case 2 is mechanical. Case 1 is not: `let buf = readBuffer(); conn.recv(buf&)` is exactly this shape, and extremely common.

**Direction:** a local that is borrowed *and* live across a call that may park is **frame-resident** from its declaration, in a frame created at function entry. A function with any frame-resident local uses the eager shape for that function. Other functions in the chain stay lazy. The borrow-origin analysis (spec section 5) already has what is needed to decide this; the new fact is only that the flag *borrowed across a may-park call* is recorded for each local. This is a real constraint on how often `lazy` applies, and not measured: the toy benchmark has no borrowed locals. See [open decision D2](open-decisions.md#d2-borrowed-locals-and-the-lazy-shape).

## 3. Calls that never suspend

`Never` functions compile to ordinary C and are inlined by the C compiler under its usual rules. Without `[yield]`, a loop adds no effect, so most helpers (parsing, rendering, hashing) are `Never`. A `Cond(P)` function called with a known non-parking argument binds to its plain entry.

For a `May` function the non-suspending path should be straight-line C with one status test after each `May` call. The cost of that test is what the 0.7 to 1.5 ns above measures.

## 4. Errors, cancellation and cleanup

### Errors (`throws`)

C has no unwinding, so a throw is a status return (`THROWN`) with the exception in the task, and each function branches to its cleanup block for the scope it is in, then returns `THROWN`. This is the same machinery as the park return, which is why the two compose. Not built in the C prototypes yet (listed in `open-questions.md`).

Across a park, a thrown exception arriving at `f_resume` (`t->wake == THROWN`) enters the same cleanup blocks as it would on the fast path. The cleanup code is shared between `f_call` and `f_resume`; this is a code generation requirement, not a runtime one.

### Cancellation (Direction)

Cooperative, as in spec 6.2: the cancelling party marks the task and wakes it. A task is only ever cancelled **at a park point**, because that is where its state is in a frame. The wake carries `wake = CANCELLED`, and `f_resume` takes the unwinding path: run cleanups of the scopes it is in, free its own frame, resume the parent with `CANCELLED`, and so on up the chain. Frames are never discarded without running cleanups, and cleanups never park, so unwinding is bounded.

Cancellation is **not an ordinary exception**: it is not named in `throws`, and `catch` cannot swallow it. Letting `catch (Exception)` swallow it would make cancellation unreliable; requiring it in `throws` would put the effect back in signatures everywhere. This choice interacts with the open exception-model question (spec 10.7) and is stated here as Direction only.

A task can be cancelled at any park point, including a budget check, so a task spinning in a `[yield]` loop is cancellable. A task in a loop without `[yield]`, a `nopark` function, a plain `extern fn` or an indivisible library operation is not, until it returns.

### Deterministic cleanup

Cleanup of a resource runs when its scope exits, by normal return, `THROWN` or `CANCELLED`, and happens before the frame is freed. Task-level cleanup precedes reclamation of any task arena (spec section 9, unchanged). A parked task holding a `sync` permission is impossible by construction (no park inside the block), so no frame ever owns a lock.

## 5. Recursion

- A recursive function with park sources in its SCC is `May`. Each activation has its own frame, taken from an arena, so recursion depth while parked is limited by memory, not by a stack size.
- On the **fast path** a recursive `May` function consumes native stack until it returns or parks. A runaway recursion that never parks would overflow the worker's stack, as in any C program. **Direction:** a depth counter in recursive `May` functions; past a limit they switch to the eager shape (frames, not native stack), so depth is bounded by the declared limits (principle 11) rather than by the worker stack. **Open:** the limit, and whether recursive `Never` functions also need a declared depth bound.
- Mutual recursion across modules is not possible through ordinary imports (module imports form a DAG). It can occur through interface calls, where the effect is the declared bound (see dispatch).

## 6. Interaction with ownership

| Concern | Rule (Direction unless stated) |
|---|---|
| `Ref<T>` live across a park | Held in a frame slot; the slot is an owner. Frame destruction (normal, thrown or cancelled) releases it. A parked task therefore keeps its objects alive. |
| `WeakRef<T>` live across a park | Held in a slot. Promotion is a separate step after the resume, as everywhere (spec 3.1). The compiler must not hoist a promotion across a park point: the object may have expired meanwhile. |
| Borrow `T&` live across a park | Allowed if the referent outlives the parked task. The lifetime analysis (spec section 5) already tracks origins; the new check is that **every referent a borrow can point to is either frame-resident, a `Ref` held by the frame, or static**. A borrow of a native-stack local is the case handled above. |
| Task temporary arena | Belongs to the vthread, not to a frame. Survives parks. Reclaimed at task exit after cleanups. |
| Per-function frame arenas | Runtime-owned like per-type arenas. A frame can be freed on a different worker from the one that allocated it (a task can resume anywhere once migration exists), so frees must be cross-thread-safe; the same requirement as `new`. |
| Escape analysis (spec 4.4) | A value that does not escape the task may live in the task arena or in a frame slot. The compiler must preserve destructor timing across parks. |

## 7. Optimisation opportunities

**These are not guaranteed.** Each is something the compiler may do when conditions hold, and none changes behaviour. The tier column says what each needs from [the module format](modules.md).

| Opportunity | What it is | Conditions | Needs |
|---|---|---|---|
| Frame embedding | The callee's slots live inside the caller's frame. One allocation per task instead of D+1. | Callee frame size known; callee not recursive; one live activation per caller activation. | Callee layout (link time, or a fixed layout exported from its module) |
| Allocation elimination | No frame for a function that never parks in practice | Not provable; use `lazy` instead, which allocates only on a park | None |
| State-machine fusion | Inline a `May` callee into its caller so its park points become states of the caller's machine; one frame, one resume function. | Single call site, or small callee; direct (not virtual) target; not recursive. | Callee IR (Tier C) |
| Flattening | A chain `a -> b -> c` becomes one machine | The same, repeated; code size bounded | Callee IR |
| Slot overlay | Slots from inlined callees share storage by liveness | After fusion | IR |
| Direct resume | Resume the innermost frame only | Always applied by the `lazy`/`direct` scheme | None |
| Park-free specialisation | A `Cond(P)` function called with a non-parking argument becomes `Never` | Argument's effect known statically | Summary |

Limits on fusion: code size growth; an exported function still needs its own standalone entry points; debugging and profiling need a map from fused states back to logical frames (see [open decisions D8](open-decisions.md#d8-debugging-stack-traces-and-profiling)); cancellation points and cleanup scopes of the inlined function must keep their order and timing exactly; recursion and virtual calls block it. The measured upper bound on what any of this can save is small in the toy case: about 1 ns per parking call, "a request that does any real work costs thousands of times more" (`lazy.md`). Fusion is therefore a later, evidence-driven optimisation, not part of the first compiler.

## 8. Relative to Java virtual threads and Rust async

| Aspect | Java virtual threads | Rust async | Strata (Direction) |
|---|---|---|---|
| Source | Plain blocking style | `async fn`, `.await`, runtime choice | Plain blocking style |
| Suspension visible in signatures | No | Yes (`async`) | No in source; yes in tooling and module summaries |
| Implementation | Stack of a continuation copied to and from the heap by the JVM (stackful) | One compiler-generated state machine per `async` body, composed into a larger one | Per-function resumable frames; compiler-generated |
| Memory per parked task | Depends on the stack depth at park, copied | Size of the composed state machine, known statically | Frames per function on the chain; exact, not yet measured in a compiler |
| Can suspend through native frames | Pinned or blocked in some cases (`native`, and `synchronized` on older JDKs) | Not applicable: `await` only crosses async code | No: foreign frames cannot be captured, so the rules in [dispatch](dispatch-and-ffi.md) apply |
| Runtime requirement | JVM with JIT and GC | Library executor | Small C runtime, no GC, no JIT |
| Dynamic code and reflection | Natural | Limited | Weaker; closed-world assumptions help the optimiser |
| Recursion | Natural | Needs boxing | Natural (frames from arenas) |
| `dyn` / virtual | Natural | Needs `Box<dyn Future>` and `Pin` | Uniform: effect bound in the interface |
| Cost of a function that might park but does not | A call | A poll, plus the state machine's size | A call plus a status test (Measured, toy) |

**Advantages Strata is aiming for:** no function colouring and no `Pin`; precise frames with no stack copying; deterministic cleanup on cancellation (Rust async cancels by dropping, Java by interrupts); no runtime beyond the C one.

**Trade-offs and limitations, stated plainly:**

- Every function the analysis marks `May` costs a status test per `May` call and extra generated code. The analysis can mark a function `May` because of a deep, unseen callee; nothing in the source says so.
- Compile-time complexity moves from the programmer to the compiler: effect inference, frame layout, two code versions per function, and a module format that must carry it all.
- Foreign code cannot be suspended in the middle. Java's pinning problem exists here in a different form (see [dispatch](dispatch-and-ffi.md#blocking-foreign-calls)).
- Stack traces and profilers see the native stack, not the logical one, unless the toolchain supplies maps.
- **No performance superiority is claimed.** The existing measurements are small, hand-written, and mostly show a memory advantage over stackful fibers (and parity with Tokio-class frames). See the [evaluation plan](benchmarks.md).

## 9. Where stackful execution remains

1. **As the reference semantics.** The tests that decide whether a language feature is correct should pass under both implementations, which is cheap to arrange if the stackful runtime is kept working.
2. **As a baseline** for the evaluation plan: stackful fibers are the cost to beat on memory (4 KB floor, 4.1 KB resident at 1 KB state against Tokio's 1.4 KB).
3. **For synchronous drivers** on blocking-pool threads, where a foreign callback needs to call parking Strata code ([dispatch](dispatch-and-ffi.md#foreign-callbacks)).

It is no longer the primary implementation path. Spec section 6.1 is rewritten accordingly.
