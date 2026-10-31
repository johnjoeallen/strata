# Decisions

Rules the design has settled on, with the reason each was chosen.

| Decision | Why |
|---|---|
| Nothing hidden except virtual threads and parking | Programs stay predictable; batching and buffering must be asked for. |
| Every unbounded input has a declared limit | Memory is bounded before data is held; big bodies are streamed or spooled. |
| No biased reference counting in v1 | Equal or worse once objects cross threads; confined types get plain counts. |
| Per-type arenas, no compaction | Segregates lifetimes; addresses stay stable, which the lock order relies on. |
| Chunk epochs in resident chunk headers | `MADV_DONTNEED` zero-fills decommitted generation words. |
| `MADV_NOHUGEPAGE` on stack pools | With transparent huge pages a 64 KB stack costs 65 KB resident. |
| Errors are exceptions with `throws` | Throwing is visible in signatures; no `throws` clause means nothing escapes. |
| Cleanup never parks and never throws | It runs on every exit path, including unwinding. |
| `sync`/`mutate` acquisition parks, FIFO | Fairness, and workers keep running other vthreads. |
| Spin briefly before parking | Pure parking lost up to 75% of throughput under mixed load. |
| Multi-object heads acquire all-or-nothing | Mis-ordered heads cannot deadlock. |
| Default slow-subscriber policy: `Disconnect` | Never silently loses messages; `Notice` is available. Not yet confirmed by the user. |
| Strata identifiers are camelCase | User preference. Constants stay UPPER_CASE; generated C may use underscores. |
| Syntax: `T?`, methods in types, optional `self`, `Pattern -> {}` arms | Avoids Rust-isms. |
| No third-party crates in converted programs | Strata would have no equivalents. |
| Performance goal: same class as Rust async and Java vthreads, with a simpler model | Superiority is not claimed; ceremony is measured alongside speed. |
| Programming model (vthreads) is separate from implementation (resumable frames) | No feature may depend on the implementation; stackful runtime kept as reference. |
| Compiler inserts budget checks in loops and recursion unless the function is tagged `nopark` | Long CPU work cannot monopolise a worker or defy cancellation; `nopark` gives plain C where the author wants it. Overhead still to be measured. |
| One `Ref<T>`; sharing is inferred, not declared. `sync`/`mutate` required where the object is not provably local and the field is mutable; no-ops on unshared objects | Programmer never decides what is shared; a missing or too-weak block is a compile error; layout and behaviour do not depend on other modules. Cost of per-instance flag still to measure. |
| C stays the backend | Under the module architecture too. |
| Strata source and C live in separate folders | `stra/` is Strata only; `c/` is runtime and compiler-output stand-ins. |
