#!/bin/bash
# Record every condition (lab only): no filter, stock DENOISE, ASTRINOISE with RNNoise and with
# DeepFilterNet at 100, 80 and 60 %.   bash all.sh [modes...]   e.g. bash all.sh dfn100 dfn80 dfn60
cd "$(dirname "$0")"
asterisk -rx "astrinoise reset stats" >/dev/null
for m in ${@:-none denoise rnn100 rnn80 rnn60 dfn100 dfn80 dfn60}; do bash run.sh "$m" 20; done
asterisk -rx "astrinoise show stats"
