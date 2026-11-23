#!/usr/bin/env bash
# Lab only: Asterisk memory after many short calls, without and with ASTRINOISE (same call pattern).
#   bash leak.sh 400 -> RSS growth per 100 calls for load-raw vs load-test
set -uo pipefail
TOTAL=${1:-400}
calls() { echo $(( $(asterisk -rx "core show channels count" | sed -n 2p | cut -d' ' -f1) / 2 )); }
rss() { awk '/VmRSS/{print $2}' /proc/$(pidof asterisk)/status; }
asterisk -rx "dialplan set global SECS 2" >/dev/null
for ctx in load-raw load-test load-raw load-test; do
  until [ "$(calls)" = 0 ]; do sleep 1; done; sleep 2
  r0=$(rss)
  for i in $(seq 1 "$TOTAL"); do
    asterisk -rx "channel originate Local/office@$ctx extension office@rnn-play" >/dev/null
    while [ "$(calls)" -ge 10 ]; do sleep 0.2; done
  done
  until [ "$(calls)" = 0 ]; do sleep 1; done; sleep 3
  r1=$(rss)
  echo "$ctx: $TOTAL calls, RSS $r0 -> $r1 kB (+$(( (r1 - r0) * 100 / TOTAL )) kB per 100 calls)"
done
asterisk -rx "astrinoise show stats"
