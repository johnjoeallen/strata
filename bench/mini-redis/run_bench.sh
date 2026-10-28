#!/bin/bash
# Benchmarks for the Strata/C mini-redis server. Server on cores 0.., load generator on cores 8.. . usage: ./run_bench.sh > ../results/miniredis_c.txt
cd "$(dirname "$0")"
PORT=16379; PID=/tmp/claude-1002/mrb.pid
start() { ${BIN:-./server} port=$PORT cpu0=0 "$@" >/tmp/claude-1002/mrb.log 2>&1 & echo $! > $PID; for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done; }
stop() { kill $(cat $PID) 2>/dev/null; for i in $(seq 100); do kill -0 $(cat $PID) 2>/dev/null || break; sleep 0.1; done; }
rss() { awk '/VmRSS/{print $2}' /proc/$(cat $PID)/status; }
if ss -ltn | grep -q ":$PORT "; then echo "port $PORT busy" >&2; exit 2; fi
echo "# mini-redis (Strata/C) benchmarks, $(date -u +%F), server cores 0.., loadgen cores 8.."
echo "## GET/SET 90/10, 16-byte values, 10000 keys: ops/s and batch latency (one batch = pipe commands, each answered separately)"
for w in 1 4; do for spec in "16 1" "16 16" "64 1"; do set -- $spec
  start workers=$w; ./loadgen_resp port=$PORT conns=$1 pipe=$2 seconds=4 threads=$(( w>1 ? 4 : 1 )) cpu0=8 | tail -1 | sed "s/^/workers=$w /"; stop; done; done
echo "## values: SET/GET 90/10 with larger values, 1 worker, 16 conns, pipe=1"
for v in 16 256 4096 65536; do start workers=1; ./loadgen_resp port=$PORT conns=16 pipe=1 vsize=$v seconds=3 cpu0=8 | tail -1; stop; done
echo "## pub/sub fan-out, saturated: 4 closed-loop publishers, vsize=64, 1 channel (ops/s = publishes/s; latency here is queue-fill time, not meaningful)"
for w in 1 4; do for s in 1 16 64 256; do
  start workers=$w subqueue=65536 maxconns=1000; ./loadgen_resp port=$PORT mode=pubsub subs=$s conns=4 chans=1 vsize=64 pipe=1 seconds=4 threads=$(( w>1 ? 4 : 1 )) cpu0=8 | tail -1 | sed "s/^/workers=$w /"; stop; done; done
echo "## pub/sub delivery latency at a fixed load: 4 paced publishers, vsize=64, rate = min(10000, 200000/subs) publishes/s so deliveries stay near 200k/s"
for w in 1 4; do for s in 1 16 64 256 1000; do R=$(( 200000 / s )); [ $R -gt 10000 ] && R=10000
  start workers=$w subqueue=4096 maxconns=2000; ./loadgen_resp port=$PORT mode=pubsub subs=$s conns=4 chans=1 vsize=64 pipe=1 rate=$R seconds=4 threads=$(( w>1 ? 4 : 1 )) cpu0=8 | tail -1 | sed "s/^/workers=$w /"; stop; done; done
echo "## idle memory, 100000 connections, 4 workers (RSS of the server, KB)"
start workers=4 maxconns=200000; B=$(rss)
./loadgen_resp port=$PORT mode=idle conns=100000 threads=4 srcips=5 srcbase=1 hold=3 cpu0=8 > /tmp/claude-1002/idle.out & L=$!
for i in $(seq 300); do grep -q READY /tmp/claude-1002/idle.out && break; sleep 0.2; done; sleep 0.5; A=$(rss)
echo "idle (PING once each, no subscription): base=${B}KB with_conns=${A}KB per_conn=$(( (A-B)*1024/100000 )) B"; wait $L; stop
start workers=4 maxconns=200000; B=$(rss)
./loadgen_resp port=$PORT mode=idlesub conns=100000 chans=1 threads=4 srcips=5 srcbase=2 hold=3 cpu0=8 > /tmp/claude-1002/idle.out & L=$!
for i in $(seq 300); do grep -q READY /tmp/claude-1002/idle.out && break; sleep 0.2; done; sleep 0.5; A=$(rss)
echo "idle subscribers (each subscribed to 1 shared channel, subqueue=1024): base=${B}KB with_conns=${A}KB per_conn=$(( (A-B)*1024/100000 )) B"; wait $L; stop
start workers=4 maxconns=200000 subqueue=16; B=$(rss)
./loadgen_resp port=$PORT mode=idlesub conns=100000 chans=1 threads=4 srcips=5 srcbase=3 hold=3 cpu0=8 > /tmp/claude-1002/idle.out & L=$!
for i in $(seq 300); do grep -q READY /tmp/claude-1002/idle.out && break; sleep 0.2; done; sleep 0.5; A=$(rss)
echo "idle subscribers, subqueue=16: base=${B}KB with_conns=${A}KB per_conn=$(( (A-B)*1024/100000 )) B"; wait $L; stop
