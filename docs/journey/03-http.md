# 3. An HTTP server

The first proving ground. The server is written as `.stra` source, with a hand-written C counterpart standing in for compiler output. Tests pin the two together.

## What it showed

| 4 workers, 64 active connections | per idle connection | requests/s |
|---|---|---|
| Strata-style C, 100k connections | 89 B | 1.29M |
| Tokio, lazy futures | 897 B | 1.25M |
| Tokio, naive handler | 3,074 B | 1.25M |

- The 88 B is `sizeof(ConnTask)` and nothing else: an idle connection holds no buffer.
- Throughput is nearly equal because the workload is syscall-bound (about 3 microseconds per request). The frame model buys memory, not a large throughput win.
- Kernel memory (3 to 3.6 KB per socket) dwarfs all of it and is the same for every server.

## Features that were added, each from a review

- **Size-class input buffers** replaced a fixed 2 KB buffer; the reader no longer moves bytes per request.
- **Two body readers:** a small in-memory one for APIs (1 MB default) and a file-backed upload reader with a much larger limit. Both refuse an oversize body with 413 before holding it.
- **Pipelined requests** run as one child task each, answered in request order, with a barrier for unsafe methods.
- **Opt-in response batching,** off by default, with a bound on held responses and a max hold time. It exists to honour principle 10.
- **Lingering close,** so an error response is not destroyed by a reset when the client is still sending.
- **Layering:** generic support moved into runtime and library files; `server.stra` holds only the server.

## Negative controls

Each important property has a build that breaks it on purpose (responses in completion order, no body limits, no yield, and so on), and the matching test must fail. 38 tests pass and every control fails as intended.
