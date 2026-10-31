# Open decisions

Each item gives the alternatives, the trade-off and a recommended direction. A recommendation is not a decision: it is what to build first and test.

## D1. Where continuation lowering happens

- **At module compile.** Every function is lowered to the Level-0 convention; the module's C and object are final. *Pros:* fast incremental builds, no IR needed to link, predictable. *Cons:* no cross-module frame fusion.
- **At application link.** Modules ship IR only; lowering runs on the whole program. *Pros:* best optimisation. *Cons:* slow links, requires IR from every dependency, weak caching.
- **Both (recommended).** Lower at module compile. In optimised mode, re-lower only chains whose inlining or fusion changes them, using retained Tier C IR.

Recommendation: both, with module compile authoritative. Release a first compiler that does only module-level lowering.

## D2. Borrowed locals and the lazy shape

The `lazy` strategy copies locals into a frame only on a park, which moves them. A borrow of a moved local dangles. See [suspension](suspension.md#borrows-and-locals-that-cannot-be-moved).

- **Frame-resident borrowed locals; the function uses eager frames** (recommended). Correct; loses the lazy saving for functions with borrowed-across-park locals.
- **Pin the local's storage in the task arena.** The local is in the arena from the start, not the stack. Cheaper than a full frame; one more arena operation.
- **Forbid borrowing across a park.** Simple; too restrictive (`recv(buf&)`).

Recommendation: the first, then measure how many hot functions it affects. The toy benchmarks did not include this.

## D3. The minimum IR for cross-module optimisation

Alternatives: (a) full typed IR for all functions; (b) IR for functions under a size threshold and generic ones only; (c) author-marked inlinable functions only.

Recommendation: ship (b) by default as Tier B plus a size-limited Tier C, with the full IR as an option for libraries that want fusion. Measure size against build gain before deciding the default. Minimum requirements on any IR: explicit ownership operations, call-site effects, park points, cleanup scopes, source-independent.

## D4. Representing effects in public interfaces

Alternatives: (a) infer everything and export it (current plan); (b) require the author to declare the effect of public functions; (c) infer, and let the author optionally assert a bound.

(b) reintroduces colouring. (a) alone lets an implementation change ripple silently through dependants. Recommendation: (c) - an optional `noparks` assertion (placeholder syntax), plus tooling that prints the contract diff between versions, and a version policy that treats widening as breaking. Open: spelling of the assertion, and whether to allow `parks` on non-extern functions as a "may park" promise for stability.

## D5. Recursion and dynamic dispatch

Recursion: SCC fixed point; per-activation frames; depth-counter fallback to frames on the fast path (see suspension). Dispatch: declared bound, default `May`. Open: the depth limit, and recursion through interfaces where the bound is the only information. Recommendation as written on those pages; test with a recursive-descent parser handler and a visitor over a tree.

## D6. Generics: monomorphisation or shared code

Monomorphised: exact frames and effects, requires Tier B IR, code growth. Shared with dictionaries: no IR dependence, slower, hard to give generic frames a layout. Recommendation: monomorphise; revisit if code size hurts.

## D7. Can frame fusion work across separately compiled modules

It can only when the callee's IR is present (Tier C) at link time. It cannot at module compile. If the callee's module ships A+B only, fusion simply does not happen across it. Alternative: export a *fixed* frame layout for chosen functions so callers can embed them without IR. This freezes the layout (breaking ABI to change). Recommendation: fusion at link time from IR only; no fixed layouts. Measure the benefit before building it: the toy numbers suggest about a nanosecond per call.

## D8. Debugging, stack traces and profiling

A parked task's native stack is gone; the logical stack is the frame chain. Fusion, lazy frames and inlining make logical frames not correspond to physical ones.

- **Park-point tables in every module** (recommended): `(frame type, state) -> chain of logical frames with source locations`, as DWARF-style inline info. Tooling walks the frame chain for parked tasks, and the native stack plus the chain for running ones.
- Profilers sample the native stack and append the chain. Needs a runtime hook.
- Alternative: no maps; stack traces show only physical frames. Cheap, poor for diagnosing the "may park" chain which principle 10 demands tooling show.

## D9. Dynamic module loading and whole-program optimisation

Aggressive devirtualisation and fusion assume a closed world; plugins break it. Alternatives: no dynamic loading; open interfaces declared in build configuration; a runtime flag that turns off closed-world assumptions entirely. Recommendation: no dynamic loading in the first compiler; keep bound-based dispatch so it can be added with open interfaces declared in configuration.

## D10. Blocking FFI

Offload pool, dedicated thread, or run on worker. See the table in [dispatch](dispatch-and-ffi.md#blocking-foreign-calls). Recommendation: nonblocking where possible; `extern blocking` offload by default; watchdog diagnostics.

## D11. Versioning module compatibility and native caches

Format, ABI, contract hashes, cache keys as in [modules](modules.md#4-versioning-and-compatibility). Open: whether the compatibility policy is enforced by the compiler (refuse a minor-version consumer of a major-breaking producer) or only reported; how long caches live; reproducible builds and cache portability across machines.

## D12. Cancellation as a non-exception

Recommended: not in `throws`, not catchable, runs cleanups. Alternative: a catchable exception that must be rethrown. Interacts with the exception model (spec 10.7), and with child-vthread scopes.

## D13. Runaway non-parking code

Without preemption, a function that neither parks nor ends stalls its worker and cannot be cancelled. Compiler-inserted checks (see `ASSESSMENT.md`) widen every effect that contains a loop. Recommendation: not in the first compiler; if added, a build setting recorded in module headers and in the cache key.
