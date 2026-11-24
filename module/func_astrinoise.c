/*
 * func_astrinoise (Astri-noise) - noise suppression on a channel, inside Asterisk.
 *
 * Copyright (C) 2026, SANDZ (Solomon and Zoutenburg (Pty) Ltd).
 *
 * This program is free software, distributed under the terms of the GNU General Public License
 * Version 2, like Asterisk itself. RNNoise (xiph, BSD-3-Clause) is linked in statically; the
 * speexdsp resampler (BSD) comes from the system's libspeexdsp, as func_speex already uses.
 * DeepFilterNet (MIT OR Apache-2.0) is optional: it lives in its own library,
 * libastrinoise_dfn.so, which this module opens at load time only if it is installed.
 *
 * Why a module: an earlier design sent call audio out of Asterisk to a Python engine and back.
 * Under load that engine sometimes answered 50-150 ms late, and callers heard gaps and
 * static. Here the cleaning runs in the channel's own audio path, the same way DENOISE() (func_speex)
 * does, so there is no trip out and back and nothing can arrive late:
 *
 *   every frame in -> upsample to 48 kHz -> engine in 10 ms frames -> downsample -> FIFO -> frame out
 *
 * Two engines, both 10 ms frames at 48 kHz:
 *   rnnoise     the default. About 0.3 ms of CPU per 20 ms of audio, for many calls at once.
 *   deepfilter  DeepFilterNet3. Removes more background talk, costs several times the CPU.
 *
 * The FIFO starts with PRIME_MS of silence, so the output always has exactly as many samples as the
 * input (a fixed delay of PRIME_MS + the engine's own lag + the resamplers). If it ever runs short,
 * the original audio fills the gap (never silence) and the shortfall is counted.
 * `astrinoise show stats` shows the CPU time per frame for each engine.
 *
 * Usage (dialplan or ARI channel variable):
 *   Set(ASTRINOISE(rx)=on)              clean the audio Asterisk receives from this channel (the agent's voice)
 *   Set(ASTRINOISE(rx)=60)              60 % strength (mixes 40 % of the original back in, time-aligned)
 *   Set(ASTRINOISE(rx,deepfilter)=80)   use DeepFilterNet for this direction (rnnoise if it is not installed)
 *   Set(ASTRINOISE(rx)=off)             stop
 *   ${ASTRINOISE(rx)}                   current strength, 0 = off
 *   ${ASTRINOISE(rx,engine)}            engine cleaning this direction: rnnoise, deepfilter or empty
 *   ${ASTRINOISE(engines)}              engines this PBX can use: rnnoise or rnnoise,deepfilter
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <speex/speex_resampler.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/cli.h"
#include "asterisk/config.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"

#include "rnnoise.h"

#define ENG_RATE 48000
#define ENG_FRAME 480                 /* 10 ms at 48 kHz, both engines */
#define PRIME_MS 20                   /* fixed buffer: output never waits on a partly filled engine frame */
#define LAG_MAX 4800                  /* longest engine lag we can line the original up with (100 ms) */
#ifndef RESAMPLE_QUALITY
/* speexdsp 0-10. Lab 2026-11-23: 8 removes ~22 dB of fan noise vs ~13 dB at 4, for +0.02 ms CPU a frame. */
#define RESAMPLE_QUALITY 8
#endif
#ifndef DFN_LAG
/* DeepFilterNet3 (astrinoise-dfn 0.1.0, DeepFilterNet 0.5.6): output lags input by this many
 * samples at 48 kHz, measured with lab/eval/dfn-bench (lag mode). */
#define DFN_LAG 1440
#endif

#ifndef DFN_FILL
/* DeepFilterNet on phone audio: noise above the call's band, RMS on the -1..1 scale (see fill_setup()). */
#define DFN_FILL 0.02f
#endif

#define DFN_LIBRARY "/usr/lib/astrinoise/libastrinoise_dfn.so"
#define DFN_ABI 1

enum engine_id { ENG_RNNOISE, ENG_DEEPFILTER, ENG_COUNT };
static const char *const engine_names[ENG_COUNT] = { "rnnoise", "deepfilter" };
/* Output lag of each engine at 48 kHz. RNNoise 0.2: 2 frames = 960 samples (measured, lab/eval/lag.c). */
static const int engine_lag[ENG_COUNT] = { 960, DFN_LAG };

struct biquad {
	float b0, b1, b2, a1, a2, z1, z2;
};

struct rnn_dir {
	int engine;                       /* enum engine_id */
	void *st;                         /* DenoiseState * or the DeepFilterNet state */
	SpeexResamplerState *up, *down;
	int rate;                         /* channel sample rate this state is set up for, 0 = set up on the next frame */
	int strength;                     /* 1-100 */
	float in48[ENG_FRAME];            /* upsampled input waiting for a full engine frame */
	int in48_n;
	float dry48[LAG_MAX + ENG_FRAME]; /* the last `lag` input samples, then the current frame */
	float wet48[ENG_FRAME];
	int16_t *outq;                    /* cleaned audio at the channel rate, waiting to go out */
	int outq_n, outq_cap;
	int16_t *up_buf;                  /* scratch for upsampled audio */
	int up_cap;
	int16_t *down_buf;                /* scratch for downsampled audio */
	int down_cap;
	int fill;                         /* DeepFilterNet: add the band fill (channel rate below 32 kHz) */
	uint32_t rng;
	struct biquad hp[4];
};

struct rnn_info {
	struct ast_audiohook audiohook;
	struct rnn_dir *tx, *rx;
};

/* Counters for `astrinoise show stats`, per engine (all channels together). */
struct eng_stats {
	uint64_t frames, ns_total, ns_max, over2ms, over5ms, over10ms, errors;
	int64_t active;
};
static struct {
	struct eng_stats e[ENG_COUNT];
	uint64_t underruns, overflows;
} stats;

/* Settings from astrinoise.conf (all optional). */
static int default_engine = ENG_RNNOISE;
static char dfn_path[256] = DFN_LIBRARY;

/* The DeepFilterNet library, if installed. It builds one base model and gives each call direction a
 * copy that shares the weights: the first state takes about 0.1 s, later ones almost nothing. */
static struct {
	void *lib;
	int (*abi)(void);
	const char *(*version)(void);
	void *(*create)(float atten_lim_db);
	int (*frame_len)(const void *st);
	int (*process)(void *st, const float *in, float *out);
	void (*destroy)(void *st);
	char why[400];                    /* why it is not loaded */
	pthread_t warm;                   /* builds the base model at load, so the first call does not wait */
	int warm_up;
	uint64_t built, build_ns_total, build_ns_max, build_fail;
} dfn;

static uint64_t now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t) t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void max_ns(uint64_t *slot, uint64_t ns)
{
	uint64_t prev = __atomic_load_n(slot, __ATOMIC_RELAXED);

	while (ns > prev && !__atomic_compare_exchange_n(slot, &prev, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

/* Make one DeepFilterNet state and count it. */
static void *dfn_build(void)
{
	uint64_t t0 = now_ns(), ns;
	void *st = dfn.create(100.0f);

	ns = now_ns() - t0;
	if (!st || dfn.frame_len(st) != ENG_FRAME) {
		if (st) {
			dfn.destroy(st);
		}
		__atomic_fetch_add(&dfn.build_fail, 1, __ATOMIC_RELAXED);
		return NULL;
	}
	__atomic_fetch_add(&dfn.built, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&dfn.build_ns_total, ns, __ATOMIC_RELAXED);
	max_ns(&dfn.build_ns_max, ns);
	return st;
}

/* Building the model needs more stack than Asterisk gives its own threads (about 500 kB), so states
 * are always made on a thread of ours with 8 MB. */
#define BUILD_STACK (8 * 1024 * 1024)

static int big_stack_thread(pthread_t *t, void *(*fn)(void *), void *arg)
{
	pthread_attr_t attr;
	int res;

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, BUILD_STACK);
	res = pthread_create(t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	return res;
}

static void *build_once(void *out)
{
	*(void **) out = dfn_build();
	return NULL;
}

/* A new state for one call direction, or NULL. */
static void *dfn_take(void)
{
	pthread_t t;
	void *st = NULL;

	if (big_stack_thread(&t, build_once, &st)) {
		return NULL;
	}
	pthread_join(t, NULL);
	return st;
}

/* At load: build the base model at low priority, then drop the state (the base stays). */
static void *dfn_warm(void *unused)
{
	void *st;

	/* nice 10 for this thread only (Asterisk hides setpriority() to protect its own priority). */
	syscall(SYS_setpriority, PRIO_PROCESS, (int) syscall(SYS_gettid), 10);
	if ((st = dfn_build())) {
		dfn.destroy(st);
	} else {
		ast_log(LOG_ERROR, "ASTRINOISE: DeepFilterNet could not build its model; deepfilter calls will use rnnoise\n");
	}
	return NULL;
}

static void dfn_load(void)
{
	void *lib;

	if (dfn.lib) {
		return;
	}
	if (access(dfn_path, R_OK)) {
		snprintf(dfn.why, sizeof(dfn.why), "not installed (%s)", dfn_path);
		return;
	}
	/* RTLD_LOCAL: its symbols stay out of Asterisk's namespace. */
	if (!(lib = dlopen(dfn_path, RTLD_NOW | RTLD_LOCAL))) {
		snprintf(dfn.why, sizeof(dfn.why), "cannot open: %s", dlerror());
		ast_log(LOG_WARNING, "ASTRINOISE: DeepFilterNet library %s\n", dfn.why);
		return;
	}
	dfn.abi = dlsym(lib, "astrinoise_dfn_abi");
	dfn.version = dlsym(lib, "astrinoise_dfn_version");
	dfn.create = dlsym(lib, "astrinoise_dfn_new");
	dfn.frame_len = dlsym(lib, "astrinoise_dfn_frame_len");
	dfn.process = dlsym(lib, "astrinoise_dfn_process");
	dfn.destroy = dlsym(lib, "astrinoise_dfn_free");
	if (!dfn.abi || !dfn.version || !dfn.create || !dfn.frame_len || !dfn.process || !dfn.destroy || dfn.abi() != DFN_ABI) {
		snprintf(dfn.why, sizeof(dfn.why), "%s is not a libastrinoise_dfn for this module (ABI %d)", dfn_path, DFN_ABI);
		ast_log(LOG_WARNING, "ASTRINOISE: %s\n", dfn.why);
		dlclose(lib);
		return;
	}
	/* Never dlclose() a loaded copy: Rust libraries may leave thread-local destructors behind. */
	dfn.lib = lib;
	dfn.why[0] = '\0';
	ast_verb(2, "ASTRINOISE: DeepFilterNet engine available (%s)\n", dfn.version());
}

static int engine_parse(const char *s)
{
	int i;

	if (ast_strlen_zero(s)) {
		return default_engine;
	}
	for (i = 0; i < ENG_COUNT; i++) {
		if (!strcasecmp(s, engine_names[i])) {
			return i;
		}
	}
	if (!strcasecmp(s, "dfn") || !strcasecmp(s, "deepfilternet")) {
		return ENG_DEEPFILTER;
	}
	return -1;
}

/* deepfilter asked for but not installed: clean with RNNoise rather than not at all. */
static int engine_usable(int e)
{
	static time_t warned;

	if (e == ENG_DEEPFILTER && !dfn.lib) {
		if (time(NULL) - warned > 60) {
			warned = time(NULL);
			ast_log(LOG_WARNING, "ASTRINOISE: deepfilter engine %s; using rnnoise\n", dfn.why);
		}
		return ENG_RNNOISE;
	}
	return e;
}

static void engine_free(struct rnn_dir *d)
{
	if (!d->st) {
		return;
	}
	if (d->engine == ENG_DEEPFILTER) {
		dfn.destroy(d->st);
	} else {
		rnnoise_destroy(d->st);
	}
	d->st = NULL;
}

static void dir_free(struct rnn_dir *d)
{
	if (!d) {
		return;
	}
	engine_free(d);
	if (d->up) {
		speex_resampler_destroy(d->up);
	}
	if (d->down) {
		speex_resampler_destroy(d->down);
	}
	ast_free(d->outq);
	ast_free(d->up_buf);
	ast_free(d->down_buf);
	__atomic_fetch_sub(&stats.e[d->engine].active, 1, __ATOMIC_RELAXED);
	ast_free(d);
}

static void destroy_callback(void *data)
{
	struct rnn_info *ri = data;

	ast_audiohook_destroy(&ri->audiohook);
	dir_free(ri->rx);
	dir_free(ri->tx);
	ast_free(ri);
}

static const struct ast_datastore_info rnn_datastore = {
	.type = "astrinoise",
	.destroy = destroy_callback
};

static int grow(int16_t **buf, int *cap, int need)
{
	int16_t *n;

	if (need <= *cap) {
		return 0;
	}
	if (!(n = ast_realloc(*buf, need * sizeof(int16_t)))) {
		return -1;
	}
	*buf = n;
	*cap = need;
	return 0;
}

/* Band fill for DeepFilterNet. A phone call upsampled to 48 kHz has nothing above 4 kHz (8 kHz
 * calls), and DFN3 cleans that worse: PESQ-NB 2.94 through our resamplers against 3.15 through
 * scipy's, whose filter leaves a faint floor up there. Quiet white noise high-passed at 0.625 x the
 * channel rate, added before DeepFilterNet only, gave 3.54 on the same files (lab/eval/dfn-bench
 * phone mode, 2026-11-24; flat from 0.02 to 0.1 RMS). The downsampler removes everything above
 * half the channel rate, so the caller never hears it, and the strength mix uses the original. */
static void bq_highpass(struct biquad *q, float f, float fs, float Q)
{
	float w = 2.0f * (float) M_PI * f / fs, al = sinf(w) / (2.0f * Q), c = cosf(w), a0 = 1.0f + al;

	q->b0 = (1.0f + c) / 2.0f / a0;
	q->b1 = -(1.0f + c) / a0;
	q->b2 = (1.0f + c) / 2.0f / a0;
	q->a1 = -2.0f * c / a0;
	q->a2 = (1.0f - al) / a0;
	q->z1 = q->z2 = 0.0f;
}

static inline float bq(struct biquad *q, float x)
{
	float y = q->b0 * x + q->z1;

	q->z1 = q->b1 * x - q->a1 * y + q->z2;
	q->z2 = q->b2 * x - q->a2 * y;
	return y;
}

static void fill_setup(struct rnn_dir *d, int rate)
{
	/* 8th-order Butterworth: four biquads with these Qs. */
	static const float qs[4] = { 0.5098f, 0.6013f, 0.9000f, 2.5629f };
	int i;

	d->fill = DFN_FILL > 0.0f && rate < 32000;
	d->rng = 1;
	for (i = 0; i < 4; i++) {
		bq_highpass(&d->hp[i], rate * 0.625f, ENG_RATE, qs[i]);
	}
}

/* (Re)build the per-direction state for a channel sample rate. */
static int dir_setup(struct rnn_dir *d, int rate)
{
	int err = 0, prime = rate * PRIME_MS / 1000;

	if (d->up) {
		speex_resampler_destroy(d->up);
	}
	if (d->down) {
		speex_resampler_destroy(d->down);
	}
	d->up = speex_resampler_init(1, rate, ENG_RATE, RESAMPLE_QUALITY, &err);
	if (!d->up || err) {
		d->up = NULL;
		return -1;
	}
	d->down = speex_resampler_init(1, ENG_RATE, rate, RESAMPLE_QUALITY, &err);
	if (!d->down || err) {
		d->down = NULL;
		return -1;
	}
#ifndef SKIP_ZEROS
#define SKIP_ZEROS 2          /* lab 2026-11-23: skip on the downsampler only = 50 ms fixed delay, no underruns */
#endif
	if (SKIP_ZEROS & 1) {
		speex_resampler_skip_zeros(d->up);
	}
	if (SKIP_ZEROS & 2) {
		speex_resampler_skip_zeros(d->down);
	}
	/* The DeepFilterNet state comes ready from rnn_write(); RNNoise's is cheap to make here. */
	if (!d->st && d->engine == ENG_RNNOISE && !(d->st = rnnoise_create(NULL))) {
		return -1;
	}
	fill_setup(d, rate);
	d->rate = rate;
	d->in48_n = 0;
	memset(d->dry48, 0, sizeof(d->dry48));
	if (grow(&d->outq, &d->outq_cap, prime * 4 + 4096)) {
		return -1;
	}
	memset(d->outq, 0, prime * sizeof(int16_t));   /* the fixed delay: starts as a little silence */
	d->outq_n = prime;
	return 0;
}

static inline int16_t clamp16(float v)
{
	if (v > 32767.0f) {
		return 32767;
	}
	if (v < -32768.0f) {
		return -32768;
	}
	return (int16_t) lrintf(v);
}

/* Run the engine on in48 into wet48. Returns 0, or -1 if it failed (the caller uses the original). */
static int engine_run(struct rnn_dir *d)
{
	float a[ENG_FRAME], b[ENG_FRAME];
	int i;

	if (!d->st) {
		return -1;
	}
	if (d->engine == ENG_RNNOISE) {
		rnnoise_process_frame(d->st, d->wet48, d->in48);
		return 0;
	}
	/* DeepFilterNet works on -1..1. */
	for (i = 0; i < ENG_FRAME; i++) {
		a[i] = d->in48[i] * (1.0f / 32768.0f);
		if (d->fill) {
			/* uniform white noise with the given RMS (sqrt(12) = 3.4641), then the high-pass */
			float w = DFN_FILL * 3.4641f * ((d->rng = d->rng * 1664525u + 1013904223u) / 4294967296.0f - 0.5f);
			int b;
			for (b = 0; b < 4; b++) {
				w = bq(&d->hp[b], w);
			}
			a[i] += w;
		}
	}
	if (dfn.process(d->st, a, b)) {
		return -1;
	}
	for (i = 0; i < ENG_FRAME; i++) {
		d->wet48[i] = b[i] * 32768.0f;
	}
	return 0;
}

/* Run one full 48 kHz engine frame and queue the result at the channel rate. */
static void run_frame(struct rnn_dir *d)
{
	spx_uint32_t in_len = ENG_FRAME, out_len;
	int need = (int) ((int64_t) ENG_FRAME * d->rate / ENG_RATE) + 64;
	int lag = engine_lag[d->engine];
	float mixed[ENG_FRAME];
	float s = d->strength / 100.0f;
	int i, ok;

	/* dry48 = the last `lag` input samples, then this frame: dry48[i] is the input the engine's
	 * output sample i belongs to. The mix must use it, or it comb-filters (a 10 ms mismatch scored
	 * worse than no filter at 60 %). */
	memcpy(d->dry48 + lag, d->in48, sizeof(d->in48));
	ok = !engine_run(d);
	if (!ok) {
		__atomic_fetch_add(&stats.e[d->engine].errors, 1, __ATOMIC_RELAXED);
	}
	for (i = 0; i < ENG_FRAME; i++) {
		float dry = d->dry48[i];
		mixed[i] = !ok ? dry : s >= 1.0f ? d->wet48[i] : dry + s * (d->wet48[i] - dry);
	}
	memmove(d->dry48, d->dry48 + ENG_FRAME, lag * sizeof(float));
	d->in48_n = 0;

	if (grow(&d->down_buf, &d->down_cap, need)) {
		return;
	}
	out_len = need;
	{
		int16_t tmp[ENG_FRAME];
		for (i = 0; i < ENG_FRAME; i++) {
			tmp[i] = clamp16(mixed[i]);
		}
		speex_resampler_process_int(d->down, 0, tmp, &in_len, d->down_buf, &out_len);
	}
	if (d->outq_n + (int) out_len > d->outq_cap) {
		if (grow(&d->outq, &d->outq_cap, d->outq_n + out_len + 1024)) {
			return;
		}
	}
	memcpy(d->outq + d->outq_n, d->down_buf, out_len * sizeof(int16_t));
	d->outq_n += out_len;
}

static void process(struct rnn_dir *d, int16_t *pcm, int n)
{
	spx_uint32_t in_len = n, up_len;
	int need = (int) ((int64_t) n * ENG_RATE / d->rate) + 64;
	int i, prime = d->rate * PRIME_MS / 1000;

	if (grow(&d->up_buf, &d->up_cap, need)) {
		return;
	}
	up_len = need;
	speex_resampler_process_int(d->up, 0, pcm, &in_len, d->up_buf, &up_len);
	for (i = 0; i < (int) up_len; i++) {
		d->in48[d->in48_n++] = d->up_buf[i];
		if (d->in48_n == ENG_FRAME) {
			run_frame(d);
		}
	}

	if (d->outq_n >= n) {
		memcpy(pcm, d->outq, n * sizeof(int16_t));
		d->outq_n -= n;
		memmove(d->outq, d->outq + n, d->outq_n * sizeof(int16_t));
	} else {
		/* Short (should not happen once primed): send what is ready, keep the original for the rest. */
		memcpy(pcm, d->outq, d->outq_n * sizeof(int16_t));
		d->outq_n = 0;
		__atomic_fetch_add(&stats.underruns, 1, __ATOMIC_RELAXED);
	}
	if (d->outq_n > prime * 3) {
		/* Never let the delay creep: drop the oldest audio beyond 3x the buffer. */
		int drop = d->outq_n - prime;
		memmove(d->outq, d->outq + drop, prime * sizeof(int16_t));
		d->outq_n = prime;
		__atomic_fetch_add(&stats.overflows, 1, __ATOMIC_RELAXED);
	}
}

static int rnn_callback(struct ast_audiohook *audiohook, struct ast_channel *chan, struct ast_frame *frame,
	enum ast_audiohook_direction direction)
{
	struct ast_datastore *datastore;
	struct rnn_info *ri;
	struct rnn_dir *d;
	struct eng_stats *es;
	uint64_t t0, ns;
	int rate;

	if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE || !frame || frame->frametype != AST_FRAME_VOICE) {
		return -1;
	}
	/* Called with the channel already locked. */
	if (!(datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
		return -1;
	}
	ri = datastore->data;
	d = (direction == AST_AUDIOHOOK_DIRECTION_READ) ? ri->rx : ri->tx;
	if (!d || !frame->data.ptr || frame->samples <= 0) {
		return -1;
	}
	rate = ast_format_get_sample_rate(frame->subclass.format);
	if (rate <= 0) {
		return -1;
	}

	t0 = now_ns();
	if (rate != d->rate && dir_setup(d, rate)) {
		return -1;
	}
	process(d, frame->data.ptr, frame->samples);
	ns = now_ns() - t0;

	es = &stats.e[d->engine];
	__atomic_fetch_add(&es->frames, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&es->ns_total, ns, __ATOMIC_RELAXED);
	max_ns(&es->ns_max, ns);
	if (ns > 2000000) {
		__atomic_fetch_add(&es->over2ms, 1, __ATOMIC_RELAXED);
	}
	if (ns > 5000000) {
		__atomic_fetch_add(&es->over5ms, 1, __ATOMIC_RELAXED);
	}
	if (ns > 10000000) {
		__atomic_fetch_add(&es->over10ms, 1, __ATOMIC_RELAXED);
	}
	return 0;
}

static int parse_strength(const char *value)
{
	int n;

	if (ast_strlen_zero(value) || ast_false(value)) {
		return 0;
	}
	if (ast_true(value)) {
		return 100;
	}
	if (sscanf(value, "%30d", &n) != 1) {
		return -1;
	}
	return n < 0 ? 0 : n > 100 ? 100 : n;
}

static int rnn_write(struct ast_channel *chan, const char *cmd, char *data, const char *value)
{
	struct ast_datastore *datastore;
	struct rnn_info *ri;
	struct rnn_dir **d;
	void *dfn_st = NULL;
	int is_new = 0, strength, engine;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(dir);
		AST_APP_ARG(engine);
	);

	if (!chan) {
		ast_log(LOG_WARNING, "No channel was provided to %s function.\n", cmd);
		return -1;
	}
	AST_STANDARD_APP_ARGS(args, data);
	if (ast_strlen_zero(args.dir) || (strcasecmp(args.dir, "rx") && strcasecmp(args.dir, "tx"))) {
		ast_log(LOG_ERROR, "%s needs rx or tx\n", cmd);
		return -1;
	}
	if ((engine = engine_parse(args.engine)) < 0) {
		ast_log(LOG_ERROR, "%s(%s,%s): engine must be rnnoise or deepfilter\n", cmd, args.dir, args.engine);
		return -1;
	}
	if ((strength = parse_strength(value)) < 0) {
		ast_log(LOG_ERROR, "%s(%s)=%s: use on, off or 0-100\n", cmd, args.dir, value);
		return -1;
	}
	engine = engine_usable(engine);
	if (strength && engine == ENG_DEEPFILTER) {
		/* Does this direction already run DeepFilterNet? Then it keeps its state. */
		int have = 0;
		ast_channel_lock(chan);
		if ((datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
			struct rnn_dir *cur = !strcasecmp(args.dir, "rx") ? ((struct rnn_info *) datastore->data)->rx
				: ((struct rnn_info *) datastore->data)->tx;
			have = cur && cur->engine == ENG_DEEPFILTER && cur->st;
		}
		ast_channel_unlock(chan);
		/* Get a state before taking the channel lock again: building one takes a while. */
		if (!have && !(dfn_st = dfn_take())) {
			ast_log(LOG_WARNING, "%s: no DeepFilterNet state; using rnnoise\n", cmd);
			engine = ENG_RNNOISE;
		}
	}

	ast_channel_lock(chan);
	if (!(datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
		if (!strength) {
			ast_channel_unlock(chan);
			return 0;
		}
		if (!(datastore = ast_datastore_alloc(&rnn_datastore, NULL))) {
			goto fail_unlock;
		}
		if (!(ri = ast_calloc(1, sizeof(*ri)))) {
			ast_datastore_free(datastore);
			goto fail_unlock;
		}
		ast_audiohook_init(&ri->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "astrinoise", AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
		ri->audiohook.manipulate_callback = rnn_callback;
		is_new = 1;
	} else {
		ri = datastore->data;
	}

	d = !strcasecmp(args.dir, "rx") ? &ri->rx : &ri->tx;
	if (strength && !*d) {
		if (!(*d = ast_calloc(1, sizeof(**d)))) {
			if (is_new) {
				datastore->data = ri;
				ast_channel_unlock(chan);
				ast_datastore_free(datastore);
				goto fail;
			}
			goto fail_unlock;
		}
		(*d)->engine = engine;
		__atomic_fetch_add(&stats.e[engine].active, 1, __ATOMIC_RELAXED);
	}
	if (*d) {
		(*d)->strength = strength;
		if (!strength) {
			dir_free(*d);
			*d = NULL;
		} else if ((*d)->engine != engine || (engine == ENG_DEEPFILTER && !(*d)->st)) {
			/* Engine switch on a running direction: start again from a clean buffer on the next frame. */
			engine_free(*d);
			__atomic_fetch_sub(&stats.e[(*d)->engine].active, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&stats.e[engine].active, 1, __ATOMIC_RELAXED);
			(*d)->engine = engine;
			(*d)->rate = 0;
		}
		if (*d && engine == ENG_DEEPFILTER && !(*d)->st) {
			(*d)->st = dfn_st;
			dfn_st = NULL;
		}
	}

	if (!ri->rx && !ri->tx) {
		if (is_new) {
			datastore->data = ri;
			ast_channel_unlock(chan);
		} else {
			ast_channel_datastore_remove(chan, datastore);
			ast_audiohook_remove(chan, &ri->audiohook);
			ast_channel_unlock(chan);
			ast_audiohook_detach(&ri->audiohook);
		}
		ast_datastore_free(datastore);
		goto done;
	}

	if (is_new) {
		datastore->data = ri;
		ast_channel_datastore_add(chan, datastore);
		ast_channel_unlock(chan);
		ast_audiohook_attach(chan, &ri->audiohook);
	} else {
		ast_channel_unlock(chan);
	}
done:
	if (dfn_st) {
		dfn.destroy(dfn_st);      /* this direction already had a DeepFilterNet state */
	}
	return 0;

fail_unlock:
	ast_channel_unlock(chan);
fail:
	if (dfn_st) {
		dfn.destroy(dfn_st);
	}
	return -1;
}

static int rnn_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct ast_datastore *datastore;
	struct rnn_info *ri;
	struct rnn_dir *d = NULL;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(dir);
		AST_APP_ARG(what);
	);

	AST_STANDARD_APP_ARGS(args, data);
	if (!strcasecmp(S_OR(args.dir, ""), "engines")) {
		snprintf(buf, len, "rnnoise%s", dfn.lib ? ",deepfilter" : "");
		return 0;
	}
	if (!chan) {
		return -1;
	}
	ast_channel_lock(chan);
	if ((datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
		ri = datastore->data;
		d = !strcasecmp(S_OR(args.dir, ""), "tx") ? ri->tx : ri->rx;
	}
	if (!strcasecmp(S_OR(args.what, ""), "engine")) {
		snprintf(buf, len, "%s", d ? engine_names[d->engine] : "");
	} else {
		snprintf(buf, len, "%d", d ? d->strength : 0);
	}
	ast_channel_unlock(chan);
	return 0;
}

static struct ast_custom_function rnnoise_function = {
	.name = "ASTRINOISE",
	.write = rnn_write,
	.read = rnn_read,
	.read_max = 32,
};

static char *handle_show_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	uint64_t frames = 0, ns_total = 0, ns_max = 0, o2 = 0, o5 = 0, o10 = 0, built;
	int64_t active = 0;
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "astrinoise show stats";
		e->usage = "Usage: astrinoise show stats\n"
			"       CPU time per frame and buffer health of ASTRINOISE() since load or reset:\n"
			"       first all engines together, then one line per engine.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	for (i = 0; i < ENG_COUNT; i++) {
		struct eng_stats *s = &stats.e[i];
		active += __atomic_load_n(&s->active, __ATOMIC_RELAXED);
		frames += __atomic_load_n(&s->frames, __ATOMIC_RELAXED);
		ns_total += __atomic_load_n(&s->ns_total, __ATOMIC_RELAXED);
		if (s->ns_max > ns_max) {
			ns_max = s->ns_max;
		}
		o2 += s->over2ms;
		o5 += s->over5ms;
		o10 += s->over10ms;
	}
	ast_cli(a->fd, "active=%lld frames=%llu avg_us=%.1f max_us=%.1f over2ms=%llu over5ms=%llu over10ms=%llu underruns=%llu overflows=%llu\n",
		(long long) active, (unsigned long long) frames, frames ? ns_total / 1000.0 / frames : 0.0, ns_max / 1000.0,
		(unsigned long long) o2, (unsigned long long) o5, (unsigned long long) o10,
		(unsigned long long) stats.underruns, (unsigned long long) stats.overflows);
	for (i = 0; i < ENG_COUNT; i++) {
		struct eng_stats *s = &stats.e[i];
		uint64_t f = __atomic_load_n(&s->frames, __ATOMIC_RELAXED);
		ast_cli(a->fd, "engine %s: active=%lld frames=%llu avg_us=%.1f max_us=%.1f over2ms=%llu over5ms=%llu over10ms=%llu errors=%llu lag_ms=%.0f%s%s\n",
			engine_names[i], (long long) s->active, (unsigned long long) f, f ? s->ns_total / 1000.0 / f : 0.0,
			s->ns_max / 1000.0, (unsigned long long) s->over2ms, (unsigned long long) s->over5ms,
			(unsigned long long) s->over10ms, (unsigned long long) s->errors, engine_lag[i] * 1000.0 / ENG_RATE,
			i == default_engine ? " default" : "", i == ENG_DEEPFILTER && !dfn.lib ? " unavailable" : "");
	}
	if (dfn.lib) {
		built = __atomic_load_n(&dfn.built, __ATOMIC_RELAXED);
		ast_cli(a->fd, "deepfilter: %s, states_made=%llu make_avg_ms=%.1f make_max_ms=%.1f failed=%llu\n",
			dfn.version(), (unsigned long long) built,
			built ? dfn.build_ns_total / 1e6 / built : 0.0, dfn.build_ns_max / 1e6, (unsigned long long) dfn.build_fail);
	} else {
		ast_cli(a->fd, "deepfilter: %s\n", dfn.why);
	}
	return CLI_SUCCESS;
}

static char *handle_reset_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int64_t active[ENG_COUNT];
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "astrinoise reset stats";
		e->usage = "Usage: astrinoise reset stats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	for (i = 0; i < ENG_COUNT; i++) {
		active[i] = __atomic_load_n(&stats.e[i].active, __ATOMIC_RELAXED);
	}
	memset(&stats, 0, sizeof(stats));
	for (i = 0; i < ENG_COUNT; i++) {
		stats.e[i].active = active[i];
	}
	ast_cli(a->fd, "astrinoise stats reset\n");
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_rnnoise[] = {
	AST_CLI_DEFINE(handle_show_stats, "Show ASTRINOISE() timing"),
	AST_CLI_DEFINE(handle_reset_stats, "Reset ASTRINOISE() timing"),
};

/* astrinoise.conf is optional:
 *   [general]
 *   engine = rnnoise                  ; engine for ASTRINOISE(rx) without one: rnnoise or deepfilter
 *   deepfilter_library = /usr/lib/astrinoise/libastrinoise_dfn.so
 */
static void load_config(int reload)
{
	struct ast_flags flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };
	struct ast_config *cfg = ast_config_load("astrinoise.conf", flags);
	struct ast_variable *v;
	int engine = ENG_RNNOISE;

	if (cfg == CONFIG_STATUS_FILEUNCHANGED) {
		return;
	}
	if (cfg && cfg != CONFIG_STATUS_FILEINVALID) {
		for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
			if (!strcasecmp(v->name, "engine")) {
				if ((engine = engine_parse(v->value)) < 0) {
					ast_log(LOG_WARNING, "astrinoise.conf: engine must be rnnoise or deepfilter\n");
					engine = ENG_RNNOISE;
				}
			} else if (!strcasecmp(v->name, "deepfilter_library")) {
				if (!dfn.lib) {
					ast_copy_string(dfn_path, v->value, sizeof(dfn_path));
				}
			}
		}
		ast_config_destroy(cfg);
	} else if (cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_WARNING, "astrinoise.conf is not valid; using the defaults\n");
	}
	default_engine = engine;
	if (!dfn.lib) {
		dfn_load();
		if (dfn.lib && !big_stack_thread(&dfn.warm, dfn_warm, NULL)) {
			dfn.warm_up = 1;
		}
	}
}

static int reload_module(void)
{
	load_config(1);
	return 0;
}

static int unload_module(void)
{
	int res;

	ast_cli_unregister_multiple(cli_rnnoise, ARRAY_LEN(cli_rnnoise));
	res = ast_custom_function_unregister(&rnnoise_function);
	if (dfn.warm_up) {
		pthread_join(dfn.warm, NULL);
		dfn.warm_up = 0;
	}
	return res;
}

static int load_module(void)
{
	if (rnnoise_get_frame_size() != ENG_FRAME) {
		ast_log(LOG_ERROR, "RNNoise frame size is %d, expected %d\n", rnnoise_get_frame_size(), ENG_FRAME);
		return AST_MODULE_LOAD_DECLINE;
	}
	load_config(0);
	if (ast_custom_function_register(&rnnoise_function)) {
		unload_module();
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_cli_register_multiple(cli_rnnoise, ARRAY_LEN(cli_rnnoise));
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Astri-noise: RNNoise and DeepFilterNet noise suppression (ASTRINOISE function)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.reload = reload_module,
);
