# Lab results (RNNoise 2026-11-23, DeepFilterNet 2026-11-24)

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

## DeepFilterNet next to RNNoise (2026-11-24)

Same lab box and Asterisk 22.11, module with both engines, `libastrinoise_dfn` 0.1.0 (DeepFilterNet
0.5.6, DFN3) with the stage thresholds and band fill described in [EVAL.md](EVAL.md).
`lab/soak.sh N 10 60`, with `ENGINE=deepfilter` for DeepFilterNet. The CPU column is Asterisk's
average over the run, in cores, counting the Local channels and the clip playback.

| Test | Calls at once | Frames (20 ms) | avg per frame | slowest frame | > 5 ms | > 10 ms | underruns | Asterisk CPU |
|---|---|---|---|---|---|---|---|---|
| RNNoise soak, new build | 30 | 873,236 | 0.29 ms | 8.8 ms | 2 | 0 | 0 | 0.50 |
| DeepFilterNet soak | 15 | 442,439 | 2.43 ms | 58 ms | 8,573 | 517 | 0 | 1.69 |
| DeepFilterNet soak | 30 | 860,239 | 5.62 ms | 366 ms | 226,316 | 116,045 | 0 | 3.28 |
| DeepFilterNet, 1-2 calls (`lab/engines.sh`, just after a restart) | 1-2 | 301 | 3.14 ms | 5.8 ms | 3 | 0 | 0 | |

- **RNNoise on the new build** costs the same as v0.1.0: 0.29 ms per frame against 0.28 ms. Its
  first minute overlapped the last DeepFilterNet calls of the run before; the per-engine line keeps
  them apart. A second run during the host's nightly backup gave 0.34 ms and a 58 ms worst frame,
  so the worst frame depends on what else the host is doing.
- **DeepFilterNet costs about 8 times the CPU of RNNoise per call**: 2.4 ms per 20 ms frame, or
  11-12 % of a core per cleaned call, where RNNoise needs 1.5-1.7 %.
- **30 DeepFilterNet calls overload 4 cores.** Asterisk used 3.3 of the 4 cores, 13 % of frames took
  over 10 ms and the slowest 0.37 s. The FIFO never ran short (0 underruns), but a frame that late
  reaches the caller late, the stall we built the module to avoid. At 15 calls the box stays comfortable. Plan about one core for
  every 7 DeepFilterNet calls, and set `max` to match.
- **The first DeepFilterNet state takes 0.1-0.16 s** (it builds the model), at load, in a background
  thread. Every later call gets a copy that shares the model's weights: 0.0-0.6 ms and no memory we
  can measure. The model itself adds about 33 MB to Asterisk once.

### Memory and threads
| Test | Result |
|---|---|
| `ENGINE=deepfilter leak.sh 400` (2 x 400 calls) | Asterisk RSS flat (+0 kB per 100 calls) |
| Threads in Asterisk before and after 800 DeepFilterNet calls | 77 and 77 |
| `ENGINE=rnnoise leak.sh 400` on the new build | RSS flat |

### One copy of the weights per call, or one shared copy
Our first library built a separate model for every call. With 30 calls it used 3.6 cores, 8.1 ms per
frame and up to 0.58 s for one frame, and Asterisk grew to 940 MB: every call read its own 20 MB of
weights, so the CPU spent its time waiting on memory. The shared copy (`fresh_copy()` in
`module/dfn/patches/`) took the same test to 1.9 ms per frame, 2.4 cores and 155 MB. Those runs
used the first build's stage thresholds, which skip more frames; the table above is the final build.

### Engine switch
`lab/engines.sh` (final build):

```
start rnnoise -> [rnnoise 80 rnnoise,deepfilter]  switched -> [deepfilter 100]  off -> [x0]
start deepfilter -> [deepfilter 80 rnnoise,deepfilter]  switched -> [rnnoise 100]  off -> [x0]
hook all-1001 -> strength [100] engine [rnnoise]
hook all-1001-n-deepfilter -> strength [100] engine [deepfilter]
hook all-1001-n-rnnoise -> strength [100] engine [rnnoise]
hook all-1001-e-deepfilter -> strength [0] engine []
hook off-1001-n-deepfilter -> strength [0] engine []
```

Each call starts on one engine, switches to the other in the middle and turns off. The FreePBX
hook picks the engine from the test call and still skips the emergency route and mode `off`.

### Without the DeepFilterNet library
With `libastrinoise_dfn.so` removed, the module loads as before, `${ASTRINOISE(engines)}` returns
`rnnoise`, a call set to `deepfilter` gets RNNoise and the log says why, and
`astrinoise show stats` reports `deepfilter: not installed`.

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
