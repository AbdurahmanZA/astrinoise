#!/usr/bin/env python3
"""VoiceBank+DEMAND test set -> telephony inputs.
clean 48k -> 8k reference wav (/root/eval/ref8k/<id>.wav); noisy 48k -> 8k -> G.711 u-law raw
(/var/lib/asterisk/sounds/eval/<id>.ulaw, played by Asterisk exactly like a phone leg).
ids are 1001.. (Asterisk extensions), map in /root/eval/map.csv with noise type and SNR."""
import audioop, csv, os, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
D = '/root/eval/data'
os.makedirs('/root/eval/ref8k', exist_ok=True)
os.makedirs('/var/lib/asterisk/sounds/eval', exist_ok=True)
log = {}
for line in open(D + '/log_testset.txt'):
    p = line.split()
    if len(p) == 3: log[p[0]] = (p[1], float(p[2]))
names = sorted(f[:-4] for f in os.listdir(D + '/clean_testset_wav') if f.endswith('.wav'))
def to8k(x):
    y = resample_poly(x.astype(np.float64), 1, 6)
    return np.clip(np.round(y), -32768, 32767).astype(np.int16)
with open('/root/eval/map.csv', 'w', newline='') as f:
    w = csv.writer(f); w.writerow(['id', 'name', 'noise', 'snr'])
    for i, n in enumerate(names):
        id_ = str(1001 + i)
        fs, c = wavfile.read('%s/clean_testset_wav/%s.wav' % (D, n)); assert fs == 48000
        fs, z = wavfile.read('%s/noisy_testset_wav/%s.wav' % (D, n)); assert fs == 48000
        wavfile.write('/root/eval/ref8k/%s.wav' % id_, 8000, to8k(c))
        open('/var/lib/asterisk/sounds/eval/%s.ulaw' % id_, 'wb').write(audioop.lin2ulaw(to8k(z).tobytes(), 2))
        w.writerow([id_, n] + list(log.get(n, ('?', -1))))
os.system('chown -R asterisk:asterisk /var/lib/asterisk/sounds/eval')
print(len(names), 'files')
