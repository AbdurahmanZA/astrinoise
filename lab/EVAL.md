# Quality evaluation (2026-11-23)

We sent the full VoiceBank+DEMAND test set through real Asterisk calls as phone audio and scored
every condition against the clean recordings.

## Method

- **Test set:** VoiceBank+DEMAND, all 824 test utterances (speakers p232 and p257). Five noise
  types (bus, cafe, living room, office, public square) at 2.5, 7.5, 12.5 and 17.5 dB SNR.
  Source: https://datashare.ed.ac.uk/handle/10283/2791 (`clean_testset_wav.zip`,
  `noisy_testset_wav.zip`, `logfiles.zip`).
- **Phone audio:** `prep.py` downsamples each noisy file from 48 to 8 kHz and encodes it as G.711
  u-law, the same as a desk phone sends. The clean file goes to 8 kHz as the reference.
- **Real call path:** `run.sh` makes one Local-channel call per file in Asterisk 22.7.0
  (`eval-dialplan.conf`). One leg plays the u-law file; the other applies the filter to what it
  receives and records it. That leg is set up the same way as on a PBX: `ASTRINOISE(rx)` on the
  agent's channel. We ran 20 calls at a time.
- **Conditions:**
  - no filter
  - Asterisk's stock `DENOISE()` (func_speex)
  - `ASTRINOISE` at 100, 80 and 60 % strength
- **Scores** (`score.py`):
  - **PESQ** (ITU-T P.862, narrowband): overall speech quality, 1-4.5.
  - **STOI** and **ESTOI**: intelligibility, 0-1.
  - **SI-SDR** in dB: how close the waveform is to the clean speech.
  - **DNSMOS P.835** (Microsoft's model): predicted listener scores for the voice (SIG), the
    background (BAK) and overall (OVRL), 1-5.

  Before the first four scores, `score.py` lines each recording up with the reference, so the
  filter's fixed delay doesn't count as damage. DNSMOS takes 16 kHz audio, so we upsample the
  8 kHz output and score the first 9 s window (the script tiles short clips; later windows repeat
  the same audio).
- **Our own clips:** `office.py` gives DNSMOS for the five call-centre clips from
  `lab/functional.sh` (one talker over a fan, a busy office, colleagues talking, a keyboard, and
  alone).

To repeat it: install `pesq`, `pystoi` and `onnxruntime`, put Microsoft's `sig_bak_ovr.onnx`
(DNS-Challenge repo, `DNSMOS/DNSMOS/`) in `/root/eval/data`, then run `prep.py`, `all.sh`,
`score.py` and `summary.py`. `scores.csv` holds every per-file score from this run, and
`map.csv` maps file ids to noise and SNR.

## Results: all 824 files

| Condition | PESQ | STOI | ESTOI | SI-SDR | SIG | BAK | OVRL |
|---|---|---|---|---|---|---|---|
| Clean reference | 4.55 | 1.000 | 1.000 | | 3.50 | 4.05 | 3.21 |
| No filter | 2.90 | 0.920 | 0.783 | 8.4 | 3.35 | 3.15 | 2.71 |
| Stock DENOISE | 2.97 | 0.914 | 0.776 | 9.5 | 3.31 | 3.28 | 2.72 |
| ASTRINOISE 60 % | 3.16 | 0.930 | 0.812 | 12.9 | 3.36 | 3.54 | 2.87 |
| **ASTRINOISE 80 %** | **3.31** | **0.930** | **0.821** | **13.7** | **3.36** | 3.73 | 2.96 |
| ASTRINOISE 100 % | 3.22 | 0.909 | 0.814 | 13.2 | 3.29 | **3.99** | **3.00** |

- **80 % is the best setting for the voice.** It gives the highest PESQ (+0.41), intelligibility
  (STOI +0.010, ESTOI +0.038) and SI-SDR (+5.3 dB), and the voice score stays the same as no
  filter (SIG 3.36 vs 3.35).
- **100 % removes the most background.** BAK goes from 3.15 to 3.99, which is almost the clean
  recording's 4.05. The cost is a little voice quality (SIG -0.06) and intelligibility
  (STOI -0.011).
- **Stock DENOISE barely helps** (PESQ +0.07, BAK +0.13).
- **For comparison:** published wideband results for RNNoise on this set run from 1.97 (noisy) to
  2.29-2.33. A 2026 paper (since withdrawn) measured RNNoise 0.2 at narrowband: PESQ 1.85 to 2.16.
  Our ASTRINOISE 100 % gain of +0.32 matches both. Running RNNoise inside Asterisk, with the 8 to
  48 kHz resampling, loses nothing against the reference RNNoise.

## By noise level (SNR)

| SNR | No filter PESQ / BAK | 80 % PESQ / BAK | 100 % PESQ / BAK |
|---|---|---|---|
| 2.5 dB (very noisy) | 2.34 / 2.51 | 2.77 / 3.42 | 2.68 / 3.95 |
| 7.5 dB | 2.75 / 3.08 | 3.20 / 3.71 | 3.13 / 3.99 |
| 12.5 dB | 3.09 / 3.39 | 3.50 / 3.84 | 3.39 / 4.00 |
| 17.5 dB (mild) | 3.44 / 3.61 | 3.79 / 3.95 | 3.70 / 4.02 |

The filter helps at every noise level. 80 % wins on PESQ at all four. 100 % takes the background
nearer the clean level, most at 2.5 dB.

## By noise type (PESQ / BAK)

| Noise | No filter | 80 % | 100 % |
|---|---|---|---|
| Bus | 3.37 / 3.53 | 3.74 / 3.94 | 3.64 / 4.03 |
| Cafe (chatter, dishes) | 2.35 / 2.59 | 2.76 / 3.41 | 2.63 / 3.96 |
| Living room | 2.59 / 2.79 | 3.02 / 3.55 | 2.95 / 3.94 |
| Office | 3.46 / 3.76 | 3.84 / 4.03 | 3.79 / 4.04 |
| Public square | 2.75 / 3.08 | 3.22 / 3.71 | 3.12 / 3.97 |

Cafe noise is the closest to colleagues talking. It is also where 100 % does most for the
background (BAK 3.96 against 3.41 at 80 %).

## Our call-centre clips (DNSMOS, ASTRINOISE 100 %)

| Clip | SIG raw / cleaned | BAK raw / cleaned | OVRL raw / cleaned |
|---|---|---|---|
| Fan + hum | 3.65 / 3.42 | 3.00 / 4.11 | 2.81 / 3.20 |
| Busy office | 3.61 / 3.13 | 2.27 / 3.75 | 2.36 / 2.77 |
| Colleagues talking | 3.58 / 3.14 | 2.58 / 3.82 | 2.53 / 2.81 |
| Keyboard | 3.49 / 3.36 | 2.93 / 4.04 | 2.65 / 3.10 |
| Voice alone | 3.59 / 3.57 | 4.17 / 4.15 | 3.36 / 3.35 |

- Overall improves on every noisy clip.
- A clean voice comes through unchanged (3.36 vs 3.35), so the filter doesn't damage speech
  without noise.
- In a busy office and with colleagues talking, the voice score drops by about 0.45 at 100 %.
  This matches the consonant loss other RNNoise users report.

## Bug found by this test: strength below 100

The first run at 60 % scored worse than no filter (PESQ 2.59, SI-SDR 2.0 dB). Strength mixes the
original audio back in, and the module lined that original up with RNNoise's output one 10 ms
frame back. We measured RNNoise 0.2's output lag with `lag.c` / `lag.py`: 960 samples at 48 kHz,
which is **two** frames. The mix was 10 ms out of step and comb-filtered the voice.

The fix keeps the last two input frames (`RNN_LAG 2` in `func_astrinoise.c`). After the fix, 60 %
scores 3.16 and 80 % scores 3.31. 100 % never used the mix, so earlier 100 % results stand. The
fixed delay is 20 ms FIFO + 20 ms RNNoise + resamplers, about 50 ms, the same as we measured
before.

A PBX running a build from before this fix must use strength 100 (or `on`) until it gets the new
build.

## CPU during the test

| Run | Frames | Avg per 20 ms | Slowest | Shortfalls |
|---|---|---|---|---|
| ASTRINOISE 100 % and 60 % | 208,002 | 0.32 ms | 7.2 ms | 0 |
| ASTRINOISE 60 % and 80 % (fixed build) | 208,002 | 0.32 ms | 15.6 ms | 0 |

Both runs had 20 calls at a time, on a container sharing a busy host.

## Limits of this test

- PESQ P.862 is officially superseded by POLQA (P.863), which needs a commercial licence.
  PESQ-NB is still the usual open measure for 8 kHz.
- DNSMOS was trained on 16 kHz audio, so 8 kHz scores run low (the clean reference gets 3.21
  overall). Use it to compare conditions, not as an absolute score.
- DEMAND noise is not a call centre. Cafe and office are the closest. Our own clips cover fan,
  office, colleagues and keyboard, but have no clean reference.
- No listening test with people, and no speech-recognition word error rate yet. Both would
  measure the consonant question directly.

## What to do with it

1. Set strength **80** as the default once the fixed build is on a PBX. It is the best balance
   of voice, intelligibility and background. Use 100 only where the background matters more
   than a slightly thinner voice.
2. Ship the fixed build to every PBX that has the old one before anyone uses strength 1-99.
3. Next quality step (from the research): retrain RNNoise on phone-band, G.711-coded speech. It
   keeps the same CPU and code, and only the weights file changes. GTCRN or FastEnhancer-Tiny
   through ONNX Runtime are the next candidates if retraining is not enough.
