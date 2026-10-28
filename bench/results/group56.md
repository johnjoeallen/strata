# Groups 5-7 — concurrency: promotion races, remote frees, refcounts
Machine: 9950X3D, CPUs 0-15 are distinct cores (0-7 on one L3, 8-15 on the other), threads pinned to CPU 0..T-1,
x86 only (TSO), -O2. Programs: `c/promote_stress.c`, `c/conc_alloc.c`, `c/refcount_bench.c`.
Data: `conc_alloc.txt`, `refcount.txt` (promotion stress was run by hand, commands below).

## 1. Concurrent weak promotion vs free / reuse / decommit (`promote_stress`)
Writers publish (handle, seq) into a seqlock table then drop them in bursts so whole 4 KB chunks drain and are
madvise-decommitted; readers promote live and stale handles and check that the object's payload seq matches.
- A: slot word = {full generation incl. chunk epoch : 32 | strong : 32}; promote = one CAS on that word;
  release bumps the generation in the same CAS that takes the count to zero.
- B: slot word holds only the in-slot counter; chunk epoch checked separately before the CAS
  (the design from the single-threaded tests).

| delay in window (pause iters) | A violations | B violations |
|---|---|---|
| 0 (30 s A, 5 s B) | 0 (590M promotions) | 0 |
| 200 | 0 | 1 |
| 1000 | 0 | 5 |
| 5000 | 0 | 103 (166 at sgbits=20) |
A also passed: sgbits 4, 6, 20, 24, 28 and delay up to 20000, ~1.5M decommits total, plus epoch exhaustion (decommits
capped at 477 for a 15-epoch space; chunks freeze). Each ABA violation is also counted as a use-after-free (same event).
Finding: B has a real TOCTOU race (epoch read, then free+decommit+realloc, then CAS). Without injected delay the window
is too narrow to hit, so a clean run of B proves nothing. A closes it because a zeroed slot reads strong=0 and a reused
slot carries a different full generation. Caveats: allocator under a mutex (only the promote/release fast paths are
lock-free); x86 only; madvise only (munmap is unsafe against a concurrent resolve).
Reproduce: `./promote_stress mode=A|B sgbits=6 delay=5000 seconds=5`.

## 2. Remote frees (`conc_alloc`): Mpairs/s total, 64 B objects, handoff to next thread, batched harness handoff
| remote | mode | 1T | 2T | 4T | 8T | 16T |
|---|---|---|---|---|---|---|
| 0 | owned (per-thread arenas) | 533 | 833 | 1599 | 3109 | 4729 |
| 0 | glibc | 81 | 163 | 311 | 623 | 1186 |
| 0 | global mutex | 58 | 14 | 11 | 10 | 5.6 |
| 0 | global lock-free stack | 121 | 19 | 12 | 7 | 4.4 |
| 0.1 | owned_b | 496 | 262 | 515 | 1078 | 1887 |
| 0.5 | owned_b | 502 | 111 | 244 | 418 | 740 |
| 1.0 | owned | 538 | 59 | 142 | 274 | 455 |
| 1.0 | owned_b (batch 32) | 495 | 85 | 200 | 356 | 601 |
| 1.0 | glibc | 86 | 34 | 75 | 139 | 257 |
owned = chunk header names the owner; remote frees CAS onto the owner's stack; owner drains by exchange.
owned_b = remote frees batched per destination (one CAS per 32).
Findings: global structures anti-scale (mutex and lock-free stack get slower with threads). Owner arenas scale ~linearly
with no remote frees (4.7 Gpairs/s at 16T, 4x glibc). A remote free costs ~23 ns/pair (owned_b, 2T) vs ~2 ns local:
~10x, mostly cache-line movement, so batching helps only +30-45%. owned_b is still ~2.4x glibc at 100% remote.
RSS stayed at a few MB in every run (flow-controlled handoff), so memory growth under remote frees was not stressed.

## 3. Reference counts (`refcount_bench`): ns per inc+dec pair per thread
| scenario | impl | 1T | 2T | 4T | 8T | 16T |
|---|---|---|---|---|---|---|
| thread-private | plain | 0.75 | 0.75 | 0.75 | 0.77 | 0.81 |
| thread-private | atomic | 7.24 | 7.25 | 7.26 | 7.29 | 7.90 |
| thread-private | biased | 0.46 | 0.47 | 0.47 | 0.50 | 0.68 |
| 1% shared | atomic | 7.24 | 7.58 | 8.14 | 7.97 | 8.99 |
| 1% shared | biased | 0.58 | 1.50 | 1.74 | 2.26 | 2.67 |
| 10% shared | atomic | 7.24 | 10.1 | 12.1 | 14.7 | 23.0 |
| 10% shared | biased | 0.58 | 4.64 | 8.28 | 11.3 | 22.4 |
| 50% shared | atomic | 7.24 | 15.9 | 25.9 | 32.7 | 75.3 |
| 50% shared | biased | 0.58 | 16.7 | 42.2 | 75.0 | 146.8 |
| one hot object | atomic | 7.24 | 19.4 | 40.8 | 84.4 | 233 |
| one hot object | biased | 0.44 | 5.78 | 21.2 | 69.6 | 274 |
Findings: for private objects biased is ~16x cheaper than atomic and equal to plain. The advantage shrinks with sharing,
disappears near 10% at 16 threads and reverses at 50% (owner's local count and the shared count sit on one line).
One object touched by many threads costs 233-274 ns per pair at 16 threads under either scheme.
Caveat: biased numbers are fast paths only; the owner-reaches-zero merge/queue protocol is not implemented or measured.

## 4. Design implications
1. Per-thread owner arenas with remote free lists (batched) are the only allocator structure that scales; global mutex or
   lock-free free lists are unusable beyond one thread. A cross-thread last drop costs ~10x a local one.
2. Promotion must be one CAS on {full generation, strong count}; a separate epoch check races with decommit.
3. Biased refcounts pay off only when sharing is rare (<~5%). A hot shared object (the doc's shared cache) costs 233+ ns
   per Ref copy at 16 threads regardless, so the compiler must elide count traffic on hot paths (borrow instead of clone;
   the doc's `cache_get(cache: Ref<Cache>, ...)` by-value signature would pay it on every call).
4. For hot shared read-mostly data, avoid per-access counting altogether: borrowed access inside sync, or immutable
   snapshots acquired once per request.

## 5. Biased reference counting, full lifecycle (`c/brc.c`; data `brc.txt`, `brc_plain.txt`)
Implemented the complete protocol: owner-only local count + atomic shared word (count<<2 | Q,M flags); owner merge when
its local count reaches zero; negative-count detection by non-owners that queues a merge request on the home thread;
queued merge adds local to shared and publishes MERGED; the last decrement after MERGED frees. Message passing is
"sender increfs once per receiver, receiver decrefs once" (so receivers hit the negative/queue path), with forwarding
hops; a publish/read pattern (owner publishes into a mutex-guarded cell, readers incref under the lock) forces the
owner to merge while readers still hold references.

Correctness (check mode never frees memory, so use-after-free and double free are detectable):
- 18 stress runs at 8-16 threads (up to 25.6M messages, forwarding up to 3 hops, mixed with publish/read):
  0 leaks, 0 double frees, 0 use-after-free.
- Coverage counters: queued merge requests up to 1.59M per run, queue-merges with live references up to 813k,
  owner-merges with live references up to 445k (an earlier version of the workload hit that last path once in 800k
  objects, which is why it was reworked).
- Negative controls: dropping merge requests leaked 787,875 of 800,000 objects; treating count 0 as dead without checking
  MERGED produced 3.4M double frees and 12.3M use-after-free. (A third control I first wrote, free-while-queued, turned
  out to be unreachable in valid executions, i.e. Q and M never coexist, so it was discarded.)

Throughput, Mobj/s total, whole object lifecycle (malloc + 8 or 64 private inc/dec pairs + sharing + free):
| scenario | impl | 1T | 2T | 4T | 8T | 16T |
|---|---|---|---|---|---|---|
| private, 8 pairs | atomic | 12.1 | 18.9 | 38.8 | 70.1 | 90.2 |
| private, 8 pairs | biased | 39.5 | 46.6 | 81.9 | 146 | 150 |
| private, 8 pairs | plain | 54.9 | 57.7 | 104 | 188 | 238 |
| private, 64 pairs | atomic | 2.1 | 4.1 | 8.0 | 15.9 | 28.8 |
| private, 64 pairs | biased | 14.1 | 23.3 | 41.9 | 66.1 | 87.3 |
| private, 64 pairs | plain | 34.4 | 42.3 | 69.9 | 115 | 168 |
| messages to 1 receiver | biased / atomic | 2.66x | 1.21x | 0.97x | 0.82x | 0.71x |
| messages to 4 receivers | biased / atomic | 2.07x | 1.07x | 0.96x | 0.95x | 0.87x |
| publish / read | biased / atomic | 1.01x | 1.02x | 1.03x | 1.03x | 0.99x |

Findings:
1. Biased counting is correct as implemented and 2-7x cheaper than atomic for thread-private objects, but the lifecycle
   gain is far smaller than the 16x fast-path number because allocation, free and the rest dominate.
2. Once objects cross threads regularly it is equal to or worse than plain atomic (0.71-0.87x at 16 threads with message
   passing): the queue, the negative-count path and the CAS loops cost more than the atomics they save.
3. Where a type can be proven thread-confined, plain counts beat biased by 1.25-1.6x (8 pairs) and 1.7-2.4x (64 pairs).
4. Recommendation: do not build biased RC for v1. Use the type-level classification: confined types get plain counts,
   shared types get plain atomic counts. Biased only helps for "usually private, occasionally shared" objects, at the price
   of per-object owner state, home-thread merge queues and the owner-merge protocol.
Caveats: x86 only; ring buffer, queue and workload are my own; owner-thread exit with queued objects not handled (fibers
are pinned to long-lived workers); malloc/free and the inbox ring are common costs that dilute differences.

## 6. Messaging decision (design note)

- Shared memory (`Ref`/`WeakRef` across threads, `sync`/`mutate`) is the primary sharing model. The messaging workloads in `brc.c` and `conc_alloc.c` were benchmark constructs, not design requirements: read the brc messaging losses (0.71x at 16T) as the cost of that artificial pattern, not of shared-memory programs (publish/read was ~1.0x).
- Queues stay for passing data between threads, and a send can target a specific vthread: each vthread has a typed mailbox, addressed by a generational vthread handle (same idx+gen/epoch scheme as `WeakRef`). Replies are sends to the caller's handle.
- Moves through a mailbox transfer ownership, so the payload is confined to the receiver and keeps plain counts. This is what keeps confinement inference useful.
- Remote frees are produced by refcount drops on any thread, not by queues, so the conc_alloc results still apply.
- Open items: lost-wakeup protocol (running/parked/notified), send to an exited vthread (handle check), bounded mailbox and `send` as a parking call (only `try_send` allowed inside `sync`/`mutate`), draining a dead vthread's mailbox.
- Also needed: a generic channel (`Channel<T>`, put where any consumer may take). Consumers park on a wait list, a put wakes one waiter. A single shared MPMC queue is expected to anti-scale (cf. the global lock-free stack in conc_alloc), so the candidate is sharded sub-queues with local-first take and stealing. Mailbox (one consumer, addressed) and channel (any consumer) share the wake protocol.
