# Group 1 — fiber execution contexts

Machine: Ryzen 9 9950X3D (32 threads), 123 GB, Linux 6.12, gcc 14.2, rustc 1.93 / tokio 1.54, OpenJDK 25.
Kernel: THP=always, vm.max_map_count=1048576 (stock default is 65530), overcommit=0.
Raw output: `fiber.txt` (C), commands in `../run_fiber.sh`. Tokio and Java were run by hand (commands below).

## Idle footprint per unit (100k units, single scheduler thread)

| Unit | Idle state held | Resident bytes / unit | VMAs |
|---|---|---|---|
| Tokio task | 0 B | **395** | 1 |
| Tokio task | 1 KB | 1,419 | 1 |
| Tokio task | 4 KB | 4,491 | 1 |
| Tokio task | 16 KB | 16,778 | 1 |
| Strata fiber, pooled stack (no guard) | 1 KB | **4,113** (1 page floor) | 1 total |
| Strata fiber, pooled stack | 4 KB | 8,209 | 1 total |
| Strata fiber, pooled stack | 16 KB | 20,497 | 1 total |
| Strata fiber, pooled stack | 64 KB | 69,649 | 1 total |
| Java virtual thread (heap used) | 0 B | 809 | ~70 |
| Java virtual thread (heap used) | 4 KB | 4,848 | ~60 |
| Java virtual thread (RSS, incl. touched garbage) | 4 KB | 10,401 | |

## Stack provisioning (100k fibers, 64 KB stacks, 4 KB footprint)

| Mode | VMAs | Create ns/fiber | Teardown ns/fiber | Notes |
|---|---|---|---|---|
| pool (one mmap, no guard) | 1 | ~550 | ~130 | |
| poolguard (mprotect per stack) | 200,000 | ~1,730 | ~1,140 | **fails at 1M fibers** (map-count limit) |
| each (mmap per fiber + guard) | 200,000 | ~2,130 | ~2,010 | |
| malloc | merged | ~590 | ~150 | falls back to mmap above 128 KB: +5 KB/stack |

## THP

With THP=always and no `MADV_NOHUGEPAGE` on the pool:
64 KB stacks: 65,553 B resident per fiber (8x). 1 MB stacks: 968,462 B (118x), create 43 us/fiber.

## Switching (pinned to one core)

| Case | Cost |
|---|---|
| Fiber switch, 1k fibers (cache-resident) | ~13 ns |
| Fiber switch, 100k fibers | ~35-39 ns (a scheduler visit is 2 switches) |
| Tokio yield_now, 100k tasks | ~33 ns per yield |
| Java virtual thread yield | 530-670 ns (upper bound, includes unpark pass and carrier handoff) |

## Findings

1. Pooled stackful fibers have a 4 KB floor; Tokio idle tasks cost ~0.4 KB + live state. For small idle state
   the stackless layout is ~10x smaller; the gap only closes near page size.
2. MADV_NOHUGEPAGE is mandatory for any reserved-region design on this system's default THP setting.
3. Guard pages cannot be per-stack mprotect at scale (2 VMAs each). Use no guard pages plus a compiler-checked
   stack bound or a canary, or a small number of guard regions.
4. 1M pooled fibers: 8.2 GB resident, 64 GB virtual, 1 VMA. Address space is not the binding constraint at
   small reservations; resident memory and the page floor are.
5. Provisioning cost is not the problem: pooled create is ~0.55 us.

## Caveats

- C fiber runs were single measurements except the pinned repeats; the early unpinned switch numbers (50-100 ns) were noisy.
- The "scheduler" in the C benchmark is an ideal round-robin loop; Tokio's includes real scheduling work.
- Java RSS includes young-gen pages touched by garbage; heap-used is the fairer figure.
- Tokio multi-thread yield timing was invalid (harness race) and is not reported.

## Reproduce

    gcc -O2 -o c/fiber_bench c/fiber_bench.c && ./run_fiber.sh
    (cd rust && cargo build --release && ./target/release/tokio_idle 100000 4096 20 1)
    (cd java && javac IdleVThreads.java && java -Xms8g -Xmx8g -XX:+UseParallelGC \
       -Djdk.virtualThreadScheduler.parallelism=1 -Djdk.virtualThreadScheduler.maxPoolSize=1 IdleVThreads 100000 4096 20)

---

# Option C — compiler-sized frames (`c/frames_bench.c`, `run_frames.sh`, `frames.txt`)

Hand-written C state machines as a Strata compiler would emit them: one exact-size frame per parking function,
each frame type in its own arena (256 KB chunks, NOHUGEPAGE, free list), non-parking code on the native stack.
Pinned to one core. "depth" = parking calls nested under the root besides the leaf (frames = depth + 2).

## Resident bytes per idle task (100k tasks, arena)

| Live state held | depth 0 | depth 4 | depth 16 | Pooled fiber (4 KB floor) | Tokio |
|---|---|---|---|---|---|
| 0 B | 137 | 393 | 1,161 | 4,113 | 395 |
| 1 KB | 1,166 | 1,422 | 2,189 | 4,113 | 1,419 |
| 4 KB | 4,234 | 4,490 | 5,258 | 8,209 | 4,491 |
| 16 KB | 16,730 | 16,986 | 17,754 | 20,497 | 16,778 |

1M tasks, depth 4: 392 B (no state), 1,420 B (1 KB state) -> 392 MB / 1.4 GB total.
Frame size = live state + 64 B root header + 64 B per nested level (header is oversized, see below).

## Wake/yield cost (ns per yield, ideal scheduler)

| depth | nested resume (O(depth)) | direct resume (innermost frame) |
|---|---|---|
| 0 | 3.4 | 2.9 |
| 4 | 7.5 | 3.2 |
| 16 | 26.2 | 6.3 |

At 1M tasks (cache-cold) direct resume costs 12-14 ns.

## Allocator

arena vs malloc, depth 4: 393 vs 489 B/task with no state (+25%), spawn 9 vs 20 ns.

## Findings

1. Idle memory is live state + ~130 B for a shallow task; no page floor. 10x smaller than a pooled fiber at 0-1 KB of
   state, ~2x at 4 KB, equal near 16 KB.
2. Nested resume (what Rust async does) costs O(depth): 4x slower at depth 16. Direct resume of the innermost frame
   is nearly depth-independent. It requires the compiler to control the call convention.
3. The 64 B frame header is bloated for the benchmark; a compiler can emit ~16 B (resume pointer + parent) and drop
   the run-queue/leaf fields from non-root levels, so deep chains would shrink from 64 B/level to ~24 B/level.
4. Exact-size per-type arenas beat malloc by ~100 B and ~2x on spawn for small frames.

## Caveats (what this does NOT show)

- Hand-written; no compiler analysis. The real cost is liveness analysis across park points, plus a "may park"
  effect that must be transitive through function pointers, closures, trait/dynamic dispatch and generics.
- No I/O integration (wake is an enqueue), no cancellation, no wakers, no join handles; Tokio's 33 ns yield includes
  real scheduler machinery, so the 3 ns here is not a like-for-like speedup.
- Recursion with parking calls needs one heap frame per call (shown only as depth chains, not unbounded recursion).
- Blocking C/FFI calls run on the worker's native stack and block the worker; a blocking pool is still needed.
- Frame working set (128-384 B) fits cache at 100k tasks, which flatters switch cost relative to the fiber numbers.
