#!/usr/bin/env python3
"""Check libastrinoise_dfn.so against published DeepFilterNet quality, without Asterisk.

Runs every 16th VoiceBank+DEMAND test file through dfn-bench (pipe mode) at 48 kHz and scores
PESQ-WB at 16 kHz, the way DeepFilterNet's results are usually given. Also runs the same files
through phone band (48 -> 8 -> 48 kHz) and scores PESQ-NB. If you pass the path of DeepFilterNet's
own `deep-filter` tool (release binary), it scores that too, for comparison.

    python3 dfn-check.py /path/to/libastrinoise_dfn.so [/path/to/deep-filter]

We wrote this after our first build scored PESQ-WB 2.02 where the deep-filter tool scored 2.90:
the library's default stage thresholds skip the decoder on too many frames (see module/dfn/src/lib.rs).
"""
import csv, os, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, fftconvolve
from pesq import pesq

E = '/root/eval'
LIB = sys.argv[1]
TOOL = sys.argv[2] if len(sys.argv) > 2 else None
BENCH = os.environ.get('DFN_BENCH', E + '/dfn-bench')   # gcc -O2 -o /root/eval/dfn-bench dfn-bench.c -ldl -lpthread -lm
meta = list(csv.DictReader(open(E + '/map.csv')))[::16]


def run(x48):
    y = subprocess.run([BENCH, LIB, 'pipe'], input=np.clip(x48, -32768, 32767).astype(np.int16).tobytes(), capture_output=True).stdout
    return np.frombuffer(y, dtype=np.int16).astype(float)


def align(ref, out):
    c = fftconvolve(out, ref[::-1], mode='full')[len(ref) - 1:]
    lag = int(np.argmax(np.abs(c[:4000])))
    y = out[lag:lag + len(ref)]
    return np.concatenate([y, np.zeros(len(ref) - len(y))]) if len(y) < len(ref) else y


def wav(kind, name):
    return wavfile.read('%s/data/%s_testset_wav/%s.wav' % (E, kind, name))[1].astype(float)


r = {'noisy, wideband': [], 'astrinoise-dfn, wideband': [], 'noisy, phone band': [], 'astrinoise-dfn, phone band': []}
for m in meta:
    c, x = wav('clean', m['name']), wav('noisy', m['name'])
    c16, x16 = resample_poly(c, 1, 3), resample_poly(x, 1, 3)
    r['noisy, wideband'].append(pesq(16000, c16 / 32768, x16 / 32768, 'wb'))
    r['astrinoise-dfn, wideband'].append(pesq(16000, c16 / 32768, align(c16, resample_poly(run(x), 1, 3)) / 32768, 'wb'))
    c8, x8 = resample_poly(c, 1, 6), resample_poly(x, 1, 6)
    r['noisy, phone band'].append(pesq(8000, c8 / 32768, x8 / 32768, 'nb'))
    r['astrinoise-dfn, phone band'].append(pesq(8000, c8 / 32768, align(c8, resample_poly(run(resample_poly(x8, 6, 1)), 1, 6)) / 32768, 'nb'))
if TOOL:
    d = tempfile.mkdtemp()
    subprocess.run([TOOL, '-o', d] + ['%s/data/noisy_testset_wav/%s.wav' % (E, m['name']) for m in meta], capture_output=True)
    r['deep-filter tool, wideband'] = []
    for m in meta:
        c16 = resample_poly(wav('clean', m['name']), 1, 3)
        fs, y = wavfile.read('%s/%s.wav' % (d, m['name']))
        y = y.astype(float) * (32768 if y.dtype.kind == 'f' else 1)
        r['deep-filter tool, wideband'].append(pesq(16000, c16 / 32768, align(c16, resample_poly(y, 1, 3)) / 32768, 'wb'))
for k, v in r.items():
    print('%-28s PESQ %.3f  (%d files)' % (k, np.mean(v), len(v)))
