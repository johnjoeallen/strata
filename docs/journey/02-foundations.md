# 2. Measuring the foundations

Before writing any Strata, the expensive questions got small C benchmarks, compared with Rust (Tokio, `slab`) and Java virtual threads where one existed. Full tables: [`group1.md`](https://github.com/johnjoeallen/strata/blob/main/bench/results/group1.md), [`group2.md`](https://github.com/johnjoeallen/strata/blob/main/bench/results/group2.md), [`group56.md`](https://github.com/johnjoeallen/strata/blob/main/bench/results/group56.md), [`lazy.md`](https://github.com/johnjoeallen/strata/blob/main/bench/results/lazy.md).

## Fibers and stacks

| Unit (100k idle, 1 KB state) | Resident bytes each |
|---|---|
| Tokio task | 1,419 (395 with no state) |
| Strata pooled-stack fiber | 4,113 (one page floor) |
| Java virtual thread | about 809 heap with no state |

- A stackful fiber has a 4 KB floor; a stackless task is about 10x smaller when idle state is small. The gap closes near page size.
- `MADV_NOHUGEPAGE` is mandatory with transparent huge pages on: without it a 64 KB stack costs 65 KB resident, and a 1 MB stack almost 1 MB.
- Per-stack guard pages do not scale: each costs two mappings, and 1M fibers hit the map-count limit.
- Switching is cheap: about 13 ns with 1k fibers, 35 to 39 ns with 100k.

## Typed arenas

- Layout (one reservation, a chunk table, or `munmap`) makes no measurable difference to allocation cost; choose by what handles need.
- Smaller chunks reclaim better, and geometric growth removes the small-type floor.
- Against glibc, after freeing everything an arena returns memory (0.3 MB retained against 153 MB).
- `MADV_DONTNEED` zero-fills, so a generation stored in a slot is lost when a chunk is decommitted. The fix is a per-chunk epoch kept in the resident chunk header.
- Long-lived objects scattered through shared chunks pin memory. That is solved by lifetime segregation, which per-type arenas give by construction.

## Reference counts across threads

- Biased counting is 2 to 7x cheaper than atomic for thread-private objects, but once objects cross threads it is equal or worse (0.71 to 0.87x at 16 threads with message passing).
- **Decision:** no biased counting in v1. Types proven thread-confined get plain counts; shared types get plain atomic counts.
- Messaging stays for passing data, with typed per-vthread mailboxes. Shared memory with `sync` and `mutate` is the primary sharing model.

## Frames for parked calls

Four strategies were compared: plain calls, a heap frame per call (eager), frames embedded in the caller, and lazy frames created only while unwinding a real park.

- Lazy beats eager by 3.6 ns per request at depth 4 and 7.7 ns at depth 8 when parks are rare. The whole difference between strategies is about 1 ns per parking call.
- An earlier estimate of 20 to 30 ns saved per request was too high; the measured saving is 4 to 8 ns.
- Parked memory is the same for lazy and eager.
