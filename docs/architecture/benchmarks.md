# Evaluation plan

The question is not "is Strata faster?" It is: **does Strata reach the same performance class as Java virtual threads and Rust async, with a measurably simpler program?** Both halves are measured. A result that is slightly slower with much less source ceremony is a success; a result that is faster with a more complicated model is not the goal.

## What exists, and what it does not show

The HTTP, mini-redis and micro benchmarks in `bench/` are promising (see `docs/journey/` and `bench/results/`). They are hand-written C beside hand-written Rust/Go/Java. They do **not** show that a compiler can generate the same code, and the simple HTTP handler is syscall-bound at roughly 3 microseconds a request, so throughput there is nearly equal by construction. They are not representative of applications with deep call chains, databases, TLS, dispatch or cancellation. Treat them as evidence about the runtime, nothing more.

## Ground rules

1. **Equivalent behaviour.** Same protocol, same limits, same error behaviour, same cleanup, on the same wire. Each variant passes the same black-box test suite before it is timed (the existing `run_tests.sh` pattern).
2. **Comparable I/O models.** Strata (readiness plus epoll) against Tokio (epoll) and Java virtual threads on the JDK's NIO selector. Not against blocking thread-per-request unless that is the stated question.
3. **Idiomatic baselines.** The Rust and Java versions are written by someone who would ship them, with the executor and pool settings tuned and recorded.
4. **Pre-registered bars.** Before measuring, write down what counts as "same class" (for example, within a stated factor on throughput and p99 latency, and no worse on memory). The factor is set once and not adjusted afterwards. **No factor is proposed here**; it is a decision for the author once the first end-to-end numbers exist.
5. **Report variance.** Repeated runs, pinned CPUs, fixed frequency, warm-up for the JVM, p50/p99/p99.9, not only means.
6. **Scheduler cost is separated from application cost.** Each workload is run with a *null* body (spawn, park, resume, finish) to get the pure scheduling and frame overhead, and with the real body. Reported as: `total = scheduling + application`.
7. **Nothing is claimed beyond what ran.** A benchmark that has no Strata compiler behind it is labelled hand-written.

## Workloads

| # | Workload | What it tests | Notes |
|---|---|---|---|
| B1 | Minimal HTTP handler | Floor cost; allocation per request | Exists (`bench/http`). Syscall-bound. |
| B2 | Deep call chain, park at depth 1, 4, 8, 16, 32 | Frame creation, direct-resume benefit, effect inference | Extends `frames_bench` and `lazy_bench`. Generate chains with a compiler when one exists. |
| B3 | Sequential I/O: N dependent reads | Wake cost, scheduler fairness | |
| B4 | Concurrent I/O: fan-out and join | Child vthreads, structured scopes | |
| B5 | Database pool and queries | Pool wait as park, parked-frame memory, real wire protocol | Same database, same schema, same driver protocol. |
| B6 | Outbound HTTP and TLS | Native C interop; blocking vs nonblocking foreign calls | TLS library choice affects all variants; record it. |
| B7 | Dynamic dispatch | `May` bound on a hot interface call, status test, thunks | Include devirtualised and non-devirtualised builds. |
| B8 | Cancellation, timeouts and errors | Unwinding through frames, cleanup timing, leaked resources | Correctness first: every cleanup ran exactly once. Then latency of cancellation. |
| B9 | Very many vthreads | 100k to 1M idle and mostly-idle tasks, memory per task | Extends the existing fiber/frames tables. |
| B10 | Memory and allocation | Resident and virtual bytes, allocations per request, frame arena fragmentation | Report RSS and VSZ separately; run under THP on and off (a known trap in this project). |
| B11 | Blocking FFI | Offload pool: latency of the thread hop, behaviour when the pool is full | Compare strategies 2 and 4 of [dispatch](dispatch-and-ffi.md#blocking-foreign-calls). |
| B13 | Slice-check overhead | Tight loops (hashing, parsing, copying, `brc`-style scans) as plain C, with `[yield]` checks, and with strip-mined checks; time to yield and fairness under a CPU-bound task | The cost of opting in. Also decides whether default-on is ever worth revisiting. |
| B12 | Mixed realistic service | Parse, route, query, render, with a shared cache (`sync`/`mutate`) | The end-to-end result everything else supports. |

Metrics for all: throughput, CPU utilisation (user, system), p50/p99/p99.9 latency, resident memory, allocations, context switches.

## Ceremony metrics (the half that matters most here)

For each application, written equivalently in Strata, Rust and Java:

- Lines of code and number of tokens.
- Count of `async`/`await`, `Pin`, `Box<dyn ...>`, lifetime names, `Arc`/`Mutex` wrapper types, executor/runtime setup lines.
- Count of explicit cancellation or resource-handling constructs.
- Whether a reviewer who has not seen the code can state where it may wait (needs tooling support for Strata; its quality is part of the result).

The Rust and Java versions are not made artificially verbose; the comparison is only meaningful for code each community would write.

## Build and tooling measurements

Because the module architecture trades build time for runtime, these are measured as results, not assumed:

- Clean build; incremental rebuild after (a) a body change that keeps the contract, (b) a contract-changing change, (c) an effect-widening change at depth. Report modules recompiled and wall-clock.
- Optimised link time against application size; with and without cached products.
- Module file size by tier (A, B, C) for a representative library.
- Whether the output of optimised mode beats incremental mode by an amount that justifies it. If it does not, that finding stands.

## Milestones

1. Hand-written B2 and B7 with D from 1 to 32 in the shape the lowering of [suspension](suspension.md) produces, including borrowed locals (the case the toy omitted).
2. First compiler emitting Level-0 C: rerun B1, B2, B3, B9 on generated code and compare with the hand-written versions. Any gap is a compiler quality finding.
3. B5, B6, B8, B11 once the runtime has the needed libraries (database driver, TLS binding).
4. B12 and the ceremony metrics.
