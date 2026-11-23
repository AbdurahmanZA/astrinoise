#!/usr/bin/env python3
import csv, sys
import numpy as np
E = '/root/eval'
meta = {r['id']: r for r in csv.DictReader(open(E + '/map.csv'))}
rows = list(csv.DictReader(open(E + '/scores.csv')))
ORDER = ['clean', 'input', 'none', 'denoise', 'rnn60', 'rnn80', 'rnn100']
K = ['pesq_nb', 'stoi', 'estoi', 'sisdr', 'sig', 'bak', 'ovrl']
def table(group):
    keys = sorted({group(r) for r in rows}, key=lambda g: (g[0], ORDER.index(g[1]) if g[1] in ORDER else 9))
    print('%-22s %5s ' % ('group', 'n') + ' '.join('%8s' % k for k in K))
    for g in keys:
        sel = [r for r in rows if group(r) == g]
        vals = [np.nanmean([float(r[k]) for r in sel]) for k in K]
        print('%-22s %5d ' % ('/'.join(map(str, g)), len(sel)) + ' '.join('%8.3f' % v for v in vals))
    print()
table(lambda r: ('all', r['mode']))
table(lambda r: (meta[r['id']]['noise'], r['mode']))
table(lambda r: ('snr%04.1f' % float(meta[r['id']]['snr']), r['mode']))
lags = [int(r['lag']) for r in rows if r['mode'] in ('none', 'rnn100')]
print('lags (samples) none/rnn100 median:', {m: int(np.median([int(r['lag']) for r in rows if r['mode'] == m])) for m in ('none', 'denoise', 'rnn100', 'rnn60') if any(r['mode'] == m for r in rows)})
