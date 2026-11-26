#!/bin/bash
# Poll the connection state every 0.4 s while one join runs, printing transitions
# only, so the latency from "join requested" to "connected" is visible.
PW="$1"
cd /tmp || exit 1
( printf '%s\n' "$PW" | ./a2join QCA-TEST > /tmp/a2join.out 2>&1; date +%s.%N > /tmp/a2join.done ) &
J=$!
prev=""
: > /tmp/trans.txt
for i in $(seq 1 45); do
  t=$(date +%s.%N)
  s=$(ioreg -l -w0 | grep -o 'Ath10kConnectionState" = "[^"]*"' | head -1 | sed 's/.*= "//;s/"//')
  d=$(ioreg -l -w0 | grep -o 'Ath10kAssocDefer" = "[^"]*"' | head -1 | sed 's/.*= "//;s/"//')
  if [ "$s|$d" != "$prev" ]; then echo "$t $s $d" >> /tmp/trans.txt; prev="$s|$d"; fi
  sleep 0.4
done
wait $J 2>/dev/null
cat /tmp/trans.txt
echo "--- joiner:"
grep -E '^(elapsed|ssid)' /tmp/a2join.out | head -3
