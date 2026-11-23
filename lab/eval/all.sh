#!/bin/bash
# Record every condition (lab only): no filter, stock DENOISE, ASTRINOISE at 100, 80 and 60 %.
cd "$(dirname "$0")"
asterisk -rx "astrinoise reset stats" >/dev/null
for m in none denoise rnn100 rnn80 rnn60; do bash run.sh "$m" 20; done
asterisk -rx "astrinoise show stats"
