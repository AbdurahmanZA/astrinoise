#!/usr/bin/env bash
# Lab only: reinstall func_astrinoise.so, then record every clip once cleaned and once raw (SECS each).
# usage (lab box, as root):  SO=/path/to/func_astrinoise.so bash functional.sh [secs]   -> /var/spool/asterisk/rnn/*.wav
# ENGINE=deepfilter records with DeepFilterNet (files <clip>-deepfilter-*.wav); the other engine's files stay.
# Needs lab/extensions.conf loaded and the clips in /var/lib/asterisk/sounds/rnnlab/ (see README).
set -euo pipefail
SECS=${1:-20}
M=${M:-/usr/lib/x86_64-linux-gnu/asterisk/modules}   # Asterisk built from source: /usr/lib/asterisk/modules
calls() { asterisk -rx "core show channels count" | head -1 | cut -d' ' -f1; }
SO=${SO:-$(dirname "$0")/../module/func_astrinoise.so}
mkdir -p /var/spool/asterisk/rnn && chown asterisk: /var/spool/asterisk/rnn
install -m 644 "$SO" "$M/func_astrinoise.so"
asterisk -rx "module unload func_astrinoise.so" >/dev/null || true
asterisk -rx "module load func_astrinoise.so" | tail -1
rm -f /var/spool/asterisk/rnn/*-${ENGINE:-rnnoise}-*.wav /var/spool/asterisk/rnn/*-raw-*.wav
asterisk -rx "dialplan reload" >/dev/null
asterisk -rx "dialplan set global SECS $SECS" >/dev/null
asterisk -rx "dialplan set global ENGINE ${ENGINE:-rnnoise}" >/dev/null
asterisk -rx "astrinoise reset stats" >/dev/null
for c in fan keyboard chatter office voice; do
  asterisk -rx "channel originate Local/$c@rnn-test extension $c@rnn-play" >/dev/null
  asterisk -rx "channel originate Local/$c@raw-test extension $c@rnn-play" >/dev/null
done
until [ "$(calls)" = 0 ]; do sleep 2; done
asterisk -rx "astrinoise show stats"
