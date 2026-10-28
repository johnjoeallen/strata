#!/bin/bash
# Builds the server (and the load generator once it exists). usage: ./build.sh [san]
cd "$(dirname "$0")"
G="gcc -O2 -Wall -Wextra -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-unused-function -pthread -I../c/http/runtime"
$G -o server server.c || { echo "build of server failed"; exit 1; }
[ -f loadgen_resp.c ] && { $G -o loadgen_resp loadgen_resp.c || exit 1; }
if [ "$1" = "san" ]; then   # sanitizer builds: server_asan (address + undefined), server_tsan (threads)
  $G -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -o server_asan server.c || exit 1
  $G -O1 -g -fsanitize=thread -o server_tsan server.c || exit 1
  $G -O1 -g -fsanitize=thread -DLOCK_SPIN=0 -o server_tsan_park server.c || exit 1
  $G -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DLOCK_SPIN=0 -o server_asan_park server.c || exit 1
fi
# negative controls: each breaks one property on purpose; the matching test must FAIL
$G -DBREAK_FAIR   -o server_bf server.c || exit 1      # connections never yield
$G -DBREAK_REAP   -o server_br server.c || exit 1      # ended connections leave their channels behind
$G -DBREAK_LIMIT  -o server_bl server.c || exit 1      # frame and bulk limits not enforced
$G -DBREAK_NOWAKE -o server_bw server.c || exit 1      # a push does not wake the subscriber
$G -DLOCK_SPIN=0    -o server_park     server.c || exit 1   # the lock parks at once (keeps the park/handoff path under test)
$G -DLOCK_SPIN=100  -o server_spin100  server.c || exit 1   # a shorter spin, for the lock comparison
echo "built: server"
