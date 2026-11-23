# Lab results (2026-11-23)

Lab box: an LXC container on Proxmox, Debian 12, Sangoma `asterisk22` + `asterisk22-devel`
**22.11.0-1.sng12** (the FreePBX 17 packages), Intel i5-12500 with 4 cores given to the container.
Module built with `module/build.sh` defaults: speexdsp resampler quality 8, skip_zeros on the
downsampler, 20 ms FIFO.

## Timing and stability
| Test | Calls at once | Frames (20 ms) | avg per frame | slowest frame | > 2 ms | > 5 ms | underruns | overflows |
|---|---|---|---|---|---|---|---|---|
| functional.sh (5 clips) | 5 | 5,005 | 0.31 ms | 1.6 ms | 0 | 0 | 0 | 0 |
| soak 15 min (counting bug: 30 then 20) | 20-30 | 892,966 | 0.29 ms | 4.8 ms | 17 | 0 | 0 | 0 |
| **soak 10 min, 30 calls** | **30** | **882,194** | **0.28 ms** | **3.4 ms** | **10** | **0** | **0** | **0** |
| leak.sh 2 x 400 short calls | 10 | 80,800 | 0.30 ms | 2.7 ms | 5 | 0 | 0 | 0 |

Asterisk used 44-46 % of ONE core for 30 cleaned calls, counting the Local channels and the clip
playback. Memory: we ran 2 x 400 short calls with ASTRINOISE and the same pattern without it. Asterisk's
RSS stayed flat in both (within +-20 kB per 100 calls), so we found no leak.

## Sound
We measured noise only in quiet stretches at least 100 ms away from speech.

| Clip | Noise between words | Agent voice level | Clicks | Voice dropouts |
|---|---|---|---|---|
| Fan + hum | -34.9 dB | -0.2 dB | 0 | 0 |
| Busy office | -12.3 dB | -0.5 dB | 0 | 0 |
| Other agents talking | -10.1 dB | -0.6 dB | 1 (raw had 2) | 0 |
| Keyboard | -7.6 dB | -0.1 dB | 0 (raw had 7) | 0 |
| Voice alone | -1.2 dB | 0.0 dB | 2 (= raw) | 0 |

Fixed delay: 50 ms.

## Older CPU (estimate)
We ran the same RNNoise code (no AVX) on an Intel Xeon E5620 (2010, no AVX). It took about 3x the lab
CPU: ~0.85-0.95 ms per 20 ms frame per cleaned call (~4-5 % of one core), worst frames ~10 ms. That
still fits inside the 20 ms frame, and it runs in the channel's own thread with no queue or shared
process. On such a box, 25 cleaned calls at once cost about 1.1 cores.

## Mistakes we caught on the way
- Our first quality metric let speech leak into "quiet" frames through misalignment. It showed a fake
  9 dB difference between two resampler settings whose audio was identical. We caught it by checking
  the level per 2 s and per frequency band.
- soak.sh counted the two halves of each Local channel pair as two calls (fixed: divide by 2).
