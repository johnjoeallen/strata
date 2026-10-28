# HTTP server: C-as-the-compiler-would-emit vs Tokio

Code: `c/http/{rt.h,server.c,server.stra,loadgen.c,run_scale.sh}`, `rust/src/bin/http_server.rs`. Raw data: `http_scale.txt`.

## What was built
* `server.c` is hand-written to look like compiler output for `server.stra` (blocking-style `handle`/`serve`):
  one frame struct + step function per parking function, callee frames embedded/overlaid, parking = `return PARKED`,
  one 88-byte `ConnTask` per connection from a per-type arena, 2 KB request buffer taken from a pool only while a
  request is in flight. Thread-per-core: each worker has its own epoll, run queue, SO_REUSEPORT listener, arenas.
  Edge-triggered epoll; a task always tries the syscall first and parks only on EAGAIN.
* HTTP/1.1 subset: GET /, keep-alive (1.1 default, 1.0 with header), `Connection: close`, pipelining, 404/400.
  Verified with curl, nc (pipelined + close), and `ab -k` (200k requests, 0 failures).
* `loadgen.c`: N connections (one request each, then idle), A of them closed-loop for the measurement window.
* Tokio comparator, same protocol: `sched=pc` (one current-thread runtime + SO_REUSEPORT listener per core, closest
  to server.c) or `sched=mt` (one listener, work-stealing); `buf=inline` (2 KB array in the task) or `buf=lazy`
  (no buffer held across an await).

## Results (4 server workers on cores 0-3, 4 client threads on cores 8-11, loopback, 64 active connections)
| conns | server | user-space/conn | rps | p50 us | p99 us |
|---|---|---|---|---|---|
| 10k   | strata-c | 94 B  | 1.25M | 42 | 98 |
| 100k  | strata-c | 89 B  | 1.29M | 40 | 88 |
| 200k  | strata-c | 88 B  | 1.30M | 39 | 83 |
| 10k   | tokio pc lazy | 904 B | 1.26M | 47 | 72 |
| 100k  | tokio pc lazy | 897 B | 1.25M | 53 | 78 |
| 200k  | tokio pc lazy | 896 B | 1.25M | 49 | 78 |
| 100k  | tokio pc inline | 3,074 B | 1.25M | 47 | 77 |
| 100k  | tokio mt inline | 3,073 B | 1.01M | 62 | 109 |

Kernel memory is ~3-3.6 KB per socket for every server (not a differentiator; ~40x the C server's user-space cost).
Server-bound (1 worker, saturated): strata-c 318k rps vs tokio 296-301k (5-7% more). 2 workers: 639k vs 589-601k.

## Reading
* Per-connection user-space memory: 88 B (= sizeof ConnTask, nothing else retained) vs ~900 B for a Tokio task that
  holds no buffer while idle, vs ~3 KB for the naive Tokio handler. Task future sizes: 264 B lazy, 2,368 B inline.
* Throughput is nearly the same: the workload is syscall-bound (~3.1 us per request at one worker, recv+send+epoll).
  The frame model buys memory, not a large throughput win, on this workload.
* Idle connections at 200k cost no throughput or latency in either server.

## Caveats
* The C is hand-written to mimic compiler output; real generated code may differ (more state, less tuned).
* Loopback only; the client and server share a machine. 4-worker numbers are client-limited (~1.25-1.30M); the
  1/2-worker runs are the ones that isolate the server.
* Ceiling ~250k connections on this box: `nf_conntrack_max` = 262144 (Docker/NAT rules loaded; no root to raise it).
  A 500k attempt stalled at ~262k for every server. Per-process fd hard limit is 524288.
* No timers/timeouts, no cross-worker wakeups, no TLS, one tiny route, responses are static. Arena memory is not
  returned to the OS (task_arena only grows). Only the mailbox/channel designs are untested here.
* The first Tokio "lazy" numbers were wrong (buffer held across an await via stale readiness); fixed and rerun above.
  Tokio could still be tuned further (e.g. a custom allocator); I did not.

## Known limitation: request size (added after review) - superseded for headers by "Size-class input buffers" below
* The 2 KB request buffer is unrealistic. Real header blocks are commonly 8 KB+ (cookies, long URIs) and POST/PUT
  bodies can be megabytes. server.c answers anything over 2 KB with a plain 400 and closes (should be 414/431),
  and the Tokio comparator has the same cap, so the comparison is consistent but both are optimistic.
* The idle result (88 B/conn) is unaffected: an idle connection holds no buffer. In-flight memory is understated:
  each connection that is mid-request holds a buffer (100k partial requests x 8 KB = 800 MB; slowloris does this
  deliberately). Large headers and bodies are not measured.
* Planned shape: size-class buffer pools (start 512 B-1 KB, grow to 4/8/16 KB only when the header block does not
  fit; 414/431 beyond the limit); bodies streamed into a caller-supplied buffer after the header buffer is released;
  a `T&` into a buffer must not dangle when the buffer grows and moves its bytes. The design only says a `T&` cannot outlive its referent (strata-design.md:40) and leaves growable-array storage TBD (line 44, open question 4), so this is an open design question: non-moving storage, growth as a lifetime-ending effect, offsets/handles instead of pointers, or keeping the old block alive.

## Reader change: no move per request (added after review)
`handle` used to move the leftover pipelined bytes to the front of the buffer after every request. That was written
by hand in the .stra handler and was a hidden cost. Now:
* `server.stra`: a library `Reader` type owns the buffer and the unconsumed input `buf[start..end]`; the handler is
  `input.next_request()` / `route` / `conn.write_all` and never touches a buffer, length or move. `Request` holds
  offsets, resolved through the Reader.
* `server.c`: `Handle` keeps `start`/`end`; a request is consumed by advancing `start`; bytes are compacted only when
  the tail is full and more input is needed. `-DEAGER_MOVE` rebuilds the old behaviour for comparison, `-DBUFSZ=n`
  sets the buffer size. sizeof(Handle) is still 56 B, ConnTask 88 B.
* Bug found on the way: the arena's first chunk (4 KB) was smaller than a 16 KB object, so the first buffer overran
  its chunk (segfault with BUFSZ=16384). Fixed (chunk >= one object). Earlier results are unaffected: 88 B tasks and
  2 KB buffers always fit in a 4 KB chunk.

Correctness: 5,000 pipelined requests per run sent in random 1-3000 byte fragments (requests split mid-line, buffer
forced to compact), 4 seeds x {lazy, eager, 16k}: all 5,000 responses plus the final 404, every time. Negative
controls (compaction without resetting `start`; compaction without the copy) fail: connection reset, or 34 responses
lost.

Pipelined worst case (K tiny 18 B requests written at once, 1 server worker saturated, 16 connections; requests/s,
3 runs; raw data in `http_pipeline.txt`):
| buffer | K | eager move | lazy compaction | lazy gain |
|---|---|---|---|---|
| 2 KB  | 100  | 462-471k | 469-473k | +0.3-1.5% |
| 16 KB | 800  | 465-467k | 468-473k | +0.2-1.8% |
| 64 KB | 3500 | 416-417k | 472-475k | +13% |
| 16 KB | 1 (no pipelining) | 308k | 306-309k | none |
The old per-request move is quadratic in buffer size but small while the buffer fits in L1 (16 KB: under 2%); it
becomes visible when the buffer spills the cache (64 KB: 13%). Per-request cost is dominated by the send() syscall
(one per response, because the benchmark handler is instant). Batching responses by waiting for later requests is not
possible in general: HTTP/1.1 responses are ordered and real handlers take time, so response 1 cannot wait for
requests 2..n. Only opportunistic batching adds no latency: coalesce responses that are already complete into one
send/writev, and flush before any park, when no complete request remains buffered, or when the output buffer is full.
Anything timer-based (Nagle/cork with a timeout) is bounded-time batching and trades latency for throughput. The
absolute numbers here therefore apply only to instant handlers. The lazy version removes the cliff and has no cost for
non-pipelined traffic.

## Opt-in response batching (library level, off by default; `-DBATCH_N=16`)
Holds finished responses only while the next request is already fully buffered; flushes before any read park, when no
complete request remains, or when the output buffer is full. Never waits on a timer. Raw data: `http_batch.txt`.
Handler work is a spin loop (~0.8 ns/iteration, calibrated from unpipelined runs: 500 it = 0.4 us, 2000 = 1.6 us,
8000 = 6.3 us). K=16 pipelined requests per write, 1 saturated worker, 16 connections, requests/s:
| handler time | no batching | batching | gain |
|---|---|---|---|
| ~0     | 460k | 4.10M | 8.9x |
| 0.4 us | 367k | 1.62M | 4.4x |
| 1.6 us | 252k | 574k  | 2.3x |
| 6.3 us | 120k | 160k  | 1.35x |
Cost: one connection, unloaded, K=16 (mean time until the first response arrives; time until all 16 arrive):
| handler time | no batching | batching |
|---|---|---|
| ~0     | 10 us; 38 us  | 10 us; 10 us |
| 0.4 us | 9 us; 47 us   | 16 us; 15 us |
| 1.6 us | 9 us; 68 us   | 34 us; 32 us |
| 6.3 us | 14 us; 137 us | 107 us; 103 us |
The first response is delayed by about (K-1) x handler time, as expected, while the last response arrives sooner. Under
saturation the "first response" figure reflects queueing and looks better with batching; that is a throughput effect,
not a latency improvement. Maximum latencies in the batched runs show unexplained multi-ms spikes (not investigated).
Held responses live in a pool buffer that exists only while input is buffered and is never held across a read park.
Conclusion: batching helps a lot only for very cheap handlers, and the delay is proportional to handler time, so it
must be opt-in with a stated bound (design principle 10), not something the runtime does silently.

### Flushing held responses when the connection ends
Held responses must go out on every exit path, not just the normal one. `finish:` used to free the held-response buffer
without sending it (unreachable with this server's handler, but any early exit between "hold" and "flush" would have
lost responses silently). Now `finish:` makes one best-effort non-blocking `send` of whatever is held (cleanup never
parks). Test: 5 requests pipelined in one write, handler failure injected at request 3 (`-DINJECT_FAIL_AT=3`):
responses delivered = 2 with the close-time flush, 0 without it (`-DNO_CLOSE_FLUSH`, negative control). In Strata the
mechanism is `Drop` on the batching writer (design section 9, new rule: cleanup never parks; an explicit `flush()` is how
code waits). Batching is also only allowed for handlers whose may-park effect is "cannot park": a parking handler
would otherwise hold earlier responses for as long as it waits.

### Max hold time, and what actually protects a batch from a slow request
Added to the opt-in batching: `hold_us=n` (the oldest held response is never held longer than n us, checked before each
handler and when deciding to hold another response) and a declared handler class (`classify=1`: GET /slow is declared
unbounded, so held responses are sent before it runs). Raw data: `http_batch.txt`.

One slow request (~1 ms handler) at position 9 of 16 pipelined, one unloaded connection, time to the first response:
| configuration | first response (mean) |
|---|---|
| no batching | 13 us |
| batching, no hold limit, slow undeclared | 955 us |
| batching, hold_us=50, slow undeclared | 921 us (no help) |
| batching, slow declared unbounded | 10 us |
| batching, hold_us=50, slow declared unbounded | 10 us |
A hold time cannot bound a single long handler: the library checks the clock only between handlers, and a handler that
has started cannot be interrupted (no preemption in compiled state machines). What protects the batch is knowing,
before the handler runs, that it is not cheap.

Where the hold time does help: every handler ~6.3 us, K=16, first response (unloaded) / throughput (16 conns):
| config | first response | requests/s |
|---|---|---|
| no batching | 14 us | 123k |
| batching, no limit | 108 us | 160k |
| batching, hold_us=30 | 49 us | 152k |
| batching, hold_us=100 | 106 us (batch fills first) | 159k |
The bound is roughly min(max_hold, (max_batch-1) x handler time) plus one handler. The limit trades about 5% of the
batching throughput gain for halving the delay here.

Not built: (1) enforcement of a declared class: a handler declared cheap that is actually slow still delays the batch,
and nothing detects it; a watchdog or per-handler timing check would report such violations. (2) A true time bound
independent of handler behaviour needs something other than the worker to send the held bytes (a flusher thread),
which means cross-thread access to a connection's output buffer and the lifetime hazards that come with it. Note also
that a long non-parking handler already delays every other connection on its worker; that is a runtime problem
(no preemption) that batching only makes more visible.

## Concurrent pipelined requests (RFC 7230 6.3.2): one child task per request, responses in order
Prototype in `server.c` (`conc=1`; raw data `http_conc.txt`). Each pipelined request runs as its own child task; the
connection task admits up to `depth` requests, collects the completed PREFIX in request order and sends it with one
write. POST is a barrier (admitted only when nothing is in flight, and nothing is admitted behind it until it has
finished). A lone request with nothing pipelined behind it runs inline (no spawn). The connection waits for all its
children before finishing (structured: a child never outlives the frame it reports to). New runtime pieces: per-worker
timer heap (`epoll_pwait2`, so handlers can park for a set time) and multi-source waits (socket and/or child).

Correctness (`conc_test.py`; each has a negative control that must fail):
* order: 16 requests whose handlers finish in reverse order (sleeps 32 ms .. 2 ms) come back as 0..15 in order, in 32 ms
  (sequential would take ~272 ms). `-DBREAK_ORDER` (send whichever finished first) returns 15..0: FAIL as expected.
* barrier: G G G P G G, 20 ms each, takes 61 ms (3 phases). `-DBREAK_BARRIER` takes 20 ms: FAIL as expected.
  Bug found by this test: my first version admitted requests behind an in-flight POST; fixed (`unsafe_busy`).
* depth: depth=4, 16 requests of 10 ms => 40 ms (depth is respected). close: `Connection: close` ends the stream.
* the 5,000-request fragmented-input test passes in this mode (4 seeds).

Instant handlers, 1 worker, 16 connections, K=16 pipelined, requests/s:
| server | rps |
|---|---|
| sequential, flush every response | 455-459k |
| sequential + opt-in batching (hold bounds, declarations) | 4.09M |
| concurrent, inline fast path | 4.21-4.23M |
| concurrent, always spawn | 4.20-4.27M |
My guess that spawning would cost 100-200 ns per request was wrong: it is not measurable here, and batching comes for free
(whatever prefix is complete is sent), with no hold time, no timers and no handler declarations. K=1 (no pipelining):
sequential 306k, concurrent inline 298-305k (0-3% slower), always-spawn 294k (4% slower).

Parking handlers (each waits 200 us on a timer, standing in for a DB call), 1 worker:
| | sequential | concurrent depth=16 | concurrent depth=4 |
|---|---|---|---|
| 1 connection, K=16: last response | 4.08 ms | 265 us | 1.03 ms |
| 1 connection, K=16: requests/s | 3.9k | 59.8k | 15.5k |
| 16 connections, K=16: requests/s | 62k | 808k | 214k |
| first response (all cases) | ~270-290 us | ~270-320 us | ~270-320 us |
Without pipelining (K=1, 16 connections) the two are identical (53k): overlap across connections already exists. The
gain is on the LAST response and on throughput; the first response is not faster.

CPU-bound slow request (GET /slow, ~1 ms) at position 9 of 16, instant handlers, one unloaded connection, first response:
| | first response |
|---|---|
| sequential, flush every response | 12 us |
| sequential + batching, slow undeclared | 955 us |
| sequential + batching, slow declared unbounded | 10 us |
| concurrent, slow undeclared | 957-963 us |
| concurrent, slow declared unbounded (barrier before it) | 10 us |
Concurrency does not help a CPU-bound handler on a pinned worker: while it spins, the connection task cannot run to send
the finished prefix. The same barrier used for POST fixes it: a route declared unbounded is admitted only after everything
ahead of it has finished and been sent. Something on another worker would be needed to send during the spin.

Memory: idle connection frame 88 B -> 153 B (the inline handler frame and ring pointer are embedded; 100k connections:
153 B/conn, 1.28M rps, unchanged). While pipelining, each in-flight request costs a 72 B child task, plus a 256 B ring
(from a pool, only while requests are in flight) and a transient 2 KB output buffer.

Not built / caveats: handlers are simulated (spin + timer), not user code; only same-worker concurrency (no cross-worker
wakeups); no cancellation (children run to completion and the connection waits for them); POST has no body; "unsafe" is
decided by method only; timer resolution is that of `epoll_pwait2`; one run per configuration for the parking and slow-CPU
tables, two for the instant-handler table.

## Syncing server.c with the rewritten server.stra (after the nullable / exceptions / enums rewrite)
The C had drifted from the source (it was never generated; it is a hand translation). Brought back into line (raw data
`http_conc_synced.txt`; the earlier `http_conc.txt` is the pre-sync data):
* 431 for a header block larger than the buffer (both modes; previously the connection just closed).
* A handler that throws (GET /boom) becomes a 500 for that request; the connection survives (both modes). Exceptions are
  emitted as error-return codes in frame `err` fields (the C already propagated errors this way), `try`/`catch` as branches.
* Each spawned child gets its OWN COPY of the request head (256 B pool block, or a 2 KB buffer for a larger head), and the
  handler frame reads the head after waiting, so the copy is exercised.
* Status-code responses via `reason_phrase()` (a switch on a plain value) as in the source.
New tests (`conc_test.py`), all pass in conc=0, conc=1 and conc=1 inline=0: `boom` (200,500,200,500,200 on one connection),
`toolarge` (431 then close), `copy` (four handlers waiting 60 ms while another request reuses the same buffer block: bodies
1,2,3,4,91). Negative control `-DBREAK_COPY` (children read the shared buffer): bodies [0,2,3,4,0] = FAIL, as expected.
The whole earlier suite still passes (order, barrier, close, depth, 5,000-request fragmented input in four server variants,
close-time flush: 2 responses delivered with it, 0 without), and the order and barrier negative controls still fail.

What it cost (1 worker, 16 connections; before -> after):
| case | before | after |
|---|---|---|
| 16 pipelined, always spawn (requests/s) | 4.20-4.27M | 4.00-4.04M (about -5%) |
| 16 pipelined, inline fast path | 4.21-4.23M | 3.94-3.99M (about -6%) |
| 16 pipelined, sequential flush-each | 455-459k | 444-445k |
| K=1, sequential / inline / spawn | 306k / 298-305k / 294k | 295-300k / 288-296k / 286-291k |
| 200 us parking handlers, 16 conns, depth 16 | 808k | 789k (about -2%) |
| 200 us parking handlers, 1 conn: last response | 265 us | 265 us |
| slow request at position 9, declared unbounded: first response | 10 us | 10 us (undeclared: 948 us) |
The extra cost of the head copy is a pool allocation and a ~40 B copy per spawned child. The sequential baseline also drifted
down 2-4% between the two sessions, so part of the difference is machine noise; the pipelined drop (5-6%) is larger than the
baseline drift and is plausibly the copy. Memory: connection frame 168 B (was 152) and 169 B/conn at 100k connections (was 153;
88 B/conn for the sequential server); each in-flight request now costs a 104 B child (was 72) plus its head copy.
Still not modelled in C: exception unwinding through nested scopes (errors are return codes, not unwinding), the
scope-waits-for-children rule under an exception (the C waits for children before finishing, by construction), and any
real handler code (handlers are still a spin and a timer).

## The equivalent Rust (Tokio) vs the C server
`rust/src/bin/http_pipe.rs`: the same protocol, routes and behaviour as `server.stra` / `server.c`, written as idiomatic Tokio
(one current-thread runtime and SO_REUSEPORT listener per core; a spawned task per pipelined request; `VecDeque<Slot>` of
`Done | JoinHandle`; the completed prefix is collected with `is_finished()` and sent as one write; POST and GET /slow are
barriers; 431; a handler `Err` becomes a 500; a lone request runs inline; each spawned task owns a `Vec` copy of the head).
Raw data: `http_rust_compare.txt`. Same load generator, 1 worker, 16 connections unless noted.

Correctness: the same tests all pass on the Rust server (order, barrier, depth, close, boom, toolarge, copy, and the
5,000-request fragmented-input test in both modes). `copy` has no negative control in Rust: a task cannot borrow the
connection's buffer (the compiler rejects it), whereas in C the bug is only caught by the test (`-DBREAK_COPY` fails).

Instant handlers, K=16 pipelined (requests/s):
| | C | Rust |
|---|---|---|
| sequential, flush each response | 450-454k | 445-446k |
| concurrent, inline fast path | 4.04-4.07M | 2.11M |
| concurrent, always spawn | 4.07-4.10M | 2.10-2.11M |
| Rust with spawning disabled (diagnostic, still batched) | | 3.81-3.82M |
No pipelining (K=1): C sequential 304k, Rust sequential 285-287k (about -6%); concurrent inline 297-298k vs 284-287k.
The sequential servers are level: the workload is syscall-bound. The pipelined gap is the per-request task: with the spawn
removed Rust reaches 3.8M, within about 6% of C, so `tokio::spawn` plus the owned head `Vec` costs roughly 210 ns per request
(about 475 ns per request with it, 262 ns without), against about 12 ns for the C frame and head copy (arena blocks, no heap).
This is an untuned Rust version and I did not profile it (no perf here); the diagnostic isolates the spawn, not every cost.

Parking handlers (K=16, 1 ms each, since Tokio's timer cannot honour less): the concurrency gain is the same in both,
about 16x on the last response (C 16.9 ms -> 1.07 ms, Rust 33.5 ms -> 2.09 ms; 16 connections: C 15.1k -> 229k requests/s,
Rust 7.6k -> 114k). The absolute numbers differ because Tokio's timer has 1 ms resolution and rounds up: a 1 ms sleep takes
about 2 ms, a 200 us sleep about 1.1-1.3 ms (C honours 200 us: 265 us for the batch). That is a property of Tokio's timer
wheel, not of the pipelining design.

CPU-bound slow request at position 9 of 16: identical behaviour. The barrier gives a first response of 10-13 us in both; without
it (C only, classify=0) 946 us. Sequential: 12 us in both. (The Rust handler is about 20% longer: 1.8 ms vs 1.5 ms per /slow.)

Idle memory per connection (100k connections): C 88 B sequential / 169 B pipelined; Rust 1,413 B / 1,539 B. The Rust futures
are 504 B / 584 B; the rest is the task header, the I/O registration and allocator overhead (not broken down).

Code size (non-blank, non-comment lines; NOT like for like): server.stra 373 (includes the Reader, Ring, Prefix, InFlight and
BatchedWriter types, the sequential and batched loops, and the runtime-leaf declarations), server.c 558 (plus rt.h), http_pipe.rs
298 (the sequential and pipelined loops only; Tokio, VecDeque and JoinHandle supply the runtime, ring and task types).

Pitfall found while measuring: the first Rust run of the slow-request test finished in microseconds because LLVM removed my spin
loop (the C compiler did not). The row was caught by checking a single request's latency (0.2 ms against 1.5 ms) and fixed with an
empty inline-asm barrier; the earlier rows are not affected (they use no spin).

## Size-class input buffers (replacing the fixed 2 KB buffer)
The fixed `BUFSZ` is gone from both `server.stra` (the reader's buffer is a runtime growable `u8[]`; the only number a program
states is the largest header block, `Reader.maxHead`) and `server.c` (512 B, 2 KB, 8 KB, 16 KB classes; `maxhead=` caps the
largest). A connection takes the 512 B class when its first byte arrives, compacts when the tail is full, and grows into the next
class (copy, then free the old block) only when the buffer is full of one unfinished header block. Requests are offsets into the
buffer, so growth never invalidates one. Output buffers (a completed prefix, held responses) stay one fixed 2 KB unit per write.
Raw data: `http_sizeclass.txt`.

Correctness: new `bigheader` test (headers of 3,000 / 10,000 / 15,500 B pipelined among small requests, sent in random fragments, so
the buffer must grow through the classes mid-pipeline): all statuses 200 and, in conc mode, bodies 0..7 in order, in all three
modes; `toolarge` (a 20 KB header block) is a 431 then close; `maxhead=2048` accepts a 1,500 B header and answers 431 for 3,000 B.
Negative control `-DBREAK_GROW` (grow without copying the unconsumed bytes): the server drops the connection, test FAIL. The whole
earlier suite still passes (order, barrier, close, depth, boom, copy, 5,000-request fragmented input in four variants, close-time
flush) and the order / barrier / copy negative controls still fail.

Memory, 20,000 connections held mid-request (partial header, no terminator), server RSS per connection:
| partial header | fixed 2 KB buffer | size classes |
|---|---|---|
| 60 B   | 2,136 B | 600 B (sequential), 689 B (conc) |
| 1,500 B | 2,137 B | 2,137 B (sequential), 2,225 B (conc): needs the 2 KB class either way |
| 10,000 B | 431 error | 12,379 B (16 KB class, only the touched pages resident) |
Idle connections are unchanged (no buffer: 88 B / 169 B+ per connection). The saving is for requests in flight, which is what an
attacker who opens many slow connections controls: a 60-byte partial header now pins 600 B per connection instead of 2 KB, and a
client has to actually send 8 KB to make the server hold an 8-16 KB buffer for it.

Throughput is unchanged (same session, 1 worker, 16 connections): 16 pipelined 4.09-4.12M requests/s before and after; K=1 sequential
307-314k vs 307-309k, concurrent 300k vs 299-301k; 200 us parking handlers 803k vs 804k.

Still open: large and unbounded arrays (request bodies, big collections) are not handled; the design doc keeps dedicated virtual
reservations (Goose-style) as the candidate for those. A size class is only ever left by growing, so a connection that once sent a
big header keeps its big buffer until it goes idle (fully consumed input releases it). In the C the largest class is 16 KB by
configuration; a program-chosen limit above that would need more classes.

## Request bodies: limits, streaming, and spooling to files (conc mode) - first version; superseded by "Two body readers" below
Requirement (from the design discussion): bodies must be limited; larger ones arrive as chunked or multipart; very large ones need
somewhere to live (files, or an explicit mmap); a real server never parses a 200 MB JSON document into structures. Raw data:
`http_body.txt`. Tests: `body_test.py`. Source: `server.stra` sections 2 and 3b; C: `server.c` ("request bodies").

Design (the nginx policy, with the pieces stated in the source): every route declares a `BodyPolicy`. `Buffered(max, memLimit)`: the library
reads the whole body BEFORE the handler runs; up to memLimit bytes stay in memory (a size-class array), more is spooled to an unnamed
temporary file (O_TMPFILE: no name, deleted when closed), over max is refused with 413 (a Content-Length over max before a single body
byte is read; a chunked body as soon as a chunk header would take it over). Content-Length and chunked bodies are decoded as a stream
through the connection's input buffer (chunk extensions and trailers handled, trailers capped), and the decoder stops exactly at the end
of the body, so a pipelined request behind it is untouched. A handler gets a `Body`: `bytes()` if it is in memory, `file()` / `map()` (an
explicit mmap) if it was spooled, so a huge document is processed by a streaming parser over the mapping, never held as a value. Small
JSON forms use a small cap and stay in memory. Also in the source, not built in C: `Stream(max)` (the handler reads incrementally) and
`Multipart(...)` (each part its own spooled Body). A request with a body on a route whose policy is `None` is refused (400).

C prototype: POST /upload (maxbody=100 MB, memlimit=16 KB by default; `maxbody=`, `memlimit=`, `tmpdir=`, `bodybudget=` options). It stores the
body and replies with its length, where it lives, and a CRC-32 of the bytes (so tests check a 200 MB upload byte for byte). The body is read by
the connection task itself (the route is unsafe, so the barrier guarantees nothing else is in flight). `conc=0` answers 501.

Tests (`body_test.py`), all pass:
* sizes: 0, 1, 100, 16,384 (memory) and 16,385, 300,000 (file), fixed and chunked (with extensions and trailers): digest and placement correct.
* pipelined: 24 requests (uploads of 0-70 KB in both framings, ordinary GETs, echo) in one write, random fragments: all answers in request order, every digest right.
* limits (maxbody=100,000): Content-Length 200,000 -> 413 before any body byte, connection closed; exactly 100,000 accepted (fixed and chunked); chunked 150,000 -> 413; the server still serves.
* malformed -> 400: Content-Length with chunked, bad hex size, missing CRLF after chunk data, no framing, repeated Content-Length, non-numeric Content-Length, a 20-digit chunk size.
* Expect: 100-continue: the interim response arrives, the body is then accepted.
* client vanishes mid-body (20 fixed + 20 chunked, ~150-200 KB each): server file descriptors 5 before, 5 after (spool files closed), still serving.
* 200 MB chunked upload: 0.47 s (about 450 MB/s), digest correct, placed in a file, server RSS 1 MB before and at peak (memory does not grow with the body).
Negative controls (all FAIL as intended): `-DBREAK_LIMIT` (limits not enforced: the oversized request hangs waiting for 200,000 bytes), `-DBREAK_OVERREAD` (swallow the next
request's bytes: connection reset), `-DBREAK_SPILL` (lose the in-memory part when spilling: the four file-sized cases fail). The whole earlier suite still passes.

Cost: nothing for ordinary requests. 16 pipelined 4.07-4.10M requests/s (unchanged), K=1 295k concurrent vs 304k sequential; the idle connection frame is now
192 B (was 176; the body pointer and budget), 192 B/conn at 100k connections (88 B for the sequential server), and an in-flight request 120 B (was 104).

Problem found by measuring, and fixed: the first version read a body for as long as the socket had data, so one fast upload monopolised its worker. Ordinary
requests on the same worker fell from 321k to 4k requests/s (p50 2 ms, worst 104 ms; 1.8k and 366 ms with a longer run). The body loop now has a byte budget per
turn (`bodybudget=`, default 64 KB) after which the connection goes to the back of the run queue. With uploads running back to back (a deliberately saturated mix):
| body budget | other connections (16, K=1) | upload throughput while sharing |
|---|---|---|
| never yield (control, `-DBREAK_FAIR`) | 1.8k req/s, p99 366 ms | |
| 256 KB | 25k req/s, p99 0.98 ms | 533 MB/s |
| 64 KB (default) | 78k req/s, p99 0.30 ms | 466 MB/s |
| 16 KB | 181k req/s, p99 0.12 ms | 333 MB/s |
| 8 KB | 234k req/s, p99 0.09 ms | 266 MB/s |
| no uploads (baseline) | 324k req/s, p99 0.07 ms | uploads alone 566 MB/s |
Smaller budgets trade upload speed for the latency of everything else; it is a knob, and the default is a compromise.

Known gaps in the C: (1) file writes are blocking `write()` calls on the worker. The 12-13 ms worst cases in every budget row are most likely such stalls (dirty-page
throttling); the source assumes offloaded blocking I/O (a parking `File.write`), which is not built. (2) Only POST /upload has a body in C: another route that receives
a body (for example a GET with one) would desynchronise the connection; the source refuses it with 400. (3) [RESOLVED, see "Lingering close" below] No lingering close: after a 413 or 400 with the body still
arriving, closing with unread data can reset the connection before the client reads the response (it did not in these tests, where the client stopped sending or read
concurrently). (4) `Stream` and `Multipart` policies, the streaming JSON parser and `mmap` handlers are source only. (5) `Expect: 100-continue` is best effort, and
trailers are validated for size only. (6) A body route is a barrier both ways, so one slow uploader blocks later requests on that same connection (not other connections).

## Two body readers: a small in-memory one for APIs, a file-backed one for uploads (supersedes the section above)
Requirement: typical API servers must not accept large bodies, so the runtime takes a configurable maximum; file uploads and similar need much
larger bodies, which is a runtime feature, not a language one, and suggests a different reader. The first version had one reader with a memory
threshold that spilled to a file; it is now two readers in the runtime library sharing one stream decoder (Content-Length or chunked, decoded through
the connection's input buffer, ending exactly at the end of the body):
* the STANDARD reader (`POST /api`; `Handler.Api` in the source): the whole body is read into memory before the handler runs, at most `maxbody=` (1 MB by
  default). It needs no files, and `Body.bytes()` cannot fail. Ordinary API routes use this and cannot receive more.
* the UPLOAD reader (`POST /upload`; `Handler.Upload(limit, h)`): a route opts in explicitly and states its own limit (never above `uploadmax=`, 4 GB by
  default). The body is written to an unnamed temporary file (`tmpdir=`) as it arrives; the handler gets the finished `Upload` (file, map, length, digest).
* the host passes a `ServerConfig` (source) / command-line options (C): `maxhead`, `maxbody`, `uploadmax`, `tmpdir`, `yield`, `depth`. The language knows
  nothing about any of it. `yield=` (formerly `bodybudget=`, still accepted) is the number of body bytes a connection processes before it yields to the others.
Raw data: `http_body.txt`; tests: `body_test.py`; source: `server.stra` sections 2 and 3b.

Tests, all pass: `sizes` (22 cases: API 0 B..1 MB in memory, upload 0 B..5 MB in a file, fixed and chunked with extensions and trailers, digests right);
`defaultcap` (the SAME 2 MB body is refused 413 by the API reader, fixed and chunked, and accepted by the upload reader; a 1 MB API body is accepted);
`limits` (maxbody=100,000 and uploadmax=500,000 are independent: each reader refuses its own oversize with 413, a Content-Length before any body byte, a
chunked body mid-stream, and accepts exactly its limit); `pipelined` (24 requests mixing both readers and ordinary requests in one write, random fragments,
answers in order); `malformed` (14 cases on both readers, all 400); `expect` (100-continue); `eofmid` (clients vanishing mid-body on both readers: fds 5->5,
RSS 1->1 MB); `large` (200 MB chunked upload in 0.46 s, about 458 MB/s, digest right, server RSS 1 MB at peak). Negative controls fail as intended:
`-DBREAK_LIMIT` (no limits: the test hangs waiting for the body), `-DBREAK_OVERREAD` (reset), `-DBREAK_SPILL` (all ten non-empty upload cases fail). The earlier
suite (26 checks) still passes.

Memory of the standard reader (500 connections, each declaring a 1,000,000 B body and sending 100,000 B of it): server RSS +52 MB (about 111 KB per connection:
what was actually received), VmSize +482 MB (the declared sizes, reserved but untouched), and +4 MB after the clients close (freed). So memory follows the bytes a
client really sends, not the size it declares, but the worst case is maxbody x connections that send everything, which is why a typical API server keeps maxbody small.

`yield=` is a runtime parameter (200 MB uploads back to back on the same worker as 16 ordinary connections):
| yield= | other connections | uploads while sharing |
|---|---|---|
| 4,096 | 241k req/s, p99 0.08 ms | 266 MB/s |
| 16,384 (`bodybudget=` alias) | 182k req/s, p99 0.12 ms | 333 MB/s |
| 65,536 (default) | 78k req/s, p99 0.30 ms | 466 MB/s |
| 1,048,576 | 5.5k req/s, p99 4.7 ms | 533 MB/s |
Same known gaps as before (blocking file writes on the worker, only /api and /upload carry bodies in the C, no lingering close [now resolved], `Expect: 100-continue` best
effort); `Stream` bodies and `multipart` are not built (multipart would be the upload reader applied once per part).

## Lingering close
Problem (flagged earlier): when the SERVER ends a connection while the client is still sending (an error answer such as 413, 400 or 431, or `Connection: close`
with more bytes on the way), closing a socket that holds unread input makes the kernel send a RESET. A client that writes its whole request and then reads the
answer (what most clients do) gets an error from its WRITE and never looks for the response. Reproduced: with `linger=0`, a client that writes 40 MB after a 413/431/400/
`Connection: close` gets ConnectionResetError on the write in every case.

Fix (`server.c`: start_linger, linger_resume; source: `Ending`, `runConnection`, `Conn.lingerClose`): the finished connection is handed to a small lingering task. It
half-closes the write side (`shutdown(SHUT_WR)`: the client sees the response and a clean end), then keeps reading and discarding input until the client closes, `linger=`
milliseconds pass (default 2000), or `lingerbytes=` bytes have been discarded (default 256 MB), and only then closes. It yields like a body reader (the `yield=` quantum), so a
hostile client cannot starve the worker. `linger=0` turns it off. The timeout is on the worker's timer heap and is CANCELLED when the task finishes early: the heap now keeps
each cancellable entry's index (`timer_cancel`), because a stale entry could otherwise wake a recycled task slot.

Policy, stated in the source and not hidden: always linger after an ERROR answer (431, 413, 400, 501: the client is probably still sending); after a plain `Connection: close`
linger only if the socket still holds unread input (`ioctl FIONREAD`). A client that has sent everything is closed directly and pays nothing. A reset is still possible if a
client sends AFTER a plain close with nothing pending; lingering cannot know about bytes that have not been sent.

Tests (`linger_test.py`, all pass): a naive client writes 40 MB, then reads, and gets its answer with the whole request written: 413 (API reader), 413 (upload reader), 431,
400, and 200 for `Connection: close` followed by 40 MB; sequential mode too (431, `Connection: close`). Limits: with a time cap and no byte cap an endless sender is closed at
exactly 0.5 s and 2.0 s; with `lingerbytes=4000000` the server stops accepting after about 3-4 MB; 2,000 connections that are answered with a 413 and closed by the client at
once leave the server's file descriptors unchanged (5 before, 5 after) and it keeps serving, in both modes. Negative control `linger=0`: every one of the naive-client cases
FAILs with ConnectionResetError. The full suite (46 checks) still passes and all 10 negative controls still fail.

Cost: with lingering on for EVERY server-initiated close, a close-per-request workload (`ab`, HTTP/1.0, 64 connections, one worker) went from 62-64k to 59-61k requests/s (about 3-5%)
and server CPU from 148 to 180 ticks (+20%). With the policy above that workload is closed directly: 63.1-64.2k requests/s with lingering enabled vs 63.9-66.0k with `linger=0` (three runs each; about 1.5% on
average, inside the run-to-run spread), server CPU 147-150 vs 143-146 ticks: the cost is essentially gone. Keep-alive throughput, the 200 us parking case and the timer paths are unchanged (4.11-4.21M requests/s at 16 pipelined;
parking 800k and 60k).

Remaining gaps: bytes a client sends after a plain close that had nothing pending can still produce a reset; the discard budget is per connection, not global, so many
simultaneous hostile lingerers each cost a task slot and a socket for up to `linger=` (not capped in number); `lingerClose` is a hand-off to the runtime, not something a
program can wait on.

## Code organisation: runtime and HTTP libraries separate from the server (`bench/c/http/README.md`)
`server.stra` and `server.c` used to hold everything (about 1,000 lines each). They are now split into layers so each file contains only what belongs to it, and
anything that can be written in Strata is a `.stra` file, with C used only for what needs the operating system or the scheduler:
* `runtime/`: `leaves.stra` (the runtime's interface: signatures only), `io.stra` (`writeAll`), `ring.stra` (`Ring<T>`); C: `rt.h` (tasks, pools, scheduler, timers),
  `rt_buf.h` (size-class buffers), `rt_io.h` (`write_all`, lingering close), `rt_net.h` (workers, listener, accept loop, with two hooks the application fills in).
* `http/`: `types.stra`, `config.stra`, `response.stra`, `route.stra`, `body.stra`, `reader.stra`, `pipeline.stra`, `batching.stra`; C: `http_msg.h`, `http_reader.h`, `http_body.h`.
* `server.stra` (252 lines) and `server.c` (597 lines): the route table, the handlers, the two connection loops, `runConnection`, `serve`, `main`. The Strata files `import` what
  they use (a proposed module syntax; one file is one module).
Some Strata types have no separate C file because the C keeps them inline for speed (the ring and the ordered set of in-flight requests live inside `conc_step`): the
headers of those `.stra` files say where. `./build.sh` builds the server and every test variant; `./run_tests.sh` runs everything (46 tests PASS, 11 negative controls FAIL, as before the
split). The split is mechanical: the same code in different files, with the globals moved to the layer that owns them. Throughput is unchanged (`http_layout.txt`: 4.11-4.21M requests/s at
16 pipelined, 307k at K=1 concurrent and 313k sequential, 799k with 200 us parking handlers). Sections above that refer to "server.c" or "server.stra" describe the code as it was in one file; the
names are the same, only the files differ. One leftover: `Worker` in `rt.h` still has a named pool per application type (requests, rings, heads, bodies, lingers); a generic slot array would
keep application types out of the runtime entirely.

## Go added to the comparison (C / Rust / Go; `bench/go/`, raw data `http_go_compare.txt`)
Two Go servers, standard library only, Go 1.24.4:
* `bench/go/pipe/main.go`: the equivalent of the Strata and Rust servers, written the way a Go programmer would (goroutines, channels, `sync.Pool`): same routes and
  behaviour, a goroutine per pipelined request, the completed prefix sent as one write, barriers, depth limit, inline path, 431, a handler error becomes a 500, each goroutine owns a copy
  of the request head, size-class input buffers released when a connection is idle. `conc=0` flushes every response. `batch=1` calls `runtime.Gosched()` once after the oldest
  request finishes (see below).
* `bench/go/stdhttp/main.go`: what a Go programmer writes with `net/http`: one handler, the standard server, a recover middleware (a panic in `/boom` becomes a 500).
  `net/http` reads a connection's next request only after answering the previous one, so pipelined requests are served one at a time; that is Go's design.
Scope matches the Rust comparison: routes `/`, `/echo/N?s=US`, `/slow`, `/boom`, POST `/` (barrier); NOT the request-body readers (`/api`, `/upload`, 413) and not lingering close. Method:
one worker = GOMAXPROCS=1 under `taskset -c 0` (C and Rust pinned to core 0 as before), load generator on cores 8-11, three seconds per point. `net/http` refuses an HTTP/1.1 request without a `Host`
header (400), so the load generator got `host=1` and the tests got `HOST_HDR=1` (the same requests are sent to every server in these runs). Go's CPU-spin handler checked at about the same cost as the others
(one `GET /slow` takes 1.4-1.9 ms in every server).

Correctness: `gopipe` passes the same tests as the Rust server in every mode: order, barrier, close, boom (500 and the connection survives), copy, bigheader, toolarge (431), depth, 4 workers, and the
5,000-request fragmented-input test. `net/http` passes boom, bigheader, close, copy, toolarge (at a 60 KB header: its limit is looser than `MaxHeaderBytes`, it accepted 20 KB with the limit at 12 KB)
and the fragmented-input test, and fails order and barrier as expected (277 ms for 16 x 10 ms handlers: serial).

Instant handlers, one core, 16 connections (requests/s; two runs where a range is given):
| | C | Rust | Go `pipe` | Go `net/http` |
|---|---|---|---|---|
| 16 pipelined, flush each response | 457-462k | 450-451k | 429-432k | |
| 16 pipelined, concurrent | 4.05-4.06M | 2.08-2.10M | 2.12M (`batch=1`) | |
| 16 pipelined, concurrent without the Gosched step | | | 391-396k | |
| 16 pipelined, `net/http` | | | | 219k |
| no pipelining (K=1) | 301-310k | 290-292k | 271-279k | 174k |
Syscall-bound cases are close (flush-each: 457k, 451k, 430k). The concurrent pipelined case separates the designs: C's arena-allocated child frames cost almost nothing, while a task per request costs
about the same in Tokio and Go (about 2.1M), half of C. `net/http` is about half of the hand-written Go server on the same work, because of its general machinery (header maps, a response writer and
buffered reader/writer per connection).

Go scheduler effect: without `runtime.Gosched()` the concurrent Go server runs at 391-396k, one fifth of the batched figure. When the oldest request's goroutine closes its channel, Go makes the woken
connection goroutine run NEXT (runnext), before the other ready request goroutines, so it collects one response, writes it, and goes back to wait. Adding one `Gosched` lets the siblings finish first and the
whole completed prefix goes out in one write. This is the same dependence on scheduling order that the C runtime has (a wakeup goes to the tail of the run queue, which is what batches), but Go's default order
is the opposite and the code has to know it.

Parking handlers (K=16 pipelined; time until the 16th response, p50; requests/s):
| handler sleeps | C | Rust | Go `pipe` | Go `pipe` sequential | Go `net/http` |
|---|---|---|---|---|---|
| 200 us, 1 connection | 265 us, 60.0k | 1.08 ms, 12.6k | 1.08 ms, 14.8k | 16.9 ms, 0.95k | 16.95 ms, 0.94k |
| 200 us, 16 connections | 322 us, 792k | 1.24 ms, 177k | 1.26 ms, 206k | 17.6 ms, 14.5k | 18.4 ms, 14.1k |
| 1 ms, 1 connection | 1.07 ms, 14.9k | 2.09 ms, 7.7k | 1.08 ms, 14.8k | 16.9 ms, 0.95k | 16.95 ms, 0.94k |
| 1 ms, 16 connections | 1.08 ms, 234k | 2.29 ms, 113k | 1.26 ms, 201k | 17.6 ms, 14.6k | 18.4 ms, 13.8k |
Concurrency within a connection gives the same ~16x on the last response in C, Rust and Go (the sequential servers take 16 x the sleep). Timer resolution differs: Go's `time.Sleep(200us)` takes
about 1.07 ms, because with nothing else runnable the Go runtime waits in `epoll_wait`, which takes whole milliseconds; Tokio's wheel has a 1 ms tick and rounds up, so a 1 ms sleep takes 2 ms; the C runtime
uses `epoll_pwait2` and honours 200 us. So at 1 ms Go is close to C, and below 1 ms only C keeps its precision.

Slow CPU-bound request at position 9 of 16 (first response, one connection): 10-12 us in every server (barrier before the slow request, or flush-each in the sequential and `net/http` servers).

Memory per idle connection (100,000 connections, 4 workers; server RSS delta; user space only, the kernel adds about 3.1-3.6 KB per socket to all of them):
| server | sequential loop | pipelined loop |
|---|---|---|
| C | 89 B | 192 B |
| Rust (Tokio) | 1,413 B | 1,547 B |
| Go `pipe` | 3,958 B | 5,401 B |
| Go `net/http` | 33,321 B (3.2 GB for 100k) | |
With 64 connections active on top of the 100,000 idle ones: C 1.29-1.30M requests/s (p99 79-87 us), Rust 1.23-1.24M (71-77 us), Go `pipe` 1.07-1.12M (162-174 us), Go `net/http` 587k (371 us). Go's
cost per connection is a goroutine stack (grown beyond the 2 KB minimum by the parser and the handler), the per-connection state and, for `net/http`, a 4 KB reader and a 4 KB writer buffer kept for
the life of the connection plus the request and header structures; the hand-written Go server holds no input buffer while idle.

Code size, non-blank non-comment lines (not like for like): Go `pipe` 462, Rust `http_pipe` 298, Go `net/http` 93; the Strata server file is 252 lines of server plus the libraries, the C server 597.

Reading the three together: C (hand-translated Strata) is fastest and by far the leanest per connection because child frames and connection state live in per-type arenas with no per-request allocation;
Rust and Go are within a factor of two of it on pipelined throughput and equal each other there; Go uses roughly 3x Rust's memory per connection, and `net/http` roughly 20x; Go has the best
timer behaviour of the two managed runtimes at 1 ms and is subject to the same sub-ms floor as Tokio; and a Go program written with the standard server gives up in-connection concurrency entirely.
Caveats: single runs per point except where a range is shown; loopback; the hand-written servers skip bodies and lingering; Go's GC settings are the defaults (no tuning), and the Rust and Go servers were
not profiled.
