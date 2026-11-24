"""Measure an engine's output delay at 48 kHz: run noisy VoiceBank files through a stdin/stdout
filter and cross-correlate.  python3 lag.py                          -> RNNoise (/root/eval/lag)
                             python3 lag.py ./dfn-bench LIB.so pipe  -> DeepFilterNet"""
import subprocess, sys, numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve
cmd = sys.argv[1:] or ['/root/eval/lag']
for name in ['p232_001', 'p257_100', 'p232_200']:
    fs, x = wavfile.read('/root/eval/data/noisy_testset_wav/%s.wav' % name)
    fs, c = wavfile.read('/root/eval/data/clean_testset_wav/%s.wav' % name)
    y = np.frombuffer(subprocess.run(cmd, input=x.astype(np.int16).tobytes(), capture_output=True).stdout, dtype=np.int16).astype(float)
    def lag(a, b):  # how many samples b lags a
        cc = fftconvolve(b, a[::-1], mode='full')[len(a) - 1:len(a) - 1 + 6000]
        return int(np.argmax(cc))
    print(name, 'output lags noisy input by', lag(x.astype(float), y), 'samples; lags clean by', lag(c.astype(float), y), '(480 = one 10 ms frame)')
