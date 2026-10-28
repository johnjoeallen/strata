#!/bin/bash
# Option C matrix. Output: results/frames.txt
B=$(dirname "$0")/c/frames_bench
OUT=$(dirname "$0")/results/frames.txt
: > "$OUT"
run() { echo "$*" >&2; taskset -c 2 "$B" "$@" >> "$OUT" 2>>"$OUT.err" || echo "FAILED: $*" >> "$OUT"; }
for foot in 0 1024 4096 16384; do
  for depth in 0 4 16; do
    for res in nested direct; do run 100000 $foot $depth $res arena 50; done
  done
done
# allocator comparison and scale
for foot in 0 4096; do run 100000 $foot 4 direct malloc 50; done
run 1000000 1024 4 direct arena 10
run 1000000 0 4 direct arena 10
