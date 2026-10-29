# Strata

A Rust alternative with Goose-style arenas: compiler-managed lifetimes, virtual threads, native C output. The design is in `strata-design.md`.
Nothing here compiles Strata yet; the `.stra` files are the source as it would be written, and `c/generated/` holds the C a compiler would emit for them (written by hand and kept in step by the tests).

```
strata-design.md     the design specification
stra/                Strata source only
  runtime/             the runtime's interface and the parts writable in Strata
  http/                HTTP library and server.stra; styles/{cpp,java,mixed}/server.stra are the same server in three syntax styles
  redis/               mini-redis library and server.stra
  experiments/         handler classification study (older syntax)
c/                   C only
  runtime/             hand-written runtime: scheduler, buffers, I/O, sockets, locks (rt*.h)
  generated/           stand-ins for compiler output: http/, redis/ (each has server.c)
bench/               everything that measures or tests
  http/  redis/        build scripts, tests, load generators for each server
  c/ rust/ go/ java/   micro-benchmarks and comparators
  results/             recorded measurements and write-ups
tools/               strata2html.py (render a .stra file as HTML), camel.py (rename snake_case identifiers)
```

Naming: Strata identifiers (functions, methods, fields, parameters, locals) are camelCase; constants stay UPPER_CASE. The C may use underscores.
To run a server's tests: `cd bench/http && ./build.sh && ./run_tests.sh` (same for `bench/redis`).
