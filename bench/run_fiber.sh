#!/bin/bash
# Group 1 matrix. Output: results/fiber.txt
B=$(dirname "$0")/c/fiber_bench
OUT=$(dirname "$0")/results/fiber.txt
: > "$OUT"
run() { echo "$*" >&2; timeout 300 "$B" "$@" >> "$OUT" 2>>"$OUT.err" || echo "FAILED: $*" >> "$OUT"; }
# stack size sweep, 100k fibers, 4 KB footprint
for m in pool poolguard each malloc; do
  for s in 16 64 256 1024; do run $m 100000 $s 4 20 0; done
done
# THP effect on pooled stacks
for s in 64 1024; do run pool 100000 $s 4 20 1; done
# footprint sweep (what an idle fiber actually dirties)
for t in 1 4 16 64; do run pool 100000 256 $t 20 0; done
# scale: 1M fibers
run pool 1000000 64 4 5 0
run poolguard 1000000 64 4 5 0
run malloc 1000000 64 4 5 0
