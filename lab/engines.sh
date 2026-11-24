#!/usr/bin/env bash
# Lab only: check the engine switch end to end. Needs lab/extensions.conf loaded and the clips.
#   bash engines.sh     -> one line per check, then astrinoise show stats
set -uo pipefail
calls() { echo $(( $(asterisk -rx "core show channels count" | sed -n 2p | cut -d' ' -f1) / 2 )); }
db() { asterisk -rx "database get $1 $2" | sed -n 's/^Value: //p'; }
asterisk -rx "database deltree astrinoise-lab" >/dev/null
asterisk -rx "database deltree rnnoise-test" >/dev/null
asterisk -rx "astrinoise reset stats" >/dev/null
for e in rnnoise deepfilter; do
  asterisk -rx "channel originate Local/$e@engine-check extension voice@rnn-play" >/dev/null
done
sleep 1; until [ "$(calls)" = 0 ]; do sleep 1; done
for e in rnnoise deepfilter; do
  echo "start $e -> [$(db astrinoise-lab $e)]  switched -> [$(db astrinoise-lab $e-switched)]  off -> [$(db astrinoise-lab $e-off)]"
done
# the FreePBX hook (needs dialplan/extensions_override_freepbx.conf loaded): engine from the test call
for t in all-1001 all-1001-n-deepfilter all-1001-n-rnnoise all-1001-e-deepfilter off-1001-n-deepfilter; do
  asterisk -rx "channel originate Local/$t@rnnoise-hook-test application Wait 1" >/dev/null
done
sleep 2
for t in all-1001 all-1001-n-deepfilter all-1001-n-rnnoise all-1001-e-deepfilter off-1001-n-deepfilter; do
  echo "hook $t -> strength [$(db rnnoise-test $t)] engine [$(db rnnoise-test $t-engine)]"
done
asterisk -rx "astrinoise show stats"
