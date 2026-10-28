#!/bin/bash
# usage: run_scale.sh <server-cmd...>  with env N ACTIVE WORKERS SECS
# Starts the server, ramps N connections, samples memory at READY, runs the active phase, prints one summary.
N=${N:-10000}; ACTIVE=${ACTIVE:-64}; SECS=${SECS:-5}; PORT=${PORT:-18080}
SRCIPS=$(( (N + 11999) / 12000 )); [ $SRCIPS -lt 1 ] && SRCIPS=1
ulimit -n 524288
for i in $(seq 1 90); do [ $(cat /proc/sys/net/netfilter/nf_conntrack_count) -lt 3000 ] && break; sleep 1; done
"$@" 2>/dev/null &
SP=$!
sleep 1
rss() { awk '/VmRSS/{print $2}' /proc/$1/status; }
avail() { awk '/MemAvailable/{print $2}' /proc/meminfo; }
SR0=$(rss $SP); A0=$(avail)
OUT=$(mktemp)
./loadgen conns=$N active=$ACTIVE threads=${CT:-4} cpu0=${CPU0:-8} srcips=$SRCIPS seconds=$SECS port=$PORT hold=1 > $OUT 2>&1 &
LP=$!
while ! grep -q '^HOLD\|^conns=' $OUT 2>/dev/null; do sleep 0.2; if ! kill -0 $LP 2>/dev/null; then break; fi; done
# at HOLD the measurement is finished; sample memory while everything is still connected
SR1=$(rss $SP); CR1=$(rss $LP); A1=$(avail)
wait $LP
T=$(ls /proc/$SP/task | wc -l)
kill $SP 2>/dev/null; wait $SP 2>/dev/null
READY=$(grep -o 'ready=[0-9]*' $OUT | head -1 | cut -d= -f2)
echo "N=$N srcips=$SRCIPS | server_rss_delta=$(( (SR1 - SR0) / 1024 )) MB ($(( (SR1 - SR0) * 1024 / (READY>0?READY:1) )) B/conn) | client_rss=$(( CR1 / 1024 )) MB | system_delta=$(( (A0 - A1) / 1024 )) MB | kernel_est=$(( ( (A0 - A1) - (SR1 - SR0) - CR1 ) * 1024 / (READY>0?READY:1) / 2 )) B/socket"
grep -v '^READY\|^HOLD' $OUT
rm -f $OUT
