#!/usr/bin/env python3
"""Score every condition against the clean 8 kHz reference.
Intrusive: PESQ P.862 narrowband, STOI, ESTOI, SI-SDR (output aligned to the reference by
cross-correlation, so the filter's fixed delay is not counted as damage).
Non-intrusive: DNSMOS P.835 SIG/BAK/OVRL (Microsoft sig_bak_ovr.onnx, same tiling and polyfit as
DNS-Challenge dnsmos_local.py, but only the first 9.01 s window: short clips are tiled, so later
windows repeat the same audio; 8 kHz audio upsampled to 16 kHz)."""
import os
for v in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"): os.environ[v] = "1"
import audioop, csv, sys
from multiprocessing import Pool
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, fftconvolve
from pesq import pesq
from pystoi import stoi
import onnxruntime as ort

E = '/root/eval'
MODES = sys.argv[1:] or ['input', 'none', 'denoise', 'rnn100', 'rnn80', 'rnn60', 'dfn100', 'dfn80', 'dfn60', 'clean']
sess = None
P_SIG = np.poly1d([-0.08397278, 1.22083953, 0.0052439])
P_BAK = np.poly1d([-0.13166888, 1.60915514, -0.39604546])
P_OVR = np.poly1d([-0.06766283, 1.11546468, 0.04602535])

def init():
    global sess
    o = ort.SessionOptions(); o.intra_op_num_threads = 1; o.inter_op_num_threads = 1
    sess = ort.InferenceSession(E + '/data/sig_bak_ovr.onnx', o, providers=['CPUExecutionProvider'])

def dnsmos(x8):
    a = resample_poly(x8, 2, 1).astype(np.float64)
    L = int(9.01 * 16000)
    while len(a) < L:
        a = np.append(a, a)
    hops = 1   # see docstring
    r = []
    for i in range(hops):
        seg = a[i * 16000: i * 16000 + L]
        if len(seg) < L: continue
        s, b, o = sess.run(None, {'input_1': seg.astype('float32')[np.newaxis, :]})[0][0]
        r.append((P_SIG(s), P_BAK(b), P_OVR(o)))
    return np.mean(r, axis=0)

def read(p):
    fs, x = wavfile.read(p); assert fs == 8000, (p, fs)
    return x.astype(np.float64) / 32768.0

def align(ref, out):
    c = fftconvolve(out, ref[::-1], mode='full')[len(ref) - 1:]    # lag >= 0: out is late
    lag = int(np.argmax(np.abs(c[:8000]))) if len(c) else 0
    y = out[lag:lag + len(ref)]
    if len(y) < len(ref): y = np.concatenate([y, np.zeros(len(ref) - len(y))])
    return y, lag

def sisdr(ref, est):
    ref = ref - ref.mean(); est = est - est.mean()
    t = np.dot(est, ref) / (np.dot(ref, ref) + 1e-12) * ref
    return 10 * np.log10((np.dot(t, t) + 1e-12) / (np.dot(est - t, est - t) + 1e-12))

def one(job):
    id_, mode = job
    ref = read('%s/ref8k/%s.wav' % (E, id_))
    if mode == 'clean':
        y, lag = ref, 0
    elif mode == 'input':
        u = open('/var/lib/asterisk/sounds/eval/%s.ulaw' % id_, 'rb').read()
        y, lag = np.frombuffer(audioop.ulaw2lin(u, 2), dtype=np.int16).astype(np.float64) / 32768.0, 0
        y = y[:len(ref)]
    else:
        p = '/var/spool/asterisk/eval/%s/%s.wav' % (mode, id_)
        if not os.path.exists(p): return None
        y, lag = align(ref, read(p))
    try: pq = pesq(8000, ref, y, 'nb')
    except Exception: pq = float('nan')
    st = stoi(ref, y, 8000, extended=False); es = stoi(ref, y, 8000, extended=True)
    s, b, o = dnsmos(y)
    return [id_, mode, round(pq, 3), round(st, 4), round(es, 4), round(sisdr(ref, y), 2), round(s, 3), round(b, 3), round(o, 3), lag]

if __name__ == '__main__':
    meta = {r['id']: r for r in csv.DictReader(open(E + '/map.csv'))}
    jobs = [(i, m) for m in MODES for i in sorted(meta)]
    rows = []
    with Pool(4, initializer=init) as pool:
        for n, r in enumerate(pool.imap(one, jobs, chunksize=8), 1):
            if r: rows.append(r)
            if n % 250 == 0: print('%d/%d' % (n, len(jobs)), flush=True)
    out = E + '/scores.csv'
    old = [r for r in csv.reader(open(out))][1:] if os.path.exists(out) else []
    old = [r for r in old if r[1] not in MODES]
    with open(out, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['id', 'mode', 'pesq_nb', 'stoi', 'estoi', 'sisdr', 'sig', 'bak', 'ovrl', 'lag'])
        w.writerows(old + rows)
    print('scored', len(rows), 'rows ->', out)
