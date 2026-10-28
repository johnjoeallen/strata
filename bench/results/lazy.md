# Lazy frames vs eager vs embedded (`c/lazy_bench.c`, `run_lazy.sh`, `lazy.txt`)

Same logical program in four forms (checksums verified equal for every D/p/W): a request loop calling a chain of
D+1 parking functions with a live local across each call; the leaf parks with probability p.
2000 tasks x 2000 requests, pinned to one core, -O2. Frames are 64 B, hot in L1 (LIFO free list), so this is the
best case for eager allocation.

- plain: ordinary calls, cannot park (floor)
- eager: heap frame per call, state machine, synchronous callee resume, direct resume of the leaf
- embed: callee frames inside the caller's frame, one allocation per task
- lazy: native-stack fast version; frames created only while unwinding a real park; separate resume version

## ns per request (W=8: tiny leaf work)

| D | variant | p=0 | p=0.01 | p=0.1 | p=1 |
|---|---|---|---|---|---|
| 4 | plain | 3.4 | | | |
| 4 | eager | 10.4 | 10.8 | 12.7 | 18.0 |
| 4 | embed | 7.7 | 8.0 | 10.1 | 15.4 |
| 4 | lazy | 6.8 | 7.2 | 9.0 | 17.0 |
| 8 | plain | 4.5 | | | |
| 8 | eager | 18.2 | 18.7 | 21.6 | 35.1 |
| 8 | embed | 13.2 | 13.7 | 16.8 | 28.2 |
| 8 | lazy | 10.5 | 11.5 | 14.1 | 30.6 |

With W=64 (leaf work ~30 ns) the gaps shrink: D=8, p=0: plain 35.9, eager 57.5, embed 51.3, lazy 44.6.

## Per extra parking call, p=0 (D=8, W=8): eager ~1.5 ns, embed ~1.0 ns, lazy ~0.7 ns over plain calls.
Allocations per request: eager D+1; embed 0; lazy = p x (D+1).

## Compiled code size (-O2, no inlining) of the call-chain functions
- eager: level_heap + root_heap = 719 B
- embed: level_embed + root_embed = 451 B
- lazy: lazy_level_fast + lazy_level_resume + lazy_root = 868 B  (1.2x eager, 1.9x embed)
This is a toy: one call per function, one live local. Real functions have many park points and spill sets.

## Findings
1. Lazy beats eager by 3.6 ns/request at D=4 and 7.7 ns at D=8 when parks are rare (p<=0.01). Embedded gets
   2.7 / 5.0 ns of that with no second code version. Lazy beats embed by 0.9 / 2.7 ns at p=0.
2. Once p reaches ~0.1, lazy and embed are within noise of each other; at p=1 lazy is slower than embed by 1.6-2.4 ns
   (spill code on every call) and about equal to eager.
3. The whole difference between strategies is ~1 ns per parking call. A request that does any real work (parse,
   syscall, lock, DB) costs thousands of times more. My earlier estimate of 20-30 ns saved per request was too high;
   measured savings are 4-8 ns on a 5-10 deep chain.
4. Lazy code is 1.2x eager here but needs two versions that must agree on frame layout and state numbering.
5. Parked memory is identical for lazy and eager (same frames at the same points); embed reserves the whole chain.

## Caveats
Microbenchmark; hot frames; D+1 frames of identical shape; no cache misses on frames, no real wake latency.
In a real server, frames of 100k parked tasks are cold when resumed in all variants equally.
