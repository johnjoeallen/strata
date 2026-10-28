# Strata

Strata is a design for a systems language that keeps Rust's memory safety and Goose's compiler-managed arenas, and asks the programmer for far less ceremony. It aims at servers: many cheap virtual threads, shared caches, deterministic cleanup, and no lifetime annotations.

!!! note "Status"
    Exploratory. There is no compiler yet. The `.stra` files in the repository are Strata as it would be written, and the C beside them is what a compiler would emit, written by hand and kept honest by tests and benchmarks.

## What this site is

A record of how the design got where it is: what was measured, what was decided and why, and what turned out to be wrong. Each step started as a question and ended as a number or a rule.

[Start the journey](journey/index.md){ .md-button .md-button--primary }
[Decisions so far](decisions.md){ .md-button }

## The design in six lines

1. Ordinary values carry no reference-count header; `new` makes a strong `Ref<T>`, and `WeakRef<T>` breaks cycles.
2. Lifetimes are inferred, not written.
3. Shared state is accessed through `sync` (read) and `mutate` (write) blocks; entering one may park the virtual thread.
4. Nothing is hidden except virtual threads and parking: no implicit buffering, batching or retry.
5. Every unbounded input has a declared limit, enforced before the data is held.
6. Errors are exceptions declared with `throws`; cleanup never parks and never throws.

The full text is [`strata-design.md`](https://github.com/johnjoeallen/strata/blob/main/strata-design.md).
