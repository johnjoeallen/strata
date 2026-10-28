# bench/go - Go comparators for the HTTP server

* `pipe/main.go`   the same design as the Strata/C and Rust servers, in Go (goroutine per pipelined request, ordered completion, barriers, depth limit)
* `stdhttp/main.go` plain `net/http`

```
go build -o gopipe ./pipe && go build -o gostd ./stdhttp
taskset -c 0 ./gopipe port=18080 workers=1 conc=1 depth=16 inline=1 batch=1 sleep_us=0     # workers= is GOMAXPROCS; pin with taskset
taskset -c 0 ./gostd  port=18080 workers=1
```
Tests (from `bench/c/http`; `HOST_HDR=1` adds a Host header, which net/http requires): `HOST_HDR=1 python3 conc_test.py 18080 boom`, `python3 frag_test.py 18080 5000 1`, and so on.
Benchmarks: `source ../c/http/compare.sh` and use `cmp`. Results and caveats: `../results/http.md` (section "Go added to the comparison"), data in `../results/http_go_compare.txt`.
