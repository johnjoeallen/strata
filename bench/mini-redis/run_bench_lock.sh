#!/bin/bash
# The lock comparison: park-only vs spin-then-park, by read/write mix. usage: ./run_bench_lock.sh
cd "$(dirname "$0")"
PORT=16379; PID=/tmp/claude-1002/mrb.pid
start() { ${BIN:-./server} port=$PORT cpu0=0 "$@" >/tmp/claude-1002/mrb.log 2>&1 & echo $! > $PID; for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done; }
stop() { kill $(cat $PID) 2>/dev/null; for i in $(seq 100); do kill -0 $(cat $PID) 2>/dev/null || break; sleep 0.1; done; }
if ss -ltn | grep -q ":$PORT "; then echo "port $PORT busy" >&2; exit 2; fi
echo "# the lock, $(date -u +%F): 4 workers on cores 0-3, 64 connections on 4 loadgen threads (cores 8-11), 1000 keys, 4 s per point"
echo "# server_park = park at once; server_spin100 / server (spin 2000) = spin that many rounds first. lock_contended = acquisitions that had to park (cumulative)"
for bin in ./server_park ./server_spin100 ./server; do for rp in 100 90 50 0; do
  BIN=$bin start workers=4 stats=1; ./loadgen_resp port=$PORT conns=64 pipe=1 readpct=$rp keys=1000 seconds=4 threads=4 cpu0=8 | tail -1 | sed "s|^|$bin read%=$rp |" | sed 's/batch_lat.*errors/errors/'; sleep 1.2
  grep '^live' /tmp/claude-1002/mrb.log | tail -1 | sed 's/^/    server: /'; stop; done; done
