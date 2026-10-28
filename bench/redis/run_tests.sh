#!/bin/bash
# Every test must PASS. usage: ./run_tests.sh   (build first with ./build.sh). Port 16379; nothing else is touched.
cd "$(dirname "$0")"
SERVER=${SERVER:-./server}; PORT=16379; pass=0; fail=0; bad=""
run() { # test "server args"
  if ss -ltn 2>/dev/null | grep -q ":$PORT "; then echo "port $PORT is busy"; exit 2; fi
  $SERVER port=$PORT $2 >/tmp/claude-1002/mr_$1.log 2>&1 & local P=$!
  for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
  local o; o=$(timeout 60 python3 tests/redis_test.py $PORT $1 $P 2>&1 | tail -1)
  kill $P 2>/dev/null; wait $P 2>/dev/null
  if echo "$o" | grep -q -- "-> PASS"; then pass=$((pass+1)); echo "  $o   [$2]"; else fail=$((fail+1)); bad="$bad\n  $o   [$2]"; echo "  $o   [$2]"; fi; }
for w in 1 4; do for t in basic expiry pipeline fragment bigvalue limits pubsub; do run $t "workers=$w"; done; run order "workers=$w subqueue=4096"; done
run toolarge "workers=1 maxframe=1000 maxbulk=800"
run maxkeys "workers=1 maxkeys=4"
run maxconns "workers=1 maxconns=3 linger=300"
run xworker "workers=4 subqueue=4096"
run stress "workers=4"
run churn "workers=4"
run lag "workers=2 subqueue=4"
run lagnotice "workers=2 subqueue=4 lag=notice"
run reaper "workers=2 maxtopics=5 maxsubs=3"
run maxtopics "workers=1 maxtopics=3"
run shutdown "workers=4"
echo "tests: PASS=$pass FAIL=$fail"; [ -n "$bad" ] && echo -e "failures:$bad"
if [ -z "$NOCONTROLS" ] && [ "$SERVER" = "./server" ]; then
echo "negative controls:"
ctl() { # binary test "args"
  ./$1 port=$PORT $3 >/dev/null 2>&1 & local P=$!
  for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
  local o; o=$(timeout 60 python3 tests/redis_test.py $PORT $2 $P 2>&1 | tail -1); kill $P 2>/dev/null; wait $P 2>/dev/null
  if echo "$o" | grep -q -- "-> FAIL"; then echo "  control fails as intended: $1 [$3] $2"; else echo "  CONTROL DID NOT FAIL: $1 $2 -> $o"; fail=$((fail+1)); fi; }
ctl server_bf order "workers=1"
ctl server_br reaper "workers=2 maxtopics=5 maxsubs=3"
ctl server_bl toolarge "workers=1 maxframe=1000 maxbulk=800"
ctl server_bw pubsub "workers=1"
fi
[ $fail -eq 0 ]
