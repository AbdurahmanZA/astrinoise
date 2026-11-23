# Astri-noise

A noise filter for FreePBX and Asterisk. It takes fan, keyboard and office noise out of your
staff's voices before the person on the other end hears it.

You choose where it runs:

- **Selected phones.** Pick the IP phones or softphones that should use it, on a web page or with
  one command per phone.
- **The whole trunk.** Switch it on for every call your phones make out over the trunk, then
  leave out any phone you don't want filtered.

It runs inside Asterisk as one small module, so you need no cloud service, no extra server and
no change to your phones. Your call audio stays on your PBX.

We built it for a call centre on FreePBX 17, where agents sit close together and many calls run
at once, so it has to stay light. On an Intel i5-12500, 30 calls cleaned at the same time used less than half of one CPU core,
with no dropouts over a 10-minute test. You can also set the most calls it cleans at once, so a
small PBX keeps CPU in reserve.

It never filters a call on an emergency route.

In a test of 824 recordings sent through Asterisk as phone audio, the PESQ speech quality score
went from 2.90 to 3.31. Asterisk's built-in `DENOISE()` reached 2.97 on the same recordings.

Today it cleans your users' voices on calls they make out over a trunk. It leaves the other
party's audio as it is, and it does not clean incoming calls yet.

## How it fits in Asterisk

The module, `func_astrinoise.so`, adds the dialplan function `ASTRINOISE(rx|tx)` and uses
[RNNoise](https://github.com/xiph/rnnoise) to do the cleaning. It works like the stock
`DENOISE(rx|tx)` from `func_speex`: an audiohook on the channel cleans each audio frame in place,
in the channel's own thread.

- `module/`: the module source and `build.sh`
- `dialplan/`: FreePBX integration (outbound predial hook, AstDB switches, test context, echo test)
- `dashboard/`: two web pages to switch cleaning and phones on and off. See [docs/DASHBOARD.md](docs/DASHBOARD.md)
- `lab/`: the load and sound tests we ran, with [load results](lab/RESULTS.md) and a [quality evaluation](lab/EVAL.md) on the VoiceBank+DEMAND test set

## Why inside Asterisk

Our first version sent call audio out of Asterisk (ARI + AudioSocket) to a Python engine and back.
CPU was fine at 1-2 ms per 20 ms frame. The single engine process was not: with several calls at
once it sometimes answered 50-150 ms late, and customers heard static and broken audio. We switched
it off the same day.

A module avoids that. Each call cleans its own audio in its own thread, with no network hop, no
queue and no shared process. If one call's frame takes longer, only that call waits, and the delay
stays fixed.

## How it works

```
frame in (8/16 kHz) -> upsample to 48 kHz (speexdsp, quality 8) -> RNNoise in 10 ms frames
   -> downsample -> FIFO primed with 20 ms -> exactly as many samples out as came in
```

- Fixed delay of about 50 ms (20 ms FIFO + 20 ms RNNoise + resamplers).
- If the FIFO ever runs short, the original audio fills the gap instead of silence, and the
  `underruns` counter goes up. We saw 0 underruns in every lab test.
- Strength 1-100 mixes the original back in, lined up with RNNoise's two-frame (20 ms) lag. 100 = RNNoise only.
- Any channel rate works (8, 16, 48 kHz). The module rebuilds its resamplers when the rate changes.
- Turning both directions off removes the audiohook from the channel.

## Usage

```
Set(ASTRINOISE(rx)=on)     ; clean the audio Asterisk receives from this channel (on a phone's channel: that person's voice)
Set(ASTRINOISE(tx)=on)     ; clean the audio Asterisk sends to this channel (what this caller hears)
Set(ASTRINOISE(rx)=60)     ; 60 % strength: 40 % of the original mixed back in
Set(ASTRINOISE(rx)=off)    ; stop
same => n,NoOp(${ASTRINOISE(rx)})   ; current strength, 0 = off
```

`on`, `yes`, `true` and `1` mean 100; `off`, `no`, `false` and `0` mean off. So the lowest
strength you can set is 2.

CLI:

```
*CLI> astrinoise show stats
active=12 frames=882194 avg_us=280.4 max_us=3401.0 over2ms=10 over5ms=0 over10ms=0 underruns=0 overflows=0
*CLI> astrinoise reset stats
```

`active` counts directions being cleaned. The times cover every ASTRINOISE channel together since load
or reset: average and slowest time per frame, and how many frames took over 2, 5 and 10 ms. A frame
is 20 ms of audio, so anything well under 20 ms is fine. `underruns` must stay 0; `overflows` counts
times the module dropped audio to stop the delay growing.

## Build

Build on a lab box with the **same Asterisk version and build options** as the PBX that will load
the module. Asterisk refuses a module built against different build options.

FreePBX 17 (Sangoma `asterisk22` packages on Debian 12):

```
apt-get install asterisk22-devel=<same version as asterisk22> libspeexdsp-dev gcc make autoconf automake libtool curl
bash module/build.sh            # -> module/func_astrinoise.so
```

Asterisk built from source: `make install` puts the headers in `/usr/include/asterisk`. Install
`libspeexdsp-dev` and the build tools above, then run `bash module/build.sh`. Set
`ASTERISK_INCLUDE=/path/to/include` if your headers live elsewhere.

`build.sh` downloads the RNNoise 0.2 release tarball, checks its sha256, and links it in statically
without AVX dispatch, so the same `.so` runs on CPUs without AVX. It links the system
`libspeexdsp`, which `func_speex` already needs. For experiments:
`EXTRA_CFLAGS="-DRESAMPLE_QUALITY=4" bash module/build.sh`. Quality 8 removed about 22 dB of fan
noise against 13 dB at quality 4, for 0.02 ms more CPU per frame.

Install:

```
install -m 644 module/func_astrinoise.so /usr/lib/x86_64-linux-gnu/asterisk/modules/   # source builds: /usr/lib/asterisk/modules/
asterisk -rx 'module load func_astrinoise.so'
```

Loading only registers `ASTRINOISE()`. Nothing runs until a call sets it. FreePBX autoloads the module
after the next restart.

## FreePBX integration

[dialplan/extensions_override_freepbx.conf](dialplan/extensions_override_freepbx.conf) overrides
FreePBX's empty `[macro-dialout-trunk-predial-hook]`. FreePBX runs that hook on the calling phone's
channel before it dials the trunk, so `ASTRINOISE(rx)` there cleans the agent's voice on its way
to the customer. The customer's audio stays untouched.

The hook reads its switches from AstDB family `rnnoise`, so you need no ARI, relay or extra
service in the call path, and you get no extra CDR rows:

| Key | Value |
|---|---|
| `mode` | `off`, `all` (every phone except those set `off`) or `some` (only phones set `on`). Missing = off |
| `strength` | 1-100 for every cleaned call. Missing = 100 |
| `max` | most cleaned calls at once, counted with `GROUP()`. 0 or missing = no limit |
| `ext/<phone>` | `on` or `off`. The phone can be the PJSIP endpoint, a hot-desk phone or the user number |

A phone set `off` always wins. The hook never cleans a call on an emergency route. The rules and a
test context that checks them without a real call are in [docs/DASHBOARD.md](docs/DASHBOARD.md).

Start with one phone:

```
asterisk -rx 'database put rnnoise ext/1001 on'
asterisk -rx 'database put rnnoise mode some'
```

Switch it all off for new calls with `asterisk -rx 'database put rnnoise mode off'`. Calls already
up keep cleaning until they end. To remove it completely, delete the override, run
`dialplan reload`, then `module unload func_astrinoise.so`.

To hear it, add the optional echo test from the same file to `extensions_custom.conf`. Dial `*7866`
and talk: you hear your own voice cleaned. Dial `*43` to compare.

A pilot plan that worked for us: one phone for a day, watching `astrinoise show stats` (underruns 0,
`max_us` well under 20 ms), and asking the agent and the team leader how calls sounded. Then a few
more phones, then `mode all`.

## Lab tests and results

`lab/extensions.conf` holds test contexts that play noisy clips into Local channels and record them
with and without the filter. Never load it on a production PBX.

- `lab/functional.sh [secs]` records every clip cleaned and raw, for listening and measuring.
- `lab/soak.sh 30 15 60` keeps 30 cleaned calls running for 15 minutes and logs CPU, memory and
  the module's timing every 30 s.
- `lab/leak.sh 400` runs 400 short calls with and without ASTRINOISE and compares Asterisk's memory.

Supply your own clips as 8 kHz mono WAV files in `/var/lib/asterisk/sounds/rnnlab/` named `fan`,
`office`, `chatter`, `keyboard` and `voice`: one person talking over each kind of noise. We did not
publish ours.

On an i5-12500 with Asterisk 22.11 we measured, for 30 calls at once over 10 minutes:

- 0.28 ms average and 3.4 ms slowest per 20 ms frame, 0 underruns
- no memory growth over 2 x 400 calls
- noise between words: fan -35 dB, busy office -12 dB, other agents talking -10 dB, keyboard -7.6 dB
- agent voice level within 0.6 dB of the original

On a 2010 Xeon without AVX the same code took about 3x as long (~0.9 ms per frame per call).
Full tables: [lab/RESULTS.md](lab/RESULTS.md).

Quality on the 824-file VoiceBank+DEMAND test set, sent through Asterisk as G.711 phone audio
([lab/EVAL.md](lab/EVAL.md)):

| Condition | PESQ-NB | ESTOI | SI-SDR | DNSMOS voice / background / overall |
|---|---|---|---|---|
| No filter | 2.90 | 0.783 | 8.4 dB | 3.35 / 3.15 / 2.71 |
| Stock DENOISE | 2.97 | 0.776 | 9.5 dB | 3.31 / 3.28 / 2.72 |
| ASTRINOISE 80 % | 3.31 | 0.821 | 13.7 dB | 3.36 / 3.73 / 2.96 |
| ASTRINOISE 100 % | 3.22 | 0.814 | 13.2 dB | 3.29 / 3.99 / 3.00 |

**Use strength 80 by default.** It gives the best voice and intelligibility. 100 removes the most
background but thins the voice a little, most with colleagues talking nearby.

## Engines

RNNoise is the only engine today. We chose it because it is light: about 0.3 ms of CPU for each
20 ms of audio, which lets one PBX clean many calls at once.

DeepFilterNet removes more background talk than RNNoise but needs more CPU per call. We plan to
add it as a second engine, with a switch to pick which one a PBX uses, so a small call centre can
choose the stronger cleaning and a large one can keep the low CPU cost.

## Limits

- RNNoise cleans steady noise (fans, hum, aircon) well and busy rooms less so. It turns down other
  people talking by about 10 dB; it does not remove them.
- RNNoise can soften consonants on narrowband (8 kHz) calls. Listen to real calls before you switch
  on every phone, and use a strength below 100 if voices sound dull.
- The module adds a fixed 50 ms to each cleaned direction.
- CPU cost grows with the number of cleaned calls. Set `max` on small boxes.
- Each Asterisk version needs its own build. Rebuild after every Asterisk upgrade.
- We tested on Asterisk 22 (FreePBX 17) only. The audiohook API is the same in Asterisk 18-21, but
  we have not built or tested it there.

## Related work

We looked for earlier work before publishing (2026-11):

- [sokrat-voip-dev](https://github.com/Ahmed-Emad02/sokrat-voip-dev) (GPLv2 module, active in
  2026): the closest. It has a `func_rnnoise.so` / `RNNOISE()` module, per-phone AstDB switches
  in the same FreePBX predial hook, and a per-phone web toggle, inside its own Issabel/Asterisk 18
  product. It resamples with linear interpolation and gates audio to zero; it has no fixed-delay
  FIFO, strength, stats, call cap or emergency bypass, and no published measurements.
- [deepfilter-asterisk](https://github.com/bogdanovvictor02/deepfilter-asterisk) (early 2026):
  `DEEPFILTER()` with the same func_speex-style audiohook and speexdsp resampler, running
  DeepFilterNet through an external library. Two commits.
- An Asterisk forum user described an unpublished `RNNOISE()` module in July 2025 and later dropped
  RNNoise because it removed consonants.
- Janus AudioBridge has had an RNNoise `denoise` option since 2024 (WebRTC, not Asterisk).
- We found nothing for FreeSWITCH, rtpengine or baresip, and nothing in Asterisk itself beyond
  `DENOISE()`.

What Astri-noise adds: speexdsp resampling, a fixed delay, original audio on a shortfall instead of
silence, a 0-100 strength, `astrinoise show stats`, a cap on cleaned calls, no cleaning on emergency
routes, a stand-alone add-on for stock FreePBX, and published load and quality results.

We renamed our module from `func_rnnoise` to `func_astrinoise` so the two names no longer clash.

## Licence

GPL-2.0 (see [LICENSE](LICENSE)), like Asterisk. The module links in
[RNNoise](https://github.com/xiph/rnnoise) (BSD-3-Clause, Xiph.Org / Jean-Marc Valin) statically and
uses the system's [speexdsp](https://github.com/xiph/speexdsp) (BSD-3-Clause). Both licences allow
this. Keep their copyright notices if you redistribute binaries.

Copyright (C) 2026 SANDZ (Solomon and Zoutenburg (Pty) Ltd).
