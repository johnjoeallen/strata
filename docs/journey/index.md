# The journey

| Step | Question | Answer in one line |
|---|---|---|
| [1. Why Strata](01-why.md) | What does Goose leave out for servers? | Dynamic objects, shared state and many lightweight threads. |
| [2. Foundations](02-foundations.md) | What do fibers, arenas, refcounts and frames cost? | A 4 KB fiber floor, free arena layout choices, no biased counts, about 1 ns per parking call. |
| [3. An HTTP server](03-http.md) | Can the model carry a real server? | Yes: 88 to 94 B per idle connection, throughput syscall-bound. |
| [4. Rust and Go](04-comparison.md) | Is it any better than what exists? | Memory per connection, yes; pipelined throughput, about 2x; real handlers shrink the gap. |
| [5. mini-redis](05-mini-redis.md) | What does a second, different server break? | Cross-worker wakeups, pub/sub backpressure, a shared database. |
| [6. Locks that park](06-locks.md) | How should `sync` and `mutate` wait? | Park in FIFO order, spin briefly first, acquire several locks all-or-nothing. |

All numbers were measured on one machine (Ryzen 9 9950X3D, Linux 6.12, gcc 14.2, rustc 1.93, Go 1.24, OpenJDK 25), usually from single runs. Raw data lives in [`bench/results/`](https://github.com/johnjoeallen/strata/tree/main/bench/results).
