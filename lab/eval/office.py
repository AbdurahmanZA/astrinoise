#!/usr/bin/env python3
"""Our own call-centre clips (fan, office, chatter, keyboard, voice; one talker over each noise):
DNSMOS P.835 of the raw recording and each engine's ASTRINOISE recording from lab/functional.sh (same Asterisk path)."""
import glob, sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import score
from scipy.io import wavfile
score.init()
print('%-9s %-8s %6s %6s %6s' % ('clip', 'path', 'SIG', 'BAK', 'OVRL'))
for c in ['fan', 'office', 'chatter', 'keyboard', 'voice']:
    for kind in ['raw', 'rnnoise', 'deepfilter']:
        g = sorted(glob.glob('/var/spool/asterisk/rnn/%s-%s-*.wav' % (c, kind)))
        if not g: continue
        p = g[-1]
        fs, x = wavfile.read(p)
        x = x.astype(np.float64) / 32768.0
        if fs != 8000:
            from scipy.signal import resample_poly
            x = resample_poly(x, 8000, fs)
        s, b, o = score.dnsmos(x)
        print('%-9s %-8s %6.2f %6.2f %6.2f' % (c, kind, s, b, o))
