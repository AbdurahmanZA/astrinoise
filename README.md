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

It has two cleaning engines, and you pick one per PBX on the dashboard or with one AstDB key.
RNNoise, the default, is light enough to clean a whole call centre on one PBX. DeepFilterNet
removes more of your colleagues' voices in the background and costs about 8 times the CPU. It
needs a recent CPU with AVX2: on an older server it cannot keep up with even one call.

In a test of 824 recordings sent through Asterisk as phone audio, the PESQ speech quality score
went from 2.90 to 3.31 with RNNoise and to 3.63 with DeepFilterNet. Asterisk's built-in `DENOISE()`
reached 2.97 on the same recordings.

Today it cleans your users' voices on calls they make out over a trunk. It leaves the other
party's audio as it is, and it does not clean incoming calls yet.

## How it fits in Asterisk

The module, `func_astrinoise.so`, adds the dialplan function `ASTRINOISE(rx|tx)` and uses
[RNNoise](https://github.com/xiph/rnnoise) to do the cleaning. If you also install the optional
`libastrinoise_dfn.so`, it can use [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
(DFN3) instead. It works like the stock
`DENOISE(rx|tx)` from `func_speex`: an audiohook on the channel cleans each audio frame in place,
in the channel's own thread.

- `module/`: the module source and `build.sh`
- `module/dfn/`: the optional DeepFilterNet engine, a small Rust library, and its `build.sh`
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
frame in (8/16 kHz) -> upsample to 48 kHz (speexdsp, quality 8) -> RNNoise or DeepFilterNet
   in 10 ms frames -> downsample -> FIFO primed with 20 ms -> exactly as many samples out as came in
```

- Fixed delay: about 50 ms with RNNoise (20 ms FIFO + 20 ms RNNoise + resamplers) and about 60 ms
  with DeepFilterNet (its own lag is 30 ms).
- If the FIFO ever runs short, the original audio fills the gap instead of silence, and the
  `underruns` counter goes up. We saw 0 underruns in every lab test.
- Strength 1-100 mixes the original back in, lined up with the engine's own lag: 20 ms for RNNoise
  and 30 ms for DeepFilterNet, both measured with `lab/eval/lag.py`. 100 = the engine's output only.
- Any channel rate works (8, 16, 48 kHz). The module rebuilds its resamplers when the rate changes.
- Turning both directions off removes the audiohook from the channel.

## Usage

```
Set(ASTRINOISE(rx)=on)     ; clean the audio Asterisk receives from this channel (on a phone's channel: that person's voice)
Set(ASTRINOISE(tx)=on)     ; clean the audio Asterisk sends to this channel (what this caller hears)
Set(ASTRINOISE(rx)=60)     ; 60 % strength: 40 % of the original mixed back in
Set(ASTRINOISE(rx,deepfilter)=on)   ; use DeepFilterNet for this direction
Set(ASTRINOISE(rx)=off)    ; stop
same => n,NoOp(${ASTRINOISE(rx)})          ; current strength, 0 = off
same => n,NoOp(${ASTRINOISE(rx,engine)})   ; engine on this direction: rnnoise, deepfilter, or empty when off
same => n,NoOp(${ASTRINOISE(engines)})     ; engines this PBX has: rnnoise or rnnoise,deepfilter
```

The second argument picks the engine: `rnnoise` or `deepfilter`. Leave it out and you get the
module's default, which is RNNoise unless `/etc/asterisk/astrinoise.conf` says otherwise (see
[module/astrinoise.conf.sample](module/astrinoise.conf.sample)). If you ask for `deepfilter` on a PBX
without `libastrinoise_dfn.so`, the call gets RNNoise and the log says why, once a minute at most.
Setting a different engine on a call that is already cleaned switches it at the next frame.

`on`, `yes`, `true` and `1` mean 100; `off`, `no`, `false` and `0` mean off. So the lowest
strength you can set is 2.

CLI:

```
*CLI> astrinoise show stats
active=12 frames=882194 avg_us=280.4 max_us=3401.0 over2ms=10 over5ms=0 over10ms=0 underruns=0 overflows=0
engine rnnoise: active=12 frames=882194 avg_us=280.4 max_us=3401.0 over2ms=10 over5ms=0 over10ms=0 errors=0 lag_ms=20 default
engine deepfilter: active=0 frames=0 avg_us=0.0 max_us=0.0 over2ms=0 over5ms=0 over10ms=0 errors=0 lag_ms=30
deepfilter: astrinoise-dfn 0.1.0 (DeepFilterNet 0.5.6, DFN3), states_made=1 make_avg_ms=120.0 make_max_ms=120.0 failed=0
*CLI> astrinoise reset stats
```

`active` counts directions being cleaned. The first line covers every ASTRINOISE channel together
since load or reset: average and slowest time per frame, and how many frames took over 2, 5 and
10 ms. A frame is 20 ms of audio, so anything well under 20 ms is fine. `underruns` must stay 0;
`overflows` counts times the module dropped audio to stop the delay growing. Then you get one line
per engine (`default` marks the default engine, `errors` counts frames the engine failed on and
sent the original for) and a line that says whether DeepFilterNet is installed.

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

### DeepFilterNet engine (optional)

```
curl https://sh.rustup.rs -sSf | sh -s -- -y --profile minimal    # Rust, on the build box only
apt-get install git patch gcc
bash module/dfn/build.sh        # -> module/dfn/libastrinoise_dfn.so, about 26 MB
```

One build of the library works with every Asterisk version, because it does not use Asterisk at
all. `module/dfn/build.sh` fetches DeepFilterNet 0.5.6 by git tag, checks the commit, applies the
patch in `module/dfn/patches/` and builds with the pinned `Cargo.lock`. The DFN3 model is built into
the library. The build needs about 1.5 GB of RAM per job (`JOBS=1` on a small box) and 3 minutes on
an i5-12500.

**Check the PBX's CPU before you switch any phone to DeepFilterNet.** It needs AVX2
(`grep -c avx2 /proc/cpuinfo` must not print 0). On an Intel Xeon E5620 (2010, no AVX) one
DeepFilterNet call took 23.3 ms on average and 34 ms at worst per 20 ms frame inside Asterisk,
with 16 other calls on the box. That is slower than real time, so the call breaks up. The bare
library on the same CPU took 11.6 ms per frame, and RNNoise took 1.12 ms. Before you use it, make
one test call with DeepFilterNet and run `astrinoise show stats`: the `engine deepfilter` line's
`avg_us` must stay well under 10000 (10 ms) and `max_us` under 20000.

Why a library of our own, and not DeepFilterNet's LADSPA plugin or C API:

- The LADSPA plugin starts a worker thread for each instance that polls every 2 ms and never exits,
  and its delay grows after a slow frame. A PBX opens and closes thousands of these a day.
- Its instances are clones of one model, and the clones share two buffers that DeepFilterNet
  writes on every frame, so two calls at once can overwrite each other's features (we read this in
  its code; our library's `lab/eval/dfn-bench xtalk` test checks that ours does not).
- The C API panics on errors, and a Rust panic that reaches Asterisk stops your PBX. Our library
  catches panics and returns an error; the module then sends that frame's original audio.
- Our library loads the model once and gives each call a copy that shares the read-only weights
  (the patch adds that copy). A new call needs about 0.3 ms and no memory we can measure,
  against 70 ms and 20 MB for a separate model per call. With 30 calls the shared copy cut the
  CPU per frame from 8.1 ms to 1.9 ms, because the calls read one set of weights instead of 30.

Install:

```
install -m 644 module/func_astrinoise.so /usr/lib/x86_64-linux-gnu/asterisk/modules/   # source builds: /usr/lib/asterisk/modules/
asterisk -rx 'module load func_astrinoise.so'

# optional, for DeepFilterNet:
mkdir -p /usr/lib/astrinoise
install -m 644 module/dfn/libastrinoise_dfn.so /usr/lib/astrinoise/
```

Install the library before the module loads, or restart Asterisk after you add or replace it. The
module opens it once, at load, and never closes it. Do not put it in Asterisk's modules folder:
Asterisk would try to load it as a module.

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
| `engine` | `rnnoise` or `deepfilter`. Missing = the module's default (RNNoise) |
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
and talk: you hear your own voice cleaned with the engine in `rnnoise/engine`. `*7867` always uses
DeepFilterNet. Dial `*43` to compare.

To try DeepFilterNet on one PBX: `asterisk -rx 'database put rnnoise engine deepfilter'`. New calls
use it straight away; calls already up keep their engine. Lower `max` first (see Engines).

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
- `lab/engines.sh` checks the engine switch: both engines, a switch in the middle of a call, and the
  FreePBX hook with each engine.
- Put `ENGINE=deepfilter` in front of `functional.sh`, `soak.sh` or `leak.sh` to test DeepFilterNet.

Supply your own clips as 8 kHz mono WAV files in `/var/lib/asterisk/sounds/rnnlab/` named `fan`,
`office`, `chatter`, `keyboard` and `voice`: one person talking over each kind of noise. We did not
publish ours.

On an i5-12500 with Asterisk 22.11 we measured, for 30 RNNoise calls at once over 10 minutes:

- 0.28 ms average and 3.4 ms slowest per 20 ms frame, 0 underruns
- no memory growth over 2 x 400 calls
- noise between words: fan -35 dB, busy office -12 dB, other agents talking -10 dB, keyboard -7.6 dB
- agent voice level within 0.6 dB of the original

On a 2010 Xeon without AVX the same code took about 3x as long (~0.9 ms per frame per call).
With DeepFilterNet, 15 calls at once took 2.4 ms per frame and 1.7 cores, and 30 calls overloaded
the lab box's 4 cores. On a Xeon E5620 without AVX, one DeepFilterNet call took 23.3 ms per
20 ms frame, slower than real time, so DeepFilterNet is not usable on that CPU.
Full tables: [lab/RESULTS.md](lab/RESULTS.md).

Quality on the 824-file VoiceBank+DEMAND test set, sent through Asterisk as G.711 phone audio
([lab/EVAL.md](lab/EVAL.md)):

| Condition | PESQ-NB | ESTOI | SI-SDR | DNSMOS voice / background / overall |
|---|---|---|---|---|
| No filter | 2.90 | 0.783 | 8.4 dB | 3.35 / 3.15 / 2.71 |
| Stock DENOISE | 2.97 | 0.776 | 9.5 dB | 3.31 / 3.28 / 2.72 |
| RNNoise 80 % | 3.31 | 0.821 | 13.7 dB | 3.36 / 3.73 / 2.96 |
| RNNoise 100 % | 3.22 | 0.814 | 13.2 dB | 3.29 / 3.99 / 3.00 |
| DeepFilterNet 80 % | 3.40 | 0.831 | 16.0 dB | 3.38 / 3.76 / 2.98 |
| DeepFilterNet 100 % | 3.63 | 0.843 | 17.8 dB | 3.36 / 4.05 / 3.09 |

**With RNNoise, use strength 80.** It gives the best voice and intelligibility. 100 removes the
most background but thins the voice a little, most with colleagues talking nearby. **With
DeepFilterNet, use 100**: it scores best there on everything but the voice score, which stays level.

## Engines

| | RNNoise (default) | DeepFilterNet (DFN3) |
|---|---|---|
| CPU per 20 ms frame | 0.29 ms (30 calls at once) | 2.4 ms (15 calls at once) |
| Asterisk CPU | 0.5 core for 30 calls | 1.7 cores for 15 calls; 30 calls overloaded 4 cores |
| Calls per core, with room to spare | about 40 | about 7 |
| Memory | none we could measure | 33 MB once for the model, nothing measurable per call |
| Engine lag (fixed delay) | 20 ms (50 ms) | 30 ms (60 ms) |
| Best strength | 80 | 100 |
| PESQ-NB at that strength (no filter: 2.90) | 3.31 | 3.63 |
| ESTOI (no filter: 0.783) | 0.821 | 0.843 |
| DNSMOS voice / background / overall | 3.36 / 3.73 / 2.96 | 3.36 / 4.05 / 3.09 |
| PESQ-NB in cafe noise, the closest to colleagues talking (no filter: 2.35) | 2.76 | 3.18 |
| Needs | nothing extra | `libastrinoise_dfn.so` |

We measured both on the same lab box (4 cores of an i5-12500, Asterisk 22.11) with the same tests:
`lab/soak.sh N 10 60` and the VoiceBank+DEMAND evaluation. Full tables:
[lab/RESULTS.md](lab/RESULTS.md) and [lab/EVAL.md](lab/EVAL.md).

Pick RNNoise when one box cleans many calls: 30 calls cost half a core. Pick DeepFilterNet when
your agents sit in a busy room and you can give it about one core for every 7 cleaned calls. It
beat RNNoise on every quality score we ran except the voice score, where they tie, and it keeps
more of the agent's voice when colleagues talk nearby. Use strength 100 with DeepFilterNet and 80
with RNNoise.

Switch the whole PBX with the dashboard's Engine buttons or with
`asterisk -rx 'database put rnnoise engine deepfilter'` (back: `... engine rnnoise`). The FreePBX hook
reads the key on every new outbound call. The cap in `max`, phones set `off` and the emergency
route skip work the same with both engines. On a PBX without `libastrinoise_dfn.so`, calls set to
DeepFilterNet get RNNoise and the dashboard greys out the DeepFilterNet button.

For phone calls the module adds quiet noise above the call's band before DeepFilterNet (the band
fill in `func_astrinoise.c`). DFN3 cleans 8 kHz audio much worse without it; the caller never hears
the noise. [lab/EVAL.md](lab/EVAL.md) has the measurements.

## Limits

- RNNoise cleans steady noise (fans, hum, aircon) well and busy rooms less so. It turns down other
  people talking by about 10 dB; it does not remove them.
- RNNoise can soften consonants on narrowband (8 kHz) calls. Listen to real calls before you switch
  on every phone, and use a strength below 100 if voices sound dull.
- The module adds a fixed 50 ms (RNNoise) or 60 ms (DeepFilterNet) to each cleaned direction.
- CPU cost grows with the number of cleaned calls. Set `max` on small boxes, and lower it before you
  switch to DeepFilterNet.
- DeepFilterNet needs a CPU with AVX2. On a Xeon E5620 it ran slower than real time with one
  call. Check `avg_us` in `astrinoise show stats` on a test call first.
- `libastrinoise_dfn.so` is built for x86-64 Linux only. We have not built it for ARM.
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
  DeepFilterNet through an external library. Two commits. We read it for ideas; our DeepFilterNet
  engine shares the weights between calls, aligns the strength mix with the measured lag and has
  published load and quality results.
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

The DeepFilterNet engine, `module/dfn/`, is a separate library under MIT OR Apache-2.0
([LICENSE-MIT](module/dfn/LICENSE-MIT), [LICENSE-APACHE](module/dfn/LICENSE-APACHE)), the same terms
as [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) itself. It builds in DeepFilterNet's
code and its DFN3 model (MIT OR Apache-2.0, Hendrik Schröter), the tract inference engine (MIT OR
Apache-2.0, Sonos) and prost (Apache-2.0). Because prost is Apache-2.0 only, the library is not
GPL-2.0: the module opens it at run time if you install it, and works without it.
[module/dfn/THIRD-PARTY.md](module/dfn/THIRD-PARTY.md) lists every crate in the library with its
licence.

Copyright (C) 2026 SANDZ (Solomon and Zoutenburg (Pty) Ltd).
