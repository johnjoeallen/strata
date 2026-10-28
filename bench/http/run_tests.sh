#!/bin/bash
# The whole correctness suite: every test must PASS and every negative control must FAIL. usage: ./run_tests.sh   (build first with ./build.sh)
cd "$(dirname "$0")"
FRAG=${FRAG:-}            # optional path to the fragmented-input script; skipped when empty
pass=0; fail=0; bad=""
chk() { if echo "$1" | grep -q -- "-> PASS"; then pass=$((pass+1)); else fail=$((fail+1)); bad="$bad\n  $2: $1"; fi; }
run() { # binary args script test [extra]
  ./$1 port=18080 workers=1 cpu0=0 $2 >/dev/null 2>&1 & local P=$!; sleep 0.4
  local o=$(python3 $3 18080 $4 $P $5 2>&1 | tail -1); kill $P; wait $P 2>/dev/null; echo "$o"; }
tc() { chk "$(run $1 "$2" conc_test.py $3)" "$1 [$2] $3"; }
tb() { chk "$(run $1 "$2" body_test.py $3)" "$1 [$2] $3"; }
tl() { chk "$(run $1 "$2" linger_test.py $3 $4)" "$1 [$2] $3"; }
ctl() { local o=$(run $1 "$2" $4 $3 $5); if echo "$o" | grep -q -- "-> FAIL\|Error\|Traceback"; then echo "  control fails as intended: $1 [$2] $3"; else echo "  CONTROL DID NOT FAIL: $1 $3 -> $o"; fail=$((fail+1)); fi; }
for inl in 1 0; do for t in order barrier close boom copy bigheader toolarge; do tc server "conc=1 inline=$inl" $t; done; done
tc server "conc=1 depth=4" depth; tc server "conc=0" boom; tc server "conc=0" bigheader; tc server "conc=1 maxhead=2048" maxhead
for t in sizes defaultcap pipelined malformed expect eofmid; do tb server "conc=1" $t; done; tb server "conc=1 maxbody=100000 uploadmax=500000" limits; tb server "conc=1" large
for t in answer413 answer431 answer400 closeheader; do tl server "conc=1 maxbody=100000" $t; done; tl server "conc=1 uploadmax=100000" answer413-upload; tl server "conc=0" answer431; tl server "conc=0" closeheader
tl server "conc=1 linger=500 lingerbytes=10000000000000" forever 0.5; tl server "conc=1 linger=5000 lingerbytes=4000000" bytecap 4000000; tl server "conc=1 linger=1000" early 1; tl server "conc=0 linger=1000" early 1
if [ -n "$FRAG" ]; then for seed in 1 2; do for v in "server conc=0" "server conc=1" "server_b16 conc=0" "server_seq conc=0"; do set -- $v; ./$1 port=18080 workers=1 cpu0=0 $2 >/dev/null 2>&1 & P=$!; sleep 0.4; o=$(python3 $FRAG 18080 5000 $seed | tail -1); kill $P; wait $P 2>/dev/null; chk "$o" "frag $v seed=$seed"; done; done; fi
./server_inj port=18080 workers=1 cpu0=0 >/dev/null 2>&1 & P=$!; sleep 0.4; o=$(python3 flush_test.py 18080 5); kill $P; wait $P 2>/dev/null
echo "$o" | grep -q "responses=2" && pass=$((pass+1)) || { fail=$((fail+1)); bad="$bad\n  close-time flush: $o"; }
echo "tests: PASS=$pass FAIL=$fail"; [ -n "$bad" ] && echo -e "failures:$bad"
echo "negative controls:"
ctl server_bo "conc=1" order conc_test.py; ctl server_bb "conc=1" barrier conc_test.py; ctl server_bc "conc=1" copy conc_test.py; ctl server_bg "conc=1" bigheader conc_test.py
ctl server_bl "conc=1 maxbody=100000 uploadmax=500000" limits body_test.py; ctl server_bor "conc=1" pipelined body_test.py; ctl server_bsp "conc=1" sizes body_test.py
ctl server "conc=1 maxbody=100000 linger=0" answer413 linger_test.py; ctl server "conc=1 linger=0" answer431 linger_test.py; ctl server "conc=0 linger=0" answer431 linger_test.py
./server_inj_noflush port=18080 workers=1 cpu0=0 >/dev/null 2>&1 & P=$!; sleep 0.4; o=$(python3 flush_test.py 18080 5); kill $P; wait $P 2>/dev/null
echo "$o" | grep -q "responses=0" && echo "  control fails as intended: server_inj_noflush (held responses lost without the close-time flush)" || { echo "  CONTROL DID NOT FAIL: $o"; fail=$((fail+1)); }
[ $fail -eq 0 ]
