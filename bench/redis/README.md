# mini-redis in Strata

Second proving ground, after the HTTP server (`../http/`, `stra/http/`). Source being converted: `tokio-rs/mini-redis` (MIT), unmodified copy in `orig/src/`
(cloned from GitHub, `.git` removed). Only the server side is converted: `frame, parse, connection, db, server, shutdown, cmd/*`, about 2,400
lines with comments. The client, CLI, examples and tests are not converted; `orig/src/tests/server.rs` is the behavioural reference.

## What it tests that the HTTP server did not

| mini-redis | Strata feature |
|---|---|
| `Frame` (Simple, Error, Integer, Bulk, Null, Array) | tagged enum, `switch` arms |
| `Command` (Get, Set, Publish, Subscribe, ...) | tagged enum, one arm per command |
| `Option<T>` (get miss, expire) | `T?` |
| `Result` + `ParseError::EndOfStream` | `throws` lists, catch of one exception kind |
| `Db` behind `Mutex`, cloned `Arc` | shared state, `sync`/`mutate`, atomic count (shared across workers) |
| expiry map + `purge_expired_tasks` + `Notify` | cancellable timer, a background vthread woken by `set` |
| `broadcast::channel(1024)` per topic, `Lagged` | channel with a stated capacity and an explicit lag policy (see below) |
| `select!` in `Subscribe::apply` over N streams + socket + shutdown | `waitAny` over mailboxes, socket and a shutdown source |
| `Shutdown` + `mpsc` completion + `Semaphore(250)` | structured scope: leaving it waits for children; connection limit |

## Layout (the rule from `strata-code-layout`)

```
bench/redis/
  README.md
  orig/src/            upstream, untouched
  tests/               protocol tests, pub/sub tests, expiry tests, limit tests (port 18080 is NOT used here; use 16379)
  loadgen_resp.c       RESP load generator (GET/SET/PING/PUBLISH, pipelining)
  build.sh  run_tests.sh  run_bench.sh  run_bench_lock.sh
stra/redis/            the library: frame.stra, command.stra, db.stra, connection.stra, reply.stra, limits.stra, types.stra
stra/redis/server.stra the server only: listener, per-connection loop, subscribe loop, shutdown, main
stra/runtime/sync.stra NEW runtime support (declarations); everything else is reused from stra/runtime/ and c/runtime/
c/runtime/rt_sync.h    NEW runtime support in C (lock, wake queue); the rest is reused from the HTTP server
c/generated/redis/     the C implementation of the same: server.c and its headers
```

New runtime support needed (everything else already exists in `stra/runtime/` and `c/runtime/`):
1. **Channel / mailbox with a stated capacity**, one producer side per publisher, many consumers (`runtime/rt_chan.h`, `chan.stra`).
2. **Shutdown source**: a wait source for `waitAny` that becomes ready for every connection when the server is told to stop.
3. **Cross-worker wakeup**: a publish on worker A must wake a subscriber parked on worker B. The HTTP server never needed this.
4. **Shared database with a lock** usable from all workers (`sync`); with per-worker or sharded variants as an experiment.

## Places where the original has unbounded input or hidden behaviour

These are decided deliberately in the conversion (principles 10 and 11), not copied:

- `Frame::parse` does `Vec::with_capacity(len)` with `len` read from the wire, and bulk lengths are unchecked, so one connection can ask for
  gigabytes. Convert with declared limits: `maxArgs`, `maxBulk`, `maxFrame` (all in `redis/limits.stra`), answered with a protocol error.
- The read buffer grows without bound. Bounded by `maxFrame`.
- `check()` walks the frame, then `parse()` walks it again; the conversion parses once into offsets (like the HTTP `Request`), copying only what the database keeps.
- The database has no size limit: add `maxKeys` (values are bounded by `maxBulk`).
- Pub/sub: `broadcast::channel(1024)` silently drops the oldest messages for a slow subscriber (`Lagged(_) => {}`). That is hidden loss.
  Conversion options: disconnect the subscriber with an error, or deliver a visible "lagged N" notice. Decide and document.
- `Handler::run` ends the connection on any parse error, with no reply. Convert as an error answer (`-ERR ...`) plus close.
- `unimplemented!()` in `Frame::parse` is unreachable only because `check()` ran first; the conversion has no such split.
- Connection limit 250 is a `Semaphore`; the conversion states it as a limit that refuses (accept, answer `-ERR max clients`, close) rather than parks the accept loop.

## Plan

1. Reference run of the original (build, a load generator for RESP, baseline numbers). **Blocked: see below.**
2. `redis/*.stra`: frame, parse, commands, db.
3. `runtime/`: channel, shutdown source, cross-worker wake.
4. `server.stra` + `server.c`; tests ported from `orig/src/tests/server.rs`.
5. Compare with the original Rust (and a Go version if wanted): throughput at 1 core and 4, memory per connection, pub/sub fan-out latency, expiry accuracy.

## Third-party crates and what replaces them

The original needs `tokio, bytes, atoi, clap, tracing, async-stream, tokio-stream`; Strata has no equivalents of those, so nothing here may depend on them.

| Crate | Used for | In this conversion |
|---|---|---|
| tokio | scheduler, sockets, timers, `select!`, broadcast/mpsc/Semaphore/Notify | the runtime: `stra/runtime/leaves.stra` + new `runtime/sync.stra` (Signal, Flag, Mailbox, Limiter, `Wakeup.at`) |
| bytes | refcounted immutable buffers (`Bytes`) | `Ref<Value>`, atomic count because values cross workers |
| atoi | integer parsing | `parseDecimal` in `redis/frame.stra` |
| async-stream, tokio-stream | `StreamMap` over subscriptions | not needed: one mailbox per connection, `waitAny` |
| tracing | logging | not converted; `log(e)` stub |
| clap | CLI | not converted; options read in `main` |

Standard-library pieces the original also uses and the Strata files ASSUME (as in `../http/README.md`): `Map<K,V>` (hash map keyed by bytes),
`OrderedSet<T>` (sorted, first/remove), `Bytes` (owned byte string), `Instant`, `Duration`, `equalsIgnoreCase`, `isUtf8`, `toString`, `copy`, `find`,
`decimalText`, `min`, `assert`, `log`, `onSignal`, `spawnWorkers`, `Limits.fromArgs`, `args.getInt`. `server.c` has to write
its own hash table and expiry heap, which gives an honest cost for what a Strata standard library must supply.

## Status

Written (Strata, not compiled): `runtime/sync.stra`, `redis/{limits,types,frame,reply,connection,command,db}.stra`, `server.stra`.
Behaviour differences from the original, all deliberate and listed above, plus: one mailbox per subscriber connection (not per channel), so
lag is per connection; nested arrays and `-` elements inside a command are refused at scan time; `UNSUBSCRIBE` with no names drops all;
no lazy expiry on `get` (as the original: an expired key is visible until the purge vthread runs).
Next: `runtime/rt_sync.h` (Signal, Flag, Mailbox with cross-worker wake, Limiter, Wakeup.at), `server.c` with its own hash table and expiry heap,
`loadgen_resp.c`, tests ported from `orig/src/tests/server.rs`.

## Locks (decided with the user)

`sync`/`mutate` PARK while waiting (design doc 7.1), and multi-object heads (`mutate(a, b)`, `sync(b, a)`) cannot deadlock against each other:
all-or-nothing acquisition in a fixed order, release and park on the blocker's queue, retry with the original ticket. Consequences applied here:
every Db method that locks may park; `cleanup()` cannot lock, so `Subscriptions.cleanup` pushes to `Db.reaper` and the purge vthread unsubscribes;
`purgeExpired` removes at most `PURGE_BATCH` keys per hold. Nothing in mini-redis needs a multi-object block yet (one lock around `Store`);
a sharded `Store` (key hash -> shard) would, for nothing else but MULTI-key commands, which the original does not have.

## C implementation (done) and tests

```
runtime/rt_sync.h    cross-worker wake queue (eventfd per worker), Mailbox, Bell, parking RwLock (FIFO handoff), Limiter
redis/db.h           Value (atomic refcount), hash table, expiry heap, channel table, reaper queue; tables sized once from the limits
redis/resp.h         resumable request scanner, reply writer (one pooled unit; a big bulk is sent from where it is)
redis/command.h      the commands as borrowed views into the input buffer
server.c             accept hook, connection state machine (commandLoop / subscribeLoop), purge task, graceful shutdown, main
build.sh [san]       server, 4 negative-control builds; `san` adds server_asan (address+undefined) and server_tsan
run_tests.sh         27 tests + 4 negative controls (SERVER=./server_asan or ./server_tsan runs the same suite under a sanitizer)
tests/redis_test.py  RESP client and the tests
```
Port 16379 (never 8080). Options: `port workers cpu0 maxconns maxframe maxbulk maxkey maxkeys maxtopics maxsubs subqueue maxexpire lag=disconnect|notice linger stats`.
Results: 27/27 on `server`, `server_asan` (no errors, no leaks) and `server_tsan` (no reports); the four controls (no yield, no reaper hand-off,
no limits, no wake) fail as intended; under `stress`/`churn` the contended lock path runs (about 100 handoffs) and every channel is released.
Found by the tests: a pipelining publisher on one worker flooded the subscribers' queues, because the command loop never yielded (now PER_TURN = 64
commands per turn, in server.stra too); a wake request for a worker that had not finished starting wrote to fd 0 (now dropped until its eventfd exists).
Not done: loadgen and numbers, Rust/Go comparators, Strata never compiled, no incremental table resize (tables are sized for the limits instead).

## Measurements
`bench/results/miniredis.md` (commands, the lock, pub/sub, idle memory). Headlines: 328k ops/s per worker, 1.0-1.3M on four; a 10% write mix with a park-only lock loses 55% of
throughput, and a bounded spin before parking (now the default, `LOCK_SPIN=2000`) removes it; one worker delivers ~450k pub/sub messages/s (one `sendmsg` each);
297 B per idle connection. No baseline against the original Rust server (blocked) or Go.
Scripts: `run_bench.sh`, `run_bench_lock.sh`; the load generator is `loadgen_resp.c` (modes getset, pubsub, idle, idlesub; `rate=` paces publishers).
