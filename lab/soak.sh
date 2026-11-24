#!/usr/bin/env bash
# Lab only: keep N cleaned calls running for MINUTES (each call CALLSECS long, replaced as it ends),
# logging ASTRINOISE timing, Asterisk CPU and memory every 30 s.   bash soak.sh 30 15 60 > soak.log
# ENGINE=deepfilter bash soak.sh ...   tests DeepFilterNet instead of RNNoise.
set -uo pipefail
N=${1:-30}; MINUTES=${2:-15}; CALLSECS=${3:-60}
CLIPS=(office fan chatter keyboard voice)
# each test call is a Local channel pair = 2 "active calls" in Asterisk's count
calls() { echo $(( $(asterisk -rx "core show channels count" | sed -n 2p | cut -d' ' -f1) / 2 )); }
until [ "$(calls)" = 0 ]; do sleep 1; done   # start from an idle box
asterisk -rx "dialplan set global ENGINE ${ENGINE:-rnnoise}" >/dev/null
asterisk -rx "dialplan set global SECS $CALLSECS" >/dev/null
asterisk -rx "astrinoise reset stats" >/dev/null
PID=$(pidof asterisk)
end=$(( $(date +%s) + MINUTES * 60 )); i=0; next_log=0
t0=$(awk '{print $14+$15}' /proc/$PID/stat); s0=$(date +%s)
echo "start $(date +%T) N=$N minutes=$MINUTES rss_kb=$(awk '/VmRSS/{print $2}' /proc/$PID/status)"
while [ "$(date +%s)" -lt "$end" ]; do
  have=$(calls); have=${have:-0}
  while [ "$have" -lt "$N" ]; do
    c=${CLIPS[$((i % 5))]}; i=$((i + 1))
    asterisk -rx "channel originate Local/$c@load-test extension $c@rnn-play" >/dev/null
    have=$((have + 1)); sleep 0.2
  done
  now=$(date +%s)
  if [ "$now" -ge "$next_log" ]; then
    t1=$(awk '{print $14+$15}' /proc/$PID/stat)
    echo "$(date +%T) calls=$(calls) started=$i asterisk_cpu=$(( (t1 - t0) / (now - s0 + 1) ))% rss_kb=$(awk '/VmRSS/{print $2}' /proc/$PID/status) $(asterisk -rx 'astrinoise show stats')"
    t0=$t1; s0=$now; next_log=$((now + 30))
  fi
  sleep 1
done
echo "end $(date +%T)"; asterisk -rx "astrinoise show stats"
