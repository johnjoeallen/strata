#!/bin/bash
# Lazy vs eager vs embedded frames. Output: results/lazy.txt (checksums verified by check below)
B=$(dirname "$0")/c/lazy_bench
OUT=$(dirname "$0")/results/lazy.txt
: > "$OUT"
N=2000; R=2000
for W in 8 64; do
  for D in 2 4 8; do
    for p in 0 0.01 0.1 1; do
      for v in eager embed lazy; do taskset -c 2 "$B" $v $N $R $D $p $W >> "$OUT"; done
    done
    taskset -c 2 "$B" plain $N $R $D 0 $W >> "$OUT"
  done
done
# every variant must agree on the checksum for the same (D,p,W)
awk '{k=$2" "$3" "$4; c=$NF; if (k in s && s[k]!=c) {print "CHECKSUM MISMATCH: "k; bad=1} s[k]=c} END{if(!bad) print "all checksums agree"}' "$OUT"
