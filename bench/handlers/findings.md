# "May park" classification of realistic handlers

Source: `handlers.stra` (hand-written pseudo-Strata, every call tagged by hand), tally: `python3 tally.py handlers.stra`.
Caveat: eight handlers I wrote myself, tagged by me, trivial accessor calls mostly untagged. It shows the shape of the
problem, not a measured distribution over real code.

## Counts
- 24 functions: 9 non-parking (native stack), 15 parking (need frames).
- 84 tagged call sites: 50 N, 31 P, 2 blocking-foreign, 1 indirect. 40% of call sites are parking-class.
- Frame state per parking function (live across the longest park): mean 71 B without large locals; max 120 B.
  One function (`proxy`) holds a 4 KB buffer across parks and is 4,168 B.

## Idle state while parked (excluding per-frame header: 64 B in the benchmark, ~16 B achievable)
| parked at | frames | state bytes |
|---|---|---|
| get_user, cache hit (lock only) | 2 | 104 |
| get_user, waiting on db | 2 | 184 |
| report, waiting on db rows | 2 | 200 |
| routed get_user via dispatch/auth, on db | 4 | 336 |
| recursive walk, depth 8 | 8 | 512 |
| subscriber idle on bus | 1 | 64 |
| proxy, waiting on client read | 1 | 4,168 |

## Findings
1. Locks make functions parking. `cache_get`, `cache_put`, `verify_token` and `broadcast` are parking solely because
   lock entry can wait. 4 of 15 parking functions, plus every caller. On the cache-hit path the lock never actually
   waits, yet the callers still need frames. Mitigation: lazy frames (run on the native stack, materialize the frame
   only when a park actually happens) - needs two compiled versions of each parking function.
2. Indirect calls: one site (`next(...)` in `auth`) has targets {get_user: P, report: P, health: N}. The site is parking.
   Function values must carry the callee's frame size and an adaptor so a non-parking target can run inline without a
   frame. Closures passed to generic functions (`sort_by`, `group_by`, `map`) cost nothing: whole-program
   monomorphization sees the target, and those calls stay N.
3. Blocking foreign calls need a third class beyond N/P: `read_dir` and `c_resize` (B) must be offloaded to a blocking
   pool, which parks the caller. Extern declarations must say so. An unannotated extern defaults to N and can stall a worker.
4. Large locals live across a park go into the frame. A 4 KB read buffer makes the frame 4 KB, so memory per idle
   proxy connection is no better than a fiber. Either the language puts big or variable-size locals in the task arena
   (frame holds a pointer), or accepts it. This applies to every model, including Tokio.
5. Recursion that parks (`walk`) works: one small heap frame per level, no stack overflow, bounded only by memory.
6. A parking call inside `sync`/`mutate` is a compile error, matching the design rule; the fix is to move it out of the lock.
7. Roughly 60% of functions here became parking, but compute (parse, render, sort, map, JSON) stayed N and runs at
   native speed on the shared worker stack. The risky population is I/O-heavy glue, which is small and shallow.

## Not answered
- Real code distribution. Needs real handlers (or a corpus) and a real effect checker.
- Whether lazy frames pay for the 2x code size.
- Task-local arena sizing for big locals (first-chunk size becomes the new per-task floor if it is a full page).
