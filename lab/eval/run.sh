#!/bin/bash
# usage: bash run.sh <mode> [parallel]   modes: none denoise rnn100 rnn80 rnn60 dfn100 dfn80 dfn60
set -u
MODE=$1; N=${2:-20}
mkdir -p /var/spool/asterisk/eval/$MODE; chown asterisk: /var/spool/asterisk/eval /var/spool/asterisk/eval/$MODE
calls() { echo $(( $(asterisk -rx "core show channels count" | sed -n 2p | cut -d' ' -f1) / 2 )); }
t0=$(date +%s)
for f in /var/lib/asterisk/sounds/eval/*.ulaw; do
  id=$(basename $f .ulaw)
  [ -s /var/spool/asterisk/eval/$MODE/$id.wav ] && continue
  while [ "$(calls)" -ge "$N" ]; do sleep 0.2; done
  asterisk -rx "channel originate Local/$id@ev-$MODE extension $id@ev-play" >/dev/null
  sleep 0.05
done
until [ "$(calls)" = 0 ]; do sleep 1; done
echo "$MODE: $(ls /var/spool/asterisk/eval/$MODE | wc -l) files in $(( $(date +%s) - t0 )) s"
