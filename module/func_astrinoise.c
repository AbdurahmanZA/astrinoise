/*
 * func_astrinoise (Astri-noise) - RNNoise noise suppression on a channel, inside Asterisk.
 *
 * Copyright (C) 2026, SANDZ (Solomon and Zoutenburg (Pty) Ltd).
 *
 * This program is free software, distributed under the terms of the GNU General Public License
 * Version 2, like Asterisk itself. RNNoise (xiph, BSD-3-Clause) is linked in statically; the
 * speexdsp resampler (BSD) comes from the system's libspeexdsp, as func_speex already uses.
 *
 * Why a module: an earlier design sent call audio out of Asterisk to a Python engine and back.
 * Under load that engine sometimes answered 50-150 ms late, and callers heard gaps and
 * static. Here the cleaning runs in the channel's own audio path, the same way DENOISE() (func_speex)
 * does, so there is no trip out and back and nothing can arrive late:
 *
 *   every frame in -> upsample to 48 kHz -> RNNoise in 10 ms frames -> downsample -> FIFO -> frame out
 *
 * The FIFO starts with PRIME_MS of silence, so the output always has exactly as many samples as the
 * input (a fixed delay of PRIME_MS + RNNoise's 20 ms + the resamplers, ~50 ms at 8 kHz). If it ever runs short, the original audio fills the gap
 * (never silence) and the shortfall is counted. `astrinoise show stats` shows the CPU time per frame.
 *
 * Usage (dialplan or ARI channel variable):
 *   Set(ASTRINOISE(rx)=on)     clean the audio Asterisk receives from this channel (the agent's voice)
 *   Set(ASTRINOISE(rx)=60)     60 % strength (mixes 40 % of the original back in, time-aligned)
 *   Set(ASTRINOISE(rx)=off)    stop
 *   ${ASTRINOISE(rx)}          current strength, 0 = off
 */

/*** MODULEINFO
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <math.h>
#include <stdint.h>
#include <time.h>
#include <speex/speex_resampler.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/audiohook.h"
#include "asterisk/cli.h"

#include "rnnoise.h"

#define RNN_RATE 48000
#define RNN_FRAME 480                 /* 10 ms at 48 kHz */
#define RNN_LAG 2                     /* RNNoise 0.2 output lags its input by 2 frames (960 samples, measured) */
#define PRIME_MS 20                   /* fixed buffer: output never waits on a partly filled RNNoise frame */
#ifndef RESAMPLE_QUALITY
/* speexdsp 0-10. Lab 2026-11-23: 8 removes ~22 dB of fan noise vs ~13 dB at 4, for +0.02 ms CPU a frame. */
#define RESAMPLE_QUALITY 8
#endif

struct rnn_dir {
	DenoiseState *st;
	SpeexResamplerState *up, *down;
	int rate;                         /* channel sample rate this state is set up for */
	int strength;                     /* 1-100 */
	float in48[RNN_FRAME];            /* upsampled input waiting for a full RNNoise frame */
	int in48_n;
	float dry48[RNN_LAG][RNN_FRAME];  /* the last RNN_LAG input frames; dry48[RNN_LAG-1] lines up with RNNoise's output */
	float wet48[RNN_FRAME];
	int16_t *outq;                    /* cleaned audio at the channel rate, waiting to go out */
	int outq_n, outq_cap;
	int16_t *up_buf;                  /* scratch for upsampled audio */
	int up_cap;
	int16_t *down_buf;                /* scratch for downsampled audio */
	int down_cap;
};

struct rnn_info {
	struct ast_audiohook audiohook;
	struct rnn_dir *tx, *rx;
};

/* Counters for `astrinoise show stats` (all channels together). */
static struct {
	uint64_t frames, ns_total, ns_max, over2ms, over5ms, over10ms, underruns, overflows;
	int64_t active;
} stats;

static void dir_free(struct rnn_dir *d)
{
	if (!d) {
		return;
	}
	if (d->st) {
		rnnoise_destroy(d->st);
	}
	if (d->up) {
		speex_resampler_destroy(d->up);
	}
	if (d->down) {
		speex_resampler_destroy(d->down);
	}
	ast_free(d->outq);
	ast_free(d->up_buf);
	ast_free(d->down_buf);
	ast_free(d);
	__atomic_fetch_sub(&stats.active, 1, __ATOMIC_RELAXED);
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
	d->up = speex_resampler_init(1, rate, RNN_RATE, RESAMPLE_QUALITY, &err);
	if (!d->up || err) {
		d->up = NULL;
		return -1;
	}
	d->down = speex_resampler_init(1, RNN_RATE, rate, RESAMPLE_QUALITY, &err);
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
	if (!d->st && !(d->st = rnnoise_create(NULL))) {
		return -1;
	}
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

/* Run one full 48 kHz RNNoise frame and queue the result at the channel rate. */
static void run_frame(struct rnn_dir *d)
{
	spx_uint32_t in_len = RNN_FRAME, out_len;
	int need = (int) ((int64_t) RNN_FRAME * d->rate / RNN_RATE) + 64;
	float mixed[RNN_FRAME];
	float s = d->strength / 100.0f;
	int i;

	rnnoise_process_frame(d->st, d->wet48, d->in48);
	/* RNNoise's output lags its input by RNN_LAG frames: blend with the input from that many frames
	 * ago, or the mix comb-filters (a 1-frame mismatch scored worse than no filter at 60 %). */
	for (i = 0; i < RNN_FRAME; i++) {
		float dry = d->dry48[RNN_LAG - 1][i];
		mixed[i] = s >= 1.0f ? d->wet48[i] : dry + s * (d->wet48[i] - dry);
	}
	memmove(d->dry48[1], d->dry48[0], (RNN_LAG - 1) * sizeof(d->dry48[0]));
	memcpy(d->dry48[0], d->in48, sizeof(d->dry48[0]));
	d->in48_n = 0;

	if (grow(&d->down_buf, &d->down_cap, need)) {
		return;
	}
	out_len = need;
	{
		int16_t tmp[RNN_FRAME];
		for (i = 0; i < RNN_FRAME; i++) {
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
	int need = (int) ((int64_t) n * RNN_RATE / d->rate) + 64;
	int i, prime = d->rate * PRIME_MS / 1000;

	if (grow(&d->up_buf, &d->up_cap, need)) {
		return;
	}
	up_len = need;
	speex_resampler_process_int(d->up, 0, pcm, &in_len, d->up_buf, &up_len);
	for (i = 0; i < (int) up_len; i++) {
		d->in48[d->in48_n++] = d->up_buf[i];
		if (d->in48_n == RNN_FRAME) {
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
	struct timespec t0, t1;
	uint64_t ns, prev;
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

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (rate != d->rate && dir_setup(d, rate)) {
		return -1;
	}
	process(d, frame->data.ptr, frame->samples);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	ns = (uint64_t) (t1.tv_sec - t0.tv_sec) * 1000000000ULL + (t1.tv_nsec - t0.tv_nsec);
	__atomic_fetch_add(&stats.frames, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.ns_total, ns, __ATOMIC_RELAXED);
	prev = __atomic_load_n(&stats.ns_max, __ATOMIC_RELAXED);
	while (ns > prev && !__atomic_compare_exchange_n(&stats.ns_max, &prev, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
	if (ns > 2000000) {
		__atomic_fetch_add(&stats.over2ms, 1, __ATOMIC_RELAXED);
	}
	if (ns > 5000000) {
		__atomic_fetch_add(&stats.over5ms, 1, __ATOMIC_RELAXED);
	}
	if (ns > 10000000) {
		__atomic_fetch_add(&stats.over10ms, 1, __ATOMIC_RELAXED);
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
	int is_new = 0, strength;

	if (!chan) {
		ast_log(LOG_WARNING, "No channel was provided to %s function.\n", cmd);
		return -1;
	}
	if (strcasecmp(data, "rx") && strcasecmp(data, "tx")) {
		ast_log(LOG_ERROR, "%s needs rx or tx\n", cmd);
		return -1;
	}
	if ((strength = parse_strength(value)) < 0) {
		ast_log(LOG_ERROR, "%s(%s)=%s: use on, off or 0-100\n", cmd, data, value);
		return -1;
	}

	ast_channel_lock(chan);
	if (!(datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
		if (!strength) {
			ast_channel_unlock(chan);
			return 0;
		}
		if (!(datastore = ast_datastore_alloc(&rnn_datastore, NULL))) {
			ast_channel_unlock(chan);
			return -1;
		}
		if (!(ri = ast_calloc(1, sizeof(*ri)))) {
			ast_channel_unlock(chan);
			ast_datastore_free(datastore);
			return -1;
		}
		ast_audiohook_init(&ri->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, "astrinoise", AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
		ri->audiohook.manipulate_callback = rnn_callback;
		is_new = 1;
	} else {
		ri = datastore->data;
	}

	d = !strcasecmp(data, "rx") ? &ri->rx : &ri->tx;
	if (strength && !*d) {
		if (!(*d = ast_calloc(1, sizeof(**d)))) {
			if (is_new) {
				datastore->data = ri;
				ast_channel_unlock(chan);
				ast_datastore_free(datastore);
			} else {
				ast_channel_unlock(chan);
			}
			return -1;
		}
		__atomic_fetch_add(&stats.active, 1, __ATOMIC_RELAXED);
	}
	if (*d) {
		(*d)->strength = strength;
		if (!strength) {
			dir_free(*d);
			*d = NULL;
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
		return 0;
	}

	if (is_new) {
		datastore->data = ri;
		ast_channel_datastore_add(chan, datastore);
		ast_channel_unlock(chan);
		ast_audiohook_attach(chan, &ri->audiohook);
	} else {
		ast_channel_unlock(chan);
	}
	return 0;
}

static int rnn_read(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct ast_datastore *datastore;
	struct rnn_info *ri;
	struct rnn_dir *d = NULL;

	if (!chan) {
		return -1;
	}
	ast_channel_lock(chan);
	if ((datastore = ast_channel_datastore_find(chan, &rnn_datastore, NULL))) {
		ri = datastore->data;
		d = !strcasecmp(data, "tx") ? ri->tx : ri->rx;
	}
	snprintf(buf, len, "%d", d ? d->strength : 0);
	ast_channel_unlock(chan);
	return 0;
}

static struct ast_custom_function rnnoise_function = {
	.name = "ASTRINOISE",
	.write = rnn_write,
	.read = rnn_read,
	.read_max = 8,
};

static char *handle_show_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	uint64_t frames, ns_total;

	switch (cmd) {
	case CLI_INIT:
		e->command = "astrinoise show stats";
		e->usage = "Usage: astrinoise show stats\n       CPU time per frame and buffer health of ASTRINOISE() since load or reset.\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	frames = __atomic_load_n(&stats.frames, __ATOMIC_RELAXED);
	ns_total = __atomic_load_n(&stats.ns_total, __ATOMIC_RELAXED);
	ast_cli(a->fd, "active=%lld frames=%llu avg_us=%.1f max_us=%.1f over2ms=%llu over5ms=%llu over10ms=%llu underruns=%llu overflows=%llu\n",
		(long long) __atomic_load_n(&stats.active, __ATOMIC_RELAXED), (unsigned long long) frames,
		frames ? ns_total / 1000.0 / frames : 0.0, __atomic_load_n(&stats.ns_max, __ATOMIC_RELAXED) / 1000.0,
		(unsigned long long) stats.over2ms, (unsigned long long) stats.over5ms, (unsigned long long) stats.over10ms,
		(unsigned long long) stats.underruns, (unsigned long long) stats.overflows);
	return CLI_SUCCESS;
}

static char *handle_reset_stats(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int64_t active;

	switch (cmd) {
	case CLI_INIT:
		e->command = "astrinoise reset stats";
		e->usage = "Usage: astrinoise reset stats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}
	active = __atomic_load_n(&stats.active, __ATOMIC_RELAXED);
	memset(&stats, 0, sizeof(stats));
	stats.active = active;
	ast_cli(a->fd, "astrinoise stats reset\n");
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_rnnoise[] = {
	AST_CLI_DEFINE(handle_show_stats, "Show ASTRINOISE() timing"),
	AST_CLI_DEFINE(handle_reset_stats, "Reset ASTRINOISE() timing"),
};

static int unload_module(void)
{
	ast_cli_unregister_multiple(cli_rnnoise, ARRAY_LEN(cli_rnnoise));
	return ast_custom_function_unregister(&rnnoise_function);
}

static int load_module(void)
{
	if (rnnoise_get_frame_size() != RNN_FRAME) {
		ast_log(LOG_ERROR, "RNNoise frame size is %d, expected %d\n", rnnoise_get_frame_size(), RNN_FRAME);
		return AST_MODULE_LOAD_DECLINE;
	}
	if (ast_custom_function_register(&rnnoise_function)) {
		return AST_MODULE_LOAD_DECLINE;
	}
	ast_cli_register_multiple(cli_rnnoise, ARRAY_LEN(cli_rnnoise));
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD_EXTENDED(ASTERISK_GPL_KEY, "Astri-noise: RNNoise noise suppression (ASTRINOISE function)");
