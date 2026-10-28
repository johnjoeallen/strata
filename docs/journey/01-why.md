# 1. Why Strata

Goose offers compiler-managed lifetimes, native C output and large virtual-memory reservations that let selected arrays grow in place. Its strict thread isolation and limited dynamic allocation make typical server programs awkward.

Strata keeps those ideas and adds what a server needs:

- dynamically created objects and graphs with independent lifetimes
- thousands of virtual threads running ordinary sequential code
- efficient shared, read-heavy state such as caches
- deterministic cleanup of files, sockets and locks
- reference safety without writing lifetime names like `'a`

It is not a Goose fork or a Rust dialect, and it does not try for feature parity with Rust. Understandable semantics win over zero overhead at all costs.

## The first rules

Two rules shaped everything after them, and both came from the user rather than from measurement:

!!! tip "Principle 10: no hidden magic"
    Aside from virtual threads and parking, nothing happens behind the programmer's back. No implicit buffering, no implicit batching. A library may offer batching, but only when the program asks for it, and it is off by default.

!!! tip "Principle 11: every unbounded input has a limit"
    A header block, a request body and a parsed document's depth all have limits stated by the program. Big payloads are streamed or spooled to a file, never parsed whole. Exceeding a limit is an error the caller can see.
