# Strata language and runtime assessment against Rust and Go

Assessment date: 31 October 2026.

This assessment covers the [language specification](strata-design.md), the
[runtime interface](stra/runtime/leaves.stra), the C runtime prototypes,
[design decisions](docs/decisions.md), and the recorded benchmark findings.
It incorporates the discussion of explicit `yield()`, reader/writer locking,
possible compiler-inserted scheduling points, and (in a later update) the
resumable-frame and compiled-module architecture. It is a design review, not
a compiler audit or an independent reproduction of the benchmarks.

## Overall judgment

Strata has a credible position: a server-focused language combining ordinary
sequential code, lightweight concurrency, deterministic cleanup, and static
reference safety without programmer-written lifetime names.

The strongest part is the combination of goals and the willingness to test
runtime assumptions. The weakest part is that the central language promise
remains unproven: there is no compiler demonstrating that the proposed borrowing,
sharing, effects, and cleanup rules work together on realistic programs.

The hand-written C establishes useful evidence about particular runtime and
server architectures. It does not yet establish that a safe Strata compiler can
generate equivalent programs, or that Strata generally outperforms Rust or Go.
The repository is appropriately explicit about this limitation.

My judgment is that this is a promising language research project with a
plausible server niche. It is not yet a practical replacement for either Rust
or Go. Its strongest opportunity is to make a constrained class of concurrent
services easier to write safely and operate predictably, rather than to match
either language across every application domain.

## Comparison at a glance

| Dimension | Strata as proposed | Rust | Go |
|---|---|---|---|
| Primary memory model | Inferred borrows, reference-counted dynamic objects, scoped storage | Ownership and borrowing, with explicit shared ownership when needed | Garbage collection and escape analysis |
| Cyclic object graphs | Strong cycles need weak-edge discipline | Reference-counted cycles also need weak-edge discipline | Unreachable ordinary object cycles are collectable |
| Resource cleanup | Deterministic cleanup on scope exit and last-owner destruction | Deterministic destruction through ownership | Explicit resource management, commonly `defer` |
| Concurrent service code | Sequential-looking virtual-thread code | OS threads or executor-based async code, among other approaches | Sequential-looking goroutine code |
| Shared mutable state | Compiler-enforced `sync` and `mutate`, with detailed alias rules unfinished | Ownership and concurrency traits enforce substantial static restrictions | Locks, channels, atomics, and programmer discipline; races remain possible |
| CPU scheduling | Cooperative prototype with explicit `yield()` | OS preemption for threads; typically cooperative task scheduling in async runtimes | Runtime preemption of goroutines at safe points |
| Main usability cost | Inferred borrowing/effects, synchronization restrictions, ownership discipline | Ownership, borrowing, lifetime relationships, and async constraints | Race avoidance, resource handling, and runtime performance trade-offs |
| Present maturity | Specification, illustrative source, and hand-written C prototypes | Mature compiler, tools, and ecosystem | Mature compiler, tools, runtime, and ecosystem |

The Rust concurrency comparison follows its [ownership-based concurrency
model](https://doc.rust-lang.org/book/ch16-00-concurrency.html). The Go comparison
follows its [memory model](https://go.dev/ref/mem) and
[GC guide](https://go.dev/doc/gc-guide). Different Rust libraries and application
architectures can make substantially different trade-offs.

## What Strata gets right

### A coherent target

Concurrent servers are a sensible initial scope. They benefit from readable
control flow, predictable cleanup, bounded memory use, efficient waiting, and
safe access to shared state. Strata's design choices address those needs rather
than merely offering alternative syntax.

Ordinary blocking-looking calls with inferred parking effects are especially
attractive. They could remove much of the visible async machinery while retaining
efficient multiplexing. Whether the resulting APIs remain simpler depends on
the quality of effect inference and diagnostics.

### Clear distinctions between lifetime, access, and allocation

`Ref<T>` keeps an object alive; `sync` and `mutate` regulate concurrent access.
Those are separate responsibilities, and the specification treats them that way.

Likewise, virtual-address reservation is correctly described as an allocation
technique rather than an ownership model. The design does not pretend that
reserving a large address range solves object lifetime or makes every small
allocation cheap.

Explicit weak promotion, atomic promotion from a nonzero strong count, and
retention of the control block until weak owners disappear are appropriate
foundations. The specification also acknowledges strong-reference cycles,
foreign-function uncertainty, and destructor timing rather than hiding them.

### Resource handling and bounded input

Deterministic cleanup, explicit flushing, and non-parking destructors form a
useful contract. The distinction between best-effort cleanup and an explicit
operation that guarantees completion is valuable for network APIs.

Declared input limits and streaming or file-backed handling of large payloads
are good server-library defaults. However, per-request limits alone do not bound
process memory. Connection counts, simultaneous uploads, queued tasks, held
responses, and outstanding allocations also need aggregate admission controls.

### Measurement-driven runtime exploration

The recorded work examines memory floors, reference-count strategies, frame
representations, lock handoff, and allocator behavior. Reporting when an estimate
was too optimistic or an approach lost throughput makes the evidence more useful.

The most valuable result is not a single requests-per-second number. It is the
identification of specific costs and the ability to choose an implementation
based on those costs.

## The central language risks

### Inference must replace annotations without hiding an incomprehensible model

The proposed `choose(a, b)` example is reasonable: infer that the returned borrow
may originate from either argument and constrain its use accordingly.

The hard cases are mutable aliases, references stored in fields, callbacks,
closures, generic containers, recursive functions, and separately compiled
libraries. Removing lifetime names does not remove those relationships.

A safe implementation can conservatively reject uncertain programs, but that
alone does not establish a usable language. Developers need understandable
diagnostics and a way to express intent when inference is insufficient. Exported
inferred contracts also need stability rules: a body change must not produce an
unexplained compatibility failure in downstream code.

Rust has a substantial advantage here: a working system and years of experience
with difficult cases. Strata's opportunity is simpler interaction for its target
workloads, not an assumption that the underlying problem disappears.

### Lifetime safety is only part of reference safety

Preventing a reference from outliving its owner does not by itself prevent
invalidation or conflicting access. For example, an element borrow may become
invalid when its array grows, even though the array itself is still alive.

The language needs precise rules for mutation, relocation, overlapping mutable
access, and references into compound objects. Offsets in the HTTP prototype
avoid one particular relocation problem; the general language still needs an
answer for ordinary borrowed elements and fields.

### Sharing and protected access need a complete alias model

The unsettled rules around shareability are foundational. The compiler must know
when an object can cross tasks, when locks are required, and whether thread-local
state or non-atomic counts can safely become shared.

Transitive access is particularly important. If a locked object contains a
`Ref<Other>`, locking the outer object cannot automatically establish exclusive
access to `Other` when independent aliases exist. Borrowed views, callbacks, and
methods invoked under a lock all need rules that preserve this distinction.

The language also needs defined synchronization visibility: what publication,
lock release/acquisition, task creation, and task completion guarantee about
memory observed on other workers. Lock admission policy is not a memory model.

### Reference counting has costs as well as benefits

Reference counting removes dependence on tracing collection for ordinary
reclamation and supports last-owner destruction. It introduces strong-cycle
discipline, handle-copy costs, atomic traffic for shared objects, and potential
cache-line contention.

Destruction can also cascade through a graph. Deterministic destruction means
the trigger is defined; it does not mean the operation is cheap or has bounded
latency. A final release on a request worker can perform substantial work.

The recorded decision to use plain counts for proven confinement and atomic
counts for shared ownership is sensible. Its safety still depends on how
confinement and transfer are expressed and enforced.

Against Go, this is a trade-off rather than an automatic win. Go's collector has
CPU, memory, and latency costs, but relieves programmers of weak-edge design for
ordinary unreachable cycles. Its current collector performs core tracing work
concurrently; characterizing it simply as occasional long pauses would be
misleading. See the [Go GC guide](https://go.dev/doc/gc-guide).

### Exceptions, cleanup, and structured tasks must form one model

Checked `throws` clauses make exceptional exits visible in interfaces. They also
need rules for callbacks, generic functions, broad exception types, and API
evolution. These examples should be tested before declaring the model simpler
than Rust's explicit error propagation or Go's error returns.

The runtime interface already states that leaving a child-task scope, normally
or exceptionally, first waits for all children. The main specification still
lists aspects of this interaction as open. The remaining questions include
cancellation propagation, children that fail to finish, and how parent-owned
data stays alive while an exceptional exit waits.

Non-parking, non-throwing cleanup is a useful restriction. It still needs defined
ownership transfer, partial-construction cleanup, destruction order, and exactly
once behavior. A child-task scope exit that waits also has to participate in the
parking effect even though ordinary cleanup methods cannot park.

## Scheduling, yield, and preemption

### Explicit yielding is already present

Strata provides `yield()` in the runtime interface. It goes to the back of the
worker's run queue and resumes in its turn. The C prototype implements the
corresponding behavior through `yield_now`.

Consequently, the assessment should not describe Strata as lacking a mechanism
for long-running tasks to share CPU time. The issue is how much scheduling
fairness depends on programmers placing those calls frequently enough.

### How Rust and Go compare

Rust OS threads are scheduled and preempted by the operating system.
[`std::thread::yield_now`](https://doc.rust-lang.org/std/thread/fn.yield_now.html)
provides voluntary yielding but is generally not required to let other OS threads
run.

Rust async scheduling depends on the executor. Tokio tasks cooperate with the
executor; an arbitrary CPU loop cannot be forcibly interrupted by Tokio's task
scheduler. Tokio offers explicit yielding, cooperative budgeting for participating
operations, and facilities for moving blocking work off async workers. An
`.await` does not necessarily yield if the awaited operation is immediately ready.
See [Tokio tasks](https://docs.rs/tokio/latest/tokio/task/) and
[cooperative scheduling](https://docs.rs/tokio/latest/tokio/task/coop/).

Go's runtime can preempt goroutines at safe points, including asynchronous safe
points in ordinary user code. This generally protects other runnable goroutines
from a forgotten yield in a tight loop, although it is not a hard scheduling
deadline. See the [Go runtime preemption implementation](https://go.dev/src/runtime/preempt.go).

Strata's current cooperative approach therefore shares a real limitation with
Tokio. Go provides stronger automatic protection against that particular mistake.

### Compiler-inserted scheduling checks are feasible, with important consequences

*Update:* the author first chose these checks as the default, then reversed that: they are now opt-in per function or per type with `[yield]` (package level once packages exist), and nothing is inserted otherwise ([suspension 1.1](docs/architecture/suspension.md#11-attributes-yield-noyield-nopark)). The consequences below describe the check mechanism itself; the effect ripple applies only to functions that opt in.

A compiler could insert checks on loop back-edges and selected function entries,
then yield when a scheduling slice expires. This would cover ordinary loops and
recursion without requiring a programmer-written call at every point.

The runtime must have a way to detect slice expiry while a task is running.
A timestamp sampled at checks, an operation counter, or a flag updated by another
thread or signal are possible approaches. A flag updated only by the occupied
worker's scheduler would not provide that detection during a long computation.

The largest compiler consequence is the parking effect. If arbitrary loops can
yield, many previously non-parking functions become potentially suspending.
Callers may need resumable frames, and their callers may need them too. Checks
must compose with protected regions, cleanup, callbacks, and foreign calls.
This is more than inserting a branch into otherwise unchanged generated C.

Coverage would still have limits: opaque native code, blocking foreign calls,
large indivisible operations, and regions where scheduling is deferred. Library
operations such as sorting and copying may need internal checks or separate
execution policies.

A useful initial guarantee would be eventual scheduling opportunities in
ordinary generated Strata code outside explicitly exempt regions. A strict
maximum delay would require much stronger bounds on all work between checks.

Scheduling and cancellation should be specified separately. A check can allow
another task to run without making that location an exceptional unwind point.

## Reader/writer locking and scheduling

The intended compatibility rules are clear:

| Held permission on an object | Another reader | Another writer |
|---|---|---|
| `sync` | Compatible, subject to admission order | Waits |
| `mutate` | Waits | Waits |

Waiting virtual threads park; their workers can run other tasks. A lock on one
object does not inherently prevent unrelated tasks or other OS workers running.

FIFO admission adds an important qualification: a new reader arriving behind a
waiting writer waits too. This prevents a continuous stream of new readers from
starving that writer. A long read block can therefore delay both a writer and
readers queued behind it, while already admitted readers remain compatible.

Suspending a holder does not release its lock. Preemption can improve execution
of unrelated tasks while leaving competitors waiting for the protected object.
The same distinction applies to locks in other languages.

The current no-parking-in-block rule also means explicit `yield()` is forbidden
inside those blocks under the declared parking effect. A non-parking block can
still contain a very expensive or infinite computation. The rule bounds the kinds
of suspension, not the elapsed hold time.

If automatic preemption is added, Strata must either defer it inside protected
blocks or permit holder suspension and revise the current invariant that two
virtual threads on one worker never contend. Either choice has consequences;
it needs an explicit contract.

### Fair multi-object acquisition remains a research item

Acquiring all objects in one head before executing the body is a good direction.
It avoids the need to undo application actions after a partial acquisition fails.
Rejecting unsafe nested acquisition and upgrades also simplifies reasoning.

However, global lock ordering, retry, FIFO queues, and preserving an original
arrival ticket do not by themselves establish starvation freedom. The protocol
must explain ticket ordering across queues, reservation or handoff behavior,
cancellation, and interactions between single-object and multi-object requests.

The [lock findings](docs/journey/06-locks.md) correctly identify multi-object retry
as unimplemented. Its progress guarantees should be treated as proposed until
supported by a precise model and implementation.

## Runtime architecture and document consistency

*Update after the [architecture update](docs/architecture/index.md).* The earlier
mismatch is resolved on paper: the specification (section 6.1) now takes
compiler-generated resumable frames as the implementation direction, keeps the
programming model as virtual threads, and retains stackful fibers as a reference
implementation and memory baseline. The [C runtime](c/runtime/rt.h) already
represents tasks through sized frames and resume functions, so it matches the
direction. The resolution is a decision, not a demonstration.

The distinction between experiments still governs the claims. Very small idle
task frames do not demonstrate equally small resident stacks for general stackful
execution. Measurements of stack floors do not establish the cost of generated
resumable functions, and the hand-written frame benchmarks are not compiler
output.

This distinction changes the difficulty of preemption too. With established
stackful switching, suspending a call chain can reuse that mechanism. With
resumable frames, the compiler must preserve each affected call chain and resume
location, including newly introduced scheduling checks. The update records this
as a decision: compiler-inserted slice checks are opt-in with `[yield]` (per function or per type). Unmarked CPU-bound code can still stall a worker and cannot be cancelled; loop warnings, an advisory `loops` class in module files, and a watchdog diagnostic are the mitigations. The cost of a check is unmeasured ([D13](docs/architecture/open-decisions.md#d13-runaway-non-parking-code)).

### Risks introduced or sharpened by the module architecture

- **Borrowed locals against lazy frames.** The recommended lazy-frame strategy
  moves locals into a frame only when a park occurs, which invalidates borrows of
  them. The benchmarks that support lazy frames contain no borrowed locals, and
  patterns such as `recv(buf&)` are common. This could reduce how often the cheap
  path applies ([D2](docs/architecture/open-decisions.md#d2-borrowed-locals-and-the-lazy-shape)).
- **Inferred effects are part of the public contract.** An implementation change
  can widen a function's effect and ripple through dependants with no change to
  the source signature. This sits uneasily with "no hidden behaviour". The
  mitigations (contract diffs, an optional assertion, a version policy) are
  proposals ([D4](docs/architecture/open-decisions.md#d4-representing-effects-in-public-interfaces)).
- **Tooling carries more weight.** Suspension is invisible in source, and with
  lazy frames and fusion the logical stack no longer matches the physical one, so
  hover, diagnostics and stack-trace maps become part of the language's usability
  claim rather than extras.
- **Compiler and build complexity.** Effect inference, two code versions per
  function, a retained-IR module format and cache invalidation are substantial
  work. Build-time and module-size costs are unmeasured.
- **Foreign code.** Blocking C calls cannot be suspended. The design offers a
  blocking pool and a synchronous driver for callbacks, but the thread-hop cost
  and pool-exhaustion behaviour are unmeasured.
- **Performance expectations.** The stated goal is the same performance class as
  Rust async and Java virtual threads with a simpler model, not superiority. The
  [evaluation plan](docs/architecture/benchmarks.md) measures source ceremony
  alongside speed. That is the right test of the stated goal, and no end-to-end
  result exists yet.

There are other differences between documents and prototype behavior:

- Decisions select confined versus shared count strategies while the main
  specification still asks about them.
- The runtime interface states child-scope exit waits, while the design retains
  open questions about exceptional exits with children.
- The current C pool explicitly retains chunks in its first implementation,
  so earlier arena-reclamation experiments should not be read as a guarantee
  of the current server pool.
- Per-type allocation can improve locality and segregation, but objects of the
  same type can have very different lifetimes. Per-type arenas do not by
  themselves solve retention from long-lived objects scattered through chunks.

These discrepancies are normal in exploration, but an implementation needs one
authoritative semantic contract and a separate account of prototype limitations.

## What the benchmarks establish

The recorded [HTTP comparison](docs/journey/04-comparison.md) reports about
4.05 million requests/second for Strata-style C versus approximately 2.1 million
for Rust/Tokio and the Go pipelined server in a single-worker pipelined test.
It also reports much smaller idle memory per connection for the C prototype.

These are interesting results for the tested implementations. They support
continued investigation of task representation, buffering, allocation, and
connection handling. They do not show that equivalent Rust or Go implementations
could not adopt some of the same techniques.

The [full HTTP findings](bench/results/http.md) acknowledge single runs at many
points, loopback traffic, implementation differences, and omitted behavior.
Browser traffic also does not generally resemble deep HTTP/1.1 pipelining.
Comparing a specialized server with Go's general-purpose `net/http` includes
library functionality and policy choices as well as runtime overhead.

In particular, a throughput or tail-latency difference should not be attributed
to Go's GC without profiling or controlled evidence. Similarly, Tokio timer
behavior in a benchmark is not a universal limitation of Rust.

The next comparison should use equivalent protocol behavior, repeated runs,
matched concurrency and resource limits, realistic work, overload and slow-client
cases, and profiling. Report RSS and reserved address space separately, including
allocator retention, kernel resources where relevant, and steady-state memory
after churn. Add a Rust and Go mini-redis baseline to exercise shared-state costs.

## Where each language currently has the advantage

Rust is the strongest choice when a project needs established static safety,
precise ownership control, low runtime requirements, or broad systems integration.
Its complexity is real, but its compiler already handles the difficult cases
that Strata still needs to define and implement.

Go is the strongest choice when straightforward service development, automatic
memory management, runtime preemption, and mature operational tooling matter
more than deterministic memory reclamation or minimizing every allocation cost.
Its GC and race-discipline trade-offs remain real, but they are understood and
measurable in production.

Strata's potential advantage is a combination: sequential service code,
deterministic cleanup, static protection of shared mutable state, and efficient
logical tasks. No individual ingredient establishes superiority. The value would
come from delivering them together with substantially less programmer friction
for the intended workloads.

## Recommended next work

1. **Make the semantic contract authoritative.** Resolve the execution model,
   sharing rules, mutation permissions, child-scope behavior, and the distinction
   between explicit parking, scheduler preemption, and cancellation.
2. **Build a narrow compiler feasibility slice.** Include borrowed returns from
   multiple inputs, mutable aliases, growing collections, protected borrows,
   callbacks, separately compiled contracts (module files with effect summaries
   and Level-0 lowering to C), borrowed locals across parks, and deterministic
   cleanup.
3. **Test rejection and diagnostics as seriously as successful compilation.**
   Show why unsafe programs fail and how realistic safe programs express intent
   when inference is insufficient.
4. **Validate structured-task exits and ownership transfer.** Exercise errors,
   cancellation, child completion, partial construction, and final releases.
5. **Specify and model multi-object locking.** Test progress as well as mutual
   exclusion, including cancellation and mixed request types.
6. **Measure fairness and retention beyond peak throughput.** Include forgotten
   yields, long critical sections, destruction cascades, allocator churn, and
   mixed CPU/I/O work. Introduce compiler checks with measured cost and clear
   exemptions if automatic scheduling becomes part of the design.

The decisive milestone is generated, statically checked Strata code that remains
pleasant to write under those constraints. If it achieves that, the existing
runtime work gives the project a meaningful foundation. Until then, the strongest
claim is a promising design backed by useful runtime experiments, with its most
important safety and usability questions still ahead.
