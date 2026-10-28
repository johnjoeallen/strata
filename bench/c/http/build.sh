#!/bin/bash
# Builds the server and every variant the tests use (opt-in features and negative controls). usage: ./build.sh
cd "$(dirname "$0")"
G="gcc -O2 -Wall -Wextra -Wno-implicit-fallthrough -Wno-unused-parameter -Wno-unused-function -pthread"
b() { local out=$1; shift; $G "$@" -o "$out" server.c || { echo "build of $out failed"; exit 1; }; }
b server                                                    # the server
b server_seq    -DSLEEP                                     # sequential baseline whose handlers can sleep on the timer
b server_b16    -DBATCH_N=16                                # opt-in response batching
b server_inj    -DBATCH_N=16 -DINJECT_FAIL_AT=3             # handler failure injected at request 3 (close-time flush test)
b server_inj_noflush -DBATCH_N=16 -DINJECT_FAIL_AT=3 -DNO_CLOSE_FLUSH
# negative controls: each breaks one property on purpose; the matching test must FAIL
b server_bo     -DBREAK_ORDER                               # responses taken in completion order, not request order
b server_bb     -DBREAK_BARRIER                             # unsafe requests overlap earlier ones
b server_bc     -DBREAK_COPY                                # spawned children read the connection's buffer instead of their own copy
b server_bg     -DBREAK_GROW                                # buffer growth forgets to copy the unconsumed bytes
b server_bl     -DBREAK_LIMIT                               # body limits not enforced
b server_bor    -DBREAK_OVERREAD                            # the body reader swallows the next request's bytes
b server_bsp    -DBREAK_SPILL                               # the upload sink drops its first write
b server_bf     -DBREAK_FAIR                                # body reads never yield
echo "built: $(ls server server_* 2>/dev/null | grep -v '\.' | tr '\n' ' ')"
