#!/bin/bash
# Group 2 matrix. Output: results/arena.txt  (each run: config line + report lines)
B=$(dirname "$0")/c/arena_bench
OUT=$(dirname "$0")/results/arena.txt
: > "$OUT"
run() { echo "## $*" >> "$OUT"; taskset -c 2 "$B" "$@" >> "$OUT" 2>&1 || echo "FAILED" >> "$OUT"; }
N=2000000
# A: chunk sizing (reserve layout, lowest, madvise, spare=1, slot 64)
for cfg in "8 8" "10 10" "12 12" "14 14" "15 15" "6 12" "6 14"; do
  set -- $cfg
  for w in fill churn mixed; do run layout=reserve first=$1 cap=$2 policy=lowest contract=madvise n=$N work=$w; done
  run layout=reserve first=$1 cap=$2 policy=lowest contract=madvise n=$N work=ramp pattern=group
  run layout=reserve first=$1 cap=$2 policy=lowest contract=madvise n=$N work=ramp pattern=random
done
# B: policy x contraction (256 KB fixed)
for pol in recent lowest; do for con in none madvise; do
  for pat in random fifo group; do run layout=reserve first=12 cap=12 policy=$pol contract=$con n=$N work=ramp pattern=$pat; done
  run layout=reserve first=12 cap=12 policy=$pol contract=$con n=$N work=mixed
done; done
# C: layout
for con in madvise munmap; do for w in fill churn mixed; do run layout=chunked first=12 cap=12 policy=lowest contract=$con n=$N work=$w; done
  run layout=chunked first=12 cap=12 policy=lowest contract=$con n=$N work=ramp pattern=group; done
# D: slot sizes at ~128 MB
for sz in "16 8000000" "256 500000" "1024 125000"; do set -- $sz
  for w in fill churn mixed; do run layout=reserve first=12 cap=12 policy=lowest contract=madvise slot=$1 n=$2 work=$w; done
done
# E: glibc malloc baselines
for sz in "16 8000000" "64 2000000" "256 500000" "1024 125000"; do set -- $sz
  for w in fill churn mixed; do run layout=malloc slot=$1 n=$2 work=$w; done
done
for pat in random fifo group; do run layout=malloc slot=64 n=$N work=ramp pattern=$pat; done
# F: handle forms
run layout=reserve slot=64 n=1000000 work=deref
echo "## DONE" >> "$OUT"
