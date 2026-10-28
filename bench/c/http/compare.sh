#!/bin/bash
# Helper for the C / Rust / Go comparison (source it): cmp "label" conns K "extra loadgen args" -- server command...
# Every server gets the same requests, with a Host header (host=1), because net/http refuses an HTTP/1.1 request without one.
# Pin a Go server with taskset, e.g.: cmp "Go pipe" 16 16 "" -- taskset -c 0 ../../go/gopipe port=18080 workers=1 conc=1
# usage: cmp label conns K loadgen_extra -- server command...
cmp() { local label=$1 conns=$2 K=$3 extra=$4; shift 5
  ("$@" >/dev/null 2>&1 & echo $! > /tmp/claude-1002/srv.pid); sleep 0.8
  local pid=$(cat /tmp/claude-1002/srv.pid)
  local out=$(./loadgen conns=$conns active=$conns threads=2 cpu0=8 seconds=3 pipeline=$K host=1 $extra | tail -1)
  echo "$(printf '%-34s' "$label") | $(echo $out | grep -o 'rps=[0-9]*') | $(echo $out | grep -o 'first_resp_us mean=[0-9]* max=[0-9]*') | $(echo $out | grep -o 'p50=[0-9]* p90=[0-9]* p99=[0-9]*') | $(echo $out | grep -o 'err connect=[0-9]* io=[0-9]* partial=[0-9]*')"
  kill $pid 2>/dev/null; wait $pid 2>/dev/null; sleep 0.6; }
