# Changes and contradictions

## What this update changed

| Area | Before | Now |
|---|---|---|
| Virtual-thread implementation | Spec 6.1: stackful fibers with independently managed stacks | Programming model unchanged; implementation is compiler-generated resumable frames (Direction). Stackful runtime kept as reference and baseline |
| Parking | Spec principle 10, leaves.stra: inferred may-park effect, informally | Defined as a lattice (`Never`, `Cond(P)`, `May`) with propagation rules, exported in contracts |
| Separate compilation | Spec 5.1: interface artifact with borrow origins and effects | A full module format with three information tiers, hashes, versions and a native-cache key |
| Compiler architecture | Not described (Phase 0 and 5 only) | The pipeline in [index](index.md); C stays the backend |
| Interfaces, function values, FFI | Spec 5.2, 9, 10.9 mention them | Bound-based dispatch, `extern blocking`, callbacks, dynamic loading rules |
| Evaluation | Spec Phase 6: "against Tokio or Java vthreads" | A workload set, ground rules, ceremony metrics, build metrics |
| Sharing | Undecided: `shared var`, shareable `Ref<T>`, or a type-level declaration | One `Ref<T>`; sharing inferred; blocks required by path analysis ([sharing](../sharing.md)) |
| Open questions | 10.8 stacks, 10.10 API stability | Superseded and refined into [D1 to D13](open-decisions.md) |

The syntax of existing Strata is unchanged. Proposed additions, each marked placeholder and Open: the attributes `[yield]`, `[noyield]` and `[nopark]` (control of compiler-inserted budget checks and an assertion of no parking), and `extern blocking`.

## Contradictions found, and how each is resolved

1. **Stackful fibers vs resumable frames.** Spec 6.1 said stackful; the C runtime (`c/runtime/rt.h`) and `frames_bench` use sized frames; `ASSESSMENT.md` flagged this. **Resolved:** frames are the implementation direction; 6.1 is rewritten; the stackful runtime is demoted to reference and baseline. The frame/idle-memory numbers are labelled hand-written and are not evidence about generated code.
2. **Stack-size questions.** Spec 10.8 (fixed reserve vs segmented stacks) assumed stackful. **Resolved:** applies only to the stackful baseline and the native stack used by fast paths; the 4 KB floor and guard-page findings remain valid for that baseline.
3. **"Lifetime contracts are compile-time only" vs executables needing effect info.** Spec 5.1 says metadata is a compile-time dependency and the executable need not contain it. Still true for Tier A; but park-point tables (debug) and Level-0 entry names are in the executable if tooling is wanted. **Resolved:** debug sections are optional and separable.
4. **Borrow across a park vs the lazy frame strategy.** The `lazy` measurements assumed no borrowed locals. **Flagged:** [D2](open-decisions.md#d2-borrowed-locals-and-the-lazy-shape).
5. **"No hidden behaviour" vs inferred effects ripple.** An implementation change can now change a contract. **Flagged, partly resolved:** the exception in principle 10 is extended to effect contracts, mitigated by contract diffs, optional assertions and a version policy ([D4](open-decisions.md#d4-representing-effects-in-public-interfaces)).
6. **Cooperative cancellation and scheduling vs tasks that never park.** Previously a limitation. **Resolved by decision:** the compiler inserts budget checks only in functions marked `[yield]` (or a file marked so), which makes those loops yield and cancellation points. Unmarked CPU-bound code can still stall a worker; warnings, a watchdog and the file-level default address it. Remaining gaps and the overhead question are in [D13](open-decisions.md#d13-runaway-non-parking-code).
7. **Foreign callbacks vs vthreads.** A callback cannot be suspended. **Resolved** by the `Never` default and the synchronous driver on pool threads.

## Outstanding decisions

Short list; reasoning in [open decisions](open-decisions.md).

D1 where lowering happens; D2 borrowed locals; D3 minimum IR; D4 effects in public interfaces; D5 recursion and dispatch limits; D6 generics; D7 fusion across modules; D8 debugging and profiling; D9 dynamic loading; D10 blocking FFI; D11 compatibility policy and caches; D12 cancellation semantics; D13 budget-check overhead and unit (mechanism decided: opt-in `[yield]`).

## Not yet verified anywhere

- Any compiler output. All Level-0 C is illustrative or hand-written.
- Exceptions as status returns in generated C.
- Build-time and module-size costs.
- The frequency of borrowed-across-park locals in real handlers.
