# Group 2 — typed arenas (`c/arena_bench.c`, `rust/src/bin/arena_cmp.rs`, `run_arena.sh`)

One parameterized C arena (layout, chunk size/geometry, free policy, contraction, magazine) vs glibc malloc (same
harness) and Rust `slab::Slab` (64 B entries). Single thread pinned to one core, slot 64 B unless stated, n=2M
(128 MB live). Raw data: `arena.txt`, `arena_compact.txt`, `arena_mag.txt`, `slab.txt`. 85+ runs, zero corruption.
RSS is measured as a delta against a baseline taken after all harness arrays are touched.

## 1. Layout: contiguous reservation vs chunk table vs munmap
No measurable difference in alloc, free, churn or memory (fill 10.8/27.8 vs 12.0/24.7 ns; churn 58 vs 61; same
ratios; munmap == madvise). Only handle dereference differs (n=1M, ns per access):

| form | sequential | random |
|---|---|---|
| raw pointer | 1.60 | 3.22 |
| index into contiguous region | 0.83 | 2.95 |
| index via chunk table (fixed chunks) | 1.31 | 5.78 |
| index via geometric chunks (clz) | 1.84 | 6.97 |
| chunk table + generation check | 1.15 | 6.60 |

## 2. Chunk size (reserve, madvise, slot 64)
| chunk | fill a/f ns | churn ns | RSS/live after churn (compact, 6n) |
|---|---|---|---|
| 16 KB | 13.3/32.7 | 81.1 | not run |
| 64 KB | 10.9/28.0 | 65.2 | 1.03 |
| 256 KB | 10.8/27.8 | 58.5 | 1.10 |
| 1 MB | 10.8/25.3 | 51.3 | 1.20 |
| 4 KB -> 256 KB geometric | 10.8/27.7 | 56.7 | not run |
Geometric growth is free in speed and fixes the small-type floor (first chunk 4 KB). Smaller chunks reclaim more
and churn slightly slower; 16 KB is clearly too small (churn +40%).

## 3. Policy (which chunk to allocate from) and contraction
recent (stack) vs lowest-address: identical memory in every workload; recent is faster (37-40 vs 45-57 ns/step).
What matters is concentrating allocation in few chunks, not which ones. Contraction (madvise) returns memory only
when whole chunks empty:
- FIFO/age-ordered frees: 10.0x -> 1.03x
- random frees, or survivors spread every 64 KB: nothing returned (same as slab and malloc)
- compact workload (30% random kill, then random replacement churn, 2M objects):
  RSS/live after 6n steps: none 1.22; madvise 64 KB chunks 1.03; 256 KB 1.10-1.12; 1 MB 1.19-1.20.
  slab 1.22, glibc 1.53.
Cost of concentrating: 37-57 ns/step vs slab 17 ns (the freed slot is not the one reused: extra cache miss).

## 4. Magazine (per-worker LIFO of recently freed slots, recycled before touching chunks)
| mag | churn ns | compact ns/step | RSS/live after 6n | burst ratio |
|---|---|---|---|---|
| 0 | 39.3 | 35-37 | 1.03-1.10 | 6.9 |
| 16 | 17.8 | 9.6 | 1.22 | 3.4-3.6 |
| 64 | 18.9 | 9.6-9.7 | 1.22 | 3.6-4.3 |
| 256 | 17.5 | 9.6-9.8 | 1.22 | 4.2-6.1 |
Magazine restores slab-level speed (slab: 12-17 ns) and removes compaction (same 1.22 as slab). Unexpected benefit:
it separates recycled slots from net-new allocations, so long-lived objects pack together (burst 3.4 vs 6.9).

## 5. Against the comparators (slot 64, n=2M)
| | arena (mag=0) | arena (mag=64) | glibc malloc | slab |
|---|---|---|---|---|
| fill: alloc ns | 10.8 | 11.0 | 16.2 | 4.3 |
| fill: free (random order) ns | 27.8 | 16.9 | 49.3 | 4.8 |
| churn pair ns | 58 (256 KB) | 18.9 | 76.5 | 12.4 |
| RSS/live full | 1.01 | 1.01 | 1.25 | 1.02 |
| RSS after free-all | 0.3 MB | 0.3 MB | 152.7 MB retained | 123 MB retained |
Slot size (arena vs malloc, RSS/live full): 16 B 1.00 vs 2.00 (alloc 4.7 vs 10.3 ns); 256 B 1.00 vs 1.06;
1024 B 1.00 vs 1.02. glibc never returned memory after free-all (244 MB retained at 16 B slots).
Arena alloc has ~6 ns fixed overhead over slab's Vec push (chunk pick, bump, counters).

## 6. Findings
1. Layout choice is free; choose by what handles need. Contiguous reservation gives the cheapest index->pointer;
   chunk table costs +0.5 ns sequential, +2.8 ns random, geometric +1-4 ns.
2. Smaller chunks (64 KB) reclaim best; geometric growth from 4 KB removes the small-type floor.
3. Policy detail (recent vs lowest) is irrelevant; "concentrate allocation" is the property. Use the cheap stack.
4. Contraction only helps for age-ordered or chunk-granular frees; scattered survivors pin everything, as in slab/malloc.
5. Compaction and cache locality conflict: concentrating gives 1.03x memory at ~2x time; a magazine gives slab speed
   at slab memory (1.22x). Pick locality (magazine) by default; contraction stays opportunistic.
6. Memory retention after free-all is the clear win over glibc (0.3 MB vs 153 MB) and slab (123 MB).
7. Constraint discovered and RESOLVED (see section 7): MADV_DONTNEED zero-fills, so a generation stored in the slot is
   lost on decommit (confirmed by a standalone test: gen 7 reads back as 0, and a stale handle is accepted again once the
   counter climbs back, or immediately if it held gen 0). Fixed with a per-chunk epoch in the resident chunk header.
8. Long-lived objects scattering through shared chunks (6-11x retained after burst) is not solved by chunking or policy;
   it needs lifetime segregation, which per-type arenas provide by construction.

## Caveats
Single thread (no remote frees, no contention); 128 MB working set partly in this CPU's large L3; synthetic workloads;
no hugepage variant; generation/WeakRef not exercised beyond the deref microbenchmark; one allocation size per run.

## 7. Generational handles with decommit (chunk epoch) — implemented and tested

Design: 64-bit handle = 32-bit slot index | 32-bit generation. Generation = (chunk epoch << 20) | in-slot counter.
- The slot counter lives in the slot (bytes 4-7) and is bumped at the logical free, including when the slot goes into a
  magazine. It is lost on decommit, which is fine because:
- the chunk epoch lives in the resident chunk header and is bumped on every decommit, invalidating all outstanding
  handles to that chunk at once. The epoch is compared first, so a decommitted or munmap'd chunk is never read.
- Saturation: a slot whose counter reaches 2^20-1 is retired (not returned to the free list) until its chunk is
  decommitted; a chunk whose 12-bit epoch is exhausted is frozen (never decommitted again). Both degrade memory
  return, never safety. Bit split is configurable (sgbits).

Tests (`arena_bench work=handles`), all pass with 0 false accepts and 0 false rejects:
- T1 free/reuse/ABA on one slot
- T2 5,000 decommit cycles per layout (madvise and munmap), every handle ever issued re-checked each cycle; the epoch hit
  saturation (4095) and the chunk froze correctly
- T3 counter saturation at 8 bits with and without a magazine (19 slots retired over 5,000 reuses)
- T5 five 20M-op randomized fuzz runs against a model, decommit active, ~10M stale-handle probes in total
Negative controls (broken variants must fail): without the epoch bump, 25,005,000 stale handles were accepted
(madvise) and the munmap variant segfaulted on an unmapped chunk; with the magazine not bumping the generation,
millions of stale handles were accepted in 4 tests. So the tests discriminate.

Cost (n=1M, 256 KB chunks): full resolve (epoch + counter) 1.83 ns sequential, 7.87 ns random, vs 1.50 / 5.73 for the
unchecked chunk-table lookup and 1.27 / 6.85 for the earlier in-slot-only check. Part of the extra is the generic
chunk_of() math and bounds check, not the epoch compare. A/B of the allocator before/after the refactor: alloc and
churn unchanged within noise (churn 44.6-45.3 vs 44.8-47.5 ns); random-order free ~6 ns slower (~17%).

NOT covered (single-threaded only):
- A reader that passes the epoch check and then races a concurrent decommit can read a zeroed slot counter and wrongly
  accept a handle whose counter was 0. It needs an epoch re-check after the read (seqlock style) or deferred reclamation.
- munmap is unsafe against concurrent resolve (the reader can fault on the unmapped chunk). Use madvise only: a racing
  read then sees a zero page instead of faulting.
- Weak promotion must do the conditional strong-count increment and the generation check as one step.
