# HTTP server - an HTTP server written as it would be in Strata, with the C a compiler would emit for it

The server is split into layers. The Strata files are the source; each has a C counterpart that stands in for the compiler's output (hand-written, kept
in step by the tests). Anything that CAN be written in Strata is in a `.stra` file; C is used only for what needs the operating system or the scheduler.

```
stra/runtime/                 the runtime, Strata side
  leaves.stra                   its interface: signatures of what only C can do (sockets, files, timers, scheduler, size-class buffers)   -> c/runtime/rt*.h
  io.stra                       writeAll                                                                                                   -> rt_io.h
  ring.stra                     Ring<T>, a fixed-capacity queue                                                                            (inline in server.c)
stra/http/                    the HTTP library, Strata side
  types.stra                    Request, Resp, Method, Cost, Input, Ending, exceptions                                                     -> http_msg.h
  config.stra                   ServerConfig (limits, yield size, linger): passed in by the host                                           -> options in server.c main()
  response.stra                 canned responses, reasonPhrase, keepsAlive                                                                 -> http_msg.h
  route.stra                    Handler, Payload, Route, runHandler                                                                        -> route() in server.c
  body.stra                     request bodies: framing decoder, sinks, Body, Upload                                                       -> http_body.h
  reader.stra                   Reader: the connection's input, request heads, the two body readers                                        -> http_reader.h, rt_buf.h, conc_step
  pipeline.stra                 Prefix, InFlight: the ordered set of in-flight requests                                                    (inline in conc_step)
  batching.stra                 opt-in response batching (off by default)                                                                  -> BATCH_N blocks of handle_step
  server.stra                   the HTTP server itself: the route table, the handlers, the connection loops (sequential and pipelined), runConnection, serve, main
c/runtime/                    C only: tasks, pools, scheduler, timers (rt.h) | buffers (rt_buf.h) | write_all and lingering close (rt_io.h) | workers, listener, accept loop (rt_net.h)
c/generated/http/             the C counterparts, hand-written stand-ins for compiler output: http_msg.h  http_reader.h  http_body.h  server.c
bench/http/                   tests, load generator and scripts (this directory)
```
```
Imports: one file is one module; `import runtime.leaves` makes the public declarations of `runtime/leaves.stra` visible (a proposal; there are no cycles).

Comparators in other languages, driven by the same tests and load generator: `bench/rust/src/bin/http_pipe.rs` (Tokio) and `bench/go/` (`pipe`: the same design in Go; `stdhttp`: plain `net/http`). Results are in `bench/results/http.md`.

Build and test, from `bench/http/` (needs gcc, python3; `ab` and the load generator `loadgen` are for the benchmarks):
```
./build.sh          # builds `server` and every variant the tests use (opt-in features and negative controls)
./run_tests.sh      # every test must PASS and every negative control must FAIL (FRAG=frag_test.py also runs the fragmented-input test)
```

## Syntax used in the Strata files (a proposal, not yet in the design doc)
Syntax used here (a proposal, not yet in the design doc):
  * `T?` is a nullable T. The compiler narrows it after a null check, an assignment or an `assert`. There is no Option type.
  * Methods are declared inside the type. `self` is available in every non-static method and is OPTIONAL: members can be
    used bare (`head`) or as `self.head`. It is needed only to tell a member from a parameter or local of the same name,
    and to refer to the value itself (`switch (self)`). By convention here, reads are bare and writes say `self.`.
  * `let` is immutable, `var` is mutable. `T&` is a borrowed reference (design section 3); there is no shared/mutable split.
  * Tagged enums: `enum E { A(payload) B C(x, y) }` with an exhaustive `switch (e) { A(p) -> { ... } B -> { ... } }`. Each arm
    is `Pattern -> { block }`: no `case` keyword, no fallthrough. Several values share an arm with a comma (`"GET", "HEAD" -> {}`).
    A switch on a non-enum value needs a `default -> { ... }` arm. `break label` leaves a labelled loop from inside a switch.
    Enums may have methods. Not yet defined (deferred): ranges, guards, bindings, nested destructuring.
  * Errors are exceptions: `throw`, `try`/`catch`. A function declares what can escape it with `throws A, B`; the compiler
    checks the list and tooling shows it (as it shows parking). No `throws` clause means nothing escapes. A throw runs the
    cleanups of every scope it leaves (design section 9). Cleanup is a `cleanup()` method; it never parks and never throws.
  * `spawn f(args)` requires f to declare no `throws`: the root of a vthread handles its own errors.
  * No async/await, no function colouring: a call that waits simply blocks the vthread. The compiler infers which functions
    may park and sizes each frame from the values live across those parks. Per design principle 10, parking is the only
    hidden behaviour: nothing buffers, batches, defers or reorders unless the code, or a documented library type it uses, says so.
  * Effects are inferred, not written. Whether a function may park, and how large its frame is, are results the compiler computes and tooling shows (hover, lints,
    error messages that name the call chain). The only places a program states it are declarations that have no body to analyse: an `extern` function says `parks` if it
    may wait (`extern parks fn recv(...)`), and a function-typed parameter can require the opposite with `noparks` (`handler: fn(u8[]&): Resp noparks`).


## Helpers the Strata files assume but do not define
Assumed library helpers, not defined in this file: ok(text): Resp, HELLO, Pattern(string), routes.find(head), queryUs(head),
pathParam(head), expensiveSummary(), NO_BYTES, MAX_RESPONSE, log(e), exit(code), leak(bytes), string/byte-slice helpers
(contains, find, lines, startsWithIgnoreCase, copyFrom, moveWithin, copy), Instant, Duration, spawnWorker.
Body-related: hexValue(c), headExpects100Continue(head), min, hex(u32), , json.parse / json.stream / JsonLimits, created(),
users, importer, User, Row, NO_BYTES.
