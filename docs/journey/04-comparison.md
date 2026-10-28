# 4. Against Rust and Go

The same design was written in Rust (Tokio) and Go (a pipelined server, and plain `net/http`), driven by the same tests and load generator.

| Pipelined, 1 worker | requests/s |
|---|---|
| Strata-style C | 4.05M |
| Rust (Tokio) | 2.1M |
| Go | 2.12M |

| Idle memory per connection | bytes |
|---|---|
| Strata-style C | 89 to 192 |
| Rust | about 1,400 |
| Go | 4,000 to 5,000 |
| Go `net/http` | 33,000 |

## How to read it

!!! warning "Caveats"
    Real handlers do real work, which shrinks the throughput gap a lot. What survives is memory per connection, in-connection concurrency, the timer floor and Go's garbage-collection tail. All runs are single runs on loopback.

Browsers do not pipeline HTTP/1.1 (they multiplex over HTTP/2), so the pipelined number says more about the runtime's cost per request than about web traffic.
