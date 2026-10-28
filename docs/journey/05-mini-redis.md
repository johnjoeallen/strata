# 5. mini-redis

The second proving ground, chosen because it differs from HTTP: long-lived connections, pub/sub fan-out and one shared database. The server side of `tokio-rs/mini-redis` was converted (frame, parse, connection, db, server, shutdown, commands).

!!! note "Conversion depends on no third-party crates"
    If a program needs a crate, Strata would have no equivalent. The README maps each crate used by the original to a replacement, and lists the standard-library pieces the Strata source assumes.

## What was new

- **Cross-worker wakeups:** a publish on one worker must wake a subscriber parked on another.
- **Backpressure:** one mailbox per subscriber connection with a declared capacity and an explicit lag policy, `Disconnect` (default) or `Notice`.
- **A reaper:** `cleanup()` cannot take a lock, so a dying subscriber hands its subscriptions to a queue, and a purge vthread unsubscribes them.
- **Fairness:** a pipelining publisher starved subscribers until each connection yielded every 64 commands.
- **Limits everywhere:** frame size, bulk size, key length, key count, topics, subscriptions and queue depth.

## Measured

| Item | Result |
|---|---|
| GET/SET 90/10, 1 worker | 328k ops/s unpipelined, 468k pipelined 16 deep |
| 4 workers | 1.10M to 1.94M; the load generator tops out near 1.3M unpipelined |
| Pub/sub deliveries, 1 / 4 workers | 440 to 456k/s / up to 1.98M/s |
| Median delivery latency, 1 worker | 12 µs for 1 subscriber, 84 µs for 64, 1.4 ms for 1,000 |
| Idle connection / idle subscriber | 297 B / 354 to 594 B |

The idle subscriber first cost 4.5 KB because the mailbox ring was allocated at full size; growing it on demand fixed that.

## Tests and honesty

27 tests pass on six builds (plain, park-only, and both under AddressSanitizer and ThreadSanitizer), with four negative controls. Bugs found along the way included a wake sent to an uninitialised worker and two data races, fixed.

There is no baseline: building the original Rust server was blocked by the permission system, so none of these numbers is compared with Rust.
