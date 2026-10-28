# mini-redis in Strata/C: measurements

Server: `bench/redis/server` (built from `c/generated/redis/server.c`, the C a Strata compiler would emit for `stra/redis/server.stra`), cores 0..N-1. Load generator: `loadgen_resp`, cores 8.., same machine, loopback
(Ryzen 9 9950X3D, Linux 6.12, gcc 14.2). Raw output: `miniredis_c.txt`, `miniredis_c_lock.txt`; scripts `run_bench.sh`, `run_bench_lock.sh`. 4 s per point after 0.5 s warm-up,
one run per point unless noted. **There is no comparison with the original Rust server: building it was blocked (third-party crates), and the user chose to skip the baseline.**
Numbers are therefore absolute, not relative; the closest reference is the HTTP server's sequential C path (301-310k req/s, 1 core).

## Commands (90% GET / 10% SET, 16-byte values, 10,000 keys; every command is answered with its own send)

| workers | connections | pipeline | ops/s | batch latency p50 / p99 |
|---|---|---|---|---|
| 1 | 16 | 1 | 328k | 48 / 53 us |
| 1 | 16 | 16 | 468k | 548 / 765 us (16 commands) |
| 1 | 64 | 1 | 335k | 189 / 206 us |
| 4 | 16 | 1 | 1.10M | 13 / 23 us |
| 4 | 16 | 16 | 1.94M | 116 / 267 us |
| 4 | 64 | 1 | 1.29M | 46 / 69 us |

Values (1 worker, 16 connections, pipe 1): 16 B 327k, 256 B 329k, 4 KB 292k, 64 KB 92k ops/s (about 6 GB/s of value traffic).
The 4-worker rows were re-measured after the lock changed to spin-then-park (first measured with the park-only lock: 1.00M, 1.26M and 563k; the 64-connection row is the one the lock hurt). Four load-generator threads top out near 1.3M ops/s at pipe 1, so those rows may be client-bound.

## The lock (4 workers, 64 connections, 1000 keys; ops/s)

| GET share | park only | spin 100 rounds, then park | spin 2000 rounds, then park (default) |
|---|---|---|---|
| 100% (shared lock) | 1.29M | 1.25M | 1.29M |
| 90% | **570k** | 1.28M | 1.28M |
| 50% | **330k** | 1.00M (noisy: 326k in the earlier run) | 1.27M |
| 0% (every command takes the exclusive lock) | **318k** | 1.27M | 1.27M |

With a pure park nearly every acquisition parks (2.5M parks in a 4.5 s run at 90% GET): FIFO fairness makes every reader queue behind a waiting writer, and each handoff
costs a cross-thread wake (eventfd + malloc). The cost is paid even though hold times are about 100 ns, because the body cannot park. A bounded spin before parking removes it:
2000 rounds of 8 `pause` instructions, still entering "may park" as far as the language is concerned. The park path stays covered: `server_park` and its sanitizer builds run the full suite.
The generator is the ceiling near 1.29M, so the spin results are lower bounds. A sharded store would remove the single lock; not built.

## Pub/sub

Saturated (4 closed-loop publishers, 64-byte messages, 1 channel; queue 65,536): publishes/s and deliveries/s

| workers | subscribers | publishes/s | deliveries/s |
|---|---|---|---|
| 1 | 1 | 207k | 207k |
| 1 | 16 | 27k | 439k |
| 1 | 64 | 7.1k | 456k |
| 1 | 256 | 1.7k | 444k |
| 4 | 1 | 406k | 406k |
| 4 | 16 | 203k | 1.08M (11 subscribers were disconnected for lag) |
| 4 | 64 | 21k | 1.28M |
| 4 | 256 | 7.7k | 1.98M |

A single worker delivers about 440-456k messages/s whatever the fan-out: each delivery is one `sendmsg`, so the cost is per message, not per publish. Four workers deliver up to 2M/s.
Delivery latency at saturation is queue-fill time and is not reported. At saturation the lag policy fires as designed (the subscriber is disconnected with an error).

Delivery latency at a fixed load (paced publishers; rate = min(10,000, 200,000/subscribers) publishes/s, so deliveries stay at or below 200k/s), p50 / p99 in us:

| subscribers | 1 worker | 4 workers |
|---|---|---|
| 1 | 12 / 15 | 17 / 23 |
| 16 | 29 / 47 | 34 / 71 |
| 64 | 84 / 153 | 104 / 304 |
| 256 | 310 / 597 | 399 / 846 |
| 1000 | 1,414 / 2,558 | 1,410 / 2,596 |

More workers do not lower the latency at this load; the delivery work is spread over more threads, but each message still costs a send and a cross-worker wake.

## Memory per idle connection (100,000 connections, 4 workers, RSS of the server only; kernel socket memory is not included)

| | per connection |
|---|---|
| connected, idle (after one PING) | 297 B (`ConnTask` is 296 B; no buffer is held while idle) |
| idle subscriber, queue limit 1024 | 594 B |
| idle subscriber, queue limit 16 | 354 B |

The mailbox ring grows on demand (8 slots, doubling to the declared limit), so the limit does not cost memory until a subscriber falls behind. Before that change each idle
subscriber cost 4.5 KB with the default limit: the ring was allocated at full size. For comparison the HTTP server's sequential connection measured 89 B and its pipelined one 192 B
(different object, same pools).

## Bugs these runs found (all fixed)
- A pipelining publisher on one worker overflowed its subscribers' queues: the command loop never yielded (now 64 commands per turn).
- The load generator crashed when the server disconnected a lagging subscriber, and ran with the server's default `maxconns=250` at 256 subscribers; the idle-memory runs
  collided on TIME_WAIT source ports. All three were harness mistakes, found because the numbers looked wrong.

## Not measured
The original Rust server and a Go equivalent (no baseline), latency under overload for GET/SET, key expiry accuracy, a sharded store, more than 100,000 subscribers, a real network.
