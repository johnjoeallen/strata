# Architecture: compilation, suspension and modules

This section extends [`strata-design.md`](https://github.com/johnjoeallen/strata/blob/main/strata-design.md) (sections 5, 6 and 14). It is a design update, not an implementation plan for the compiler to start tomorrow. There is still no compiler: the C in `c/` is hand-written to look like compiler output.

| Page | Covers |
|---|---|
| This page | Objective, the two models, the pipeline, what happens where |
| [Suspension and continuations](suspension.md) | The may-park effect, resumable frames, recursion, cancellation, cleanup, fusion |
| [Compiled modules and linking](modules.md) | The module format, the three information tiers, ABI and IR boundaries, incremental versus optimised builds, recompilation rules |
| [Dynamic dispatch and external code](dispatch-and-ffi.md) | Interfaces, function values, generics, C interop, blocking foreign calls, callbacks, dynamic loading |
| [Evaluation plan](benchmarks.md) | How Strata is compared with Java virtual threads and Rust async |
| [Open decisions](open-decisions.md) | Unresolved questions, alternatives, recommended direction |
| [Changes and contradictions](changes.md) | What this update changed in the existing design, and what it contradicted |

## Objective

The target is **excellent performance with a simpler programming model**, not performance superiority. A Strata server should be written as plain sequential code (no `async`/`await`, no lifetime names, no pinning, no executor choice) and should land in the same performance class as hand-tuned Rust async or Java virtual-thread code. When the two disagree, the simpler source wins, provided the cost is measured and stated. The evaluation plan therefore measures source ceremony as well as speed.

## Status labels

Every claim on these pages carries one of four statuses, so that speculation is not read as decision.

| Label | Meaning |
|---|---|
| **Decided** | Already settled in `docs/decisions.md` or the specification. |
| **Direction** | The recommended choice. Reasoned but not validated by a compiler. |
| **Open** | Alternatives are listed; no recommendation is binding yet. |
| **Measured** | Backed by a result in `bench/results/`. These are hand-written C, so they bound what a compiler could achieve; they do not show that one does. |

## Two models, kept apart

**Programming model (Decided).** Virtual threads. A vthread runs ordinary sequential code; a call that may wait looks like any other call. The programmer sees stackful-like semantics: locals live across a wait, a `try` spans it, cleanup runs when scopes exit, and a cancelled task unwinds.

**Implementation model (Direction).** Compiler-generated resumable frames, which are stackless continuations. The compiler finds the calls that may park, gives each function that can suspend a frame holding exactly the state live across its suspension points, and generates a state machine. There is no per-vthread machine stack.

Nothing in the language may depend on which implementation is used. A program that behaves correctly under stackful fibers must behave identically under resumable frames. That is the test for any proposed feature, and it is why the existing stackful runtime stays useful as a reference (see [suspension](suspension.md#where-stackful-execution-remains)).

"Park" and "suspend" mean the same thing. The specification and the C runtime say *park*, so this section does too, and calls the inferred property the **may-park effect**.

## The pipeline

```
Strata source (.stra)
    |
    v
Parsing, name resolution, type checking
Borrow/lifetime inference, effect inference (local)
    |
    v
Typed Strata IR (per module, ownership and effects explicit)
    |
    v
Compiled Strata module (.smod) -------------------------------+
    |  interface: types, signatures, borrow origins,           |
    |             effect summaries, dispatch contracts         |
    |  retained IR (generic bodies, inlinable bodies, the rest)|
    |  call-graph edges, park-point tables                     |
    |  optional: cached C and native object per target         |
    v                                                          |
Module-level lowering (always)  <-- Level-0 suspendable        |
    |  functions to the stable calling convention              |
    |  C emitted, object cached                                |
    v                                                          |
Application link (.smod files + runtime)  <--------------------+
    |  module resolution, contract-hash check
    |  closed-world call graph, cross-module effect check
    |  lifetime check across modules
    |  [optimised mode only] devirtualise, inline, fuse frames, re-lower
    v
Generated C            (C remains the backend)
    |
    v
Native C compiler and linker
    |
    v
Executable
```

### What happens where

| Step | Module compile | Application link | Notes |
|---|---|---|---|
| Parse, type check, borrow and effect inference within the module | Yes | | Needs only the module and the interfaces it imports |
| Typed IR and module file | Yes | | |
| Lower every function to the Level-0 calling convention | Yes (**Direction**) | | Makes a module linkable with no IR from anyone else |
| Check imported contract hashes | | Yes | Also at module compile, against the interface used |
| Whole-program effect resolution (conditional effects, interface calls) | | Yes | Cheap; works on summaries, not IR |
| Devirtualise where the implementation set is closed | | Optimised mode | Needs retained IR only if the target is then inlined |
| Cross-module inlining and frame fusion | | Optimised mode | Needs retained IR |
| Lowering of fused or embedded frames | | Optimised mode | Output replaces the module-level code for the chains it touches |
| C compile and link | Per module (cached) | Once | |

Whether module compilation lowers fully, or lowering waits for the link, is [open](open-decisions.md#d1-where-continuation-lowering-happens). The recommendation is both: lower at module compile so a build with no optimisation never needs the retained IR, and re-lower only what fusion changes.

A minor source change must not mean a whole-program rebuild. The rules that guarantee this are in [recompilation rules](modules.md#when-a-module-needs-recompiling).
