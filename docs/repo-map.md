# Repository map

```
strata-design.md     the design specification
stra/                Strata source only
  runtime/             runtime interface and the parts writable in Strata
  http/                HTTP library and server.stra; styles/{cpp,java,mixed}/server.stra are the same server in three syntax styles
  redis/               mini-redis library and server.stra
  experiments/         handler classification study
c/
  runtime/             hand-written runtime (rt*.h)
  generated/           stand-ins for compiler output: http/, redis/
bench/
  http/  redis/        build scripts, tests, load generators
  c/ rust/ go/ java/   micro-benchmarks and comparators
  results/             recorded measurements
tools/               strata2html.py, camel.py
docs/                this site; docs/architecture/ holds compilation, suspension and module design
```

Run a server's tests with `cd bench/http && ./build.sh && ./run_tests.sh`, and the same in `bench/redis`.
