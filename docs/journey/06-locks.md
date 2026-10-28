# 6. Locks that park

## The rule

Entering `sync` or `mutate` may park the virtual thread. A holder never parks inside the block (a compile error), so two vthreads on one worker never contend and hold time is bounded by the code in the block.

Several objects in one head, as in `mutate(a, b)`, are acquired all-or-nothing in a fixed global order; if one is busy the acquirer releases everything, parks on that lock's FIFO queue and retries, keeping its arrival ticket. Nested acquisition of another object, reentrancy and read-to-write upgrade are errors.

## The ordering key

The order is by the address of each object's lock word, or `(arena id, slot index)` for arena objects whose storage can move. A per-allocation sequence number was rejected for its cost. Arenas never compact, and a chunk is freed only when empty, so a live object's address never changes. Only types that appear in a `sync` or `mutate` head carry a lock word.

## The measurement that changed the contract's implementation

With FIFO fairness, one waiting writer makes every later reader queue behind it, and each handoff is a cross-thread wake. Throughput at 4 workers, one lock, 64 connections:

| GET share | park only | spin then park |
|---|---|---|
| 100% | 1.29M | 1.29M |
| 90% | 570k | 1.28M |
| 50% | 330k | 1.27M |
| 0% | 318k | 1.27M |

A 10% write mix lost 55% of throughput. Spinning 2000 rounds before parking removed the loss. The contract is unchanged (entering still "may park"); only the cost differs. A 100-round spin was noisy, from 1.0M to 326k at 50% reads.

The park-only build remains in the test suite so the handoff path stays covered.

!!! warning "Not yet implemented"
    The multi-object retry exists only as design. mini-redis has a single lock.
