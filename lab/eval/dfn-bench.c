/* Lab tool for libastrinoise_dfn.so, without Asterisk.
 *   dfn-bench LIB build N    build N states: time per build and memory per state
 *   dfn-bench LIB pipe       48 kHz s16 mono on stdin -> cleaned on stdout (for lag.py)
 *   dfn-bench LIB speed      48 kHz s16 mono on stdin: CPU per 10 ms frame, average and slowest
 *   dfn-bench LIB phone Q    8 kHz s16 mono on stdin -> 8 kHz cleaned on stdout, through the same speexdsp
 *                            resampling as func_astrinoise (quality Q, skip_zeros on the downsampler).
 *                            DFN_DITHER=<rms> adds white noise before DeepFilterNet, DFN_DITHER_HP=<Hz>
 *                            high-passes it first: that is how we found the module's band fill.
 *   dfn-bench LIB xtalk      48 kHz s16 mono on stdin: two threads clean the two halves at once, each with
 *                            its own state; the first must match one state alone exactly (no shared buffers)
 * gcc -O2 -o dfn-bench dfn-bench.c -ldl -lpthread -lm -lspeexdsp */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <speex/speex_resampler.h>

static void *(*dnew)(float);
static int (*dlen)(const void *);
static int (*dproc)(void *, const float *, float *);
static void (*dfree)(void *);

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static long rss_kb(void)
{
	char l[256]; long v = -1; FILE *f = fopen("/proc/self/status", "r");
	while (f && fgets(l, sizeof(l), f)) if (!strncmp(l, "VmRSS:", 6)) v = atol(l + 6);
	if (f) fclose(f);
	return v;
}

struct biquad { float b0, b1, b2, a1, a2, z1, z2; };
static void bq_highpass(struct biquad *q, float f, float fs, float Q)
{
	float w = 2 * 3.14159265f * f / fs, al = sinf(w) / (2 * Q), c = cosf(w), a0 = 1 + al;
	q->b0 = (1 + c) / 2 / a0; q->b1 = -(1 + c) / a0; q->b2 = (1 + c) / 2 / a0;
	q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0; q->z1 = q->z2 = 0;
}
static float bq(struct biquad *q, float x)
{
	float y = q->b0 * x + q->z1;
	q->z1 = q->b1 * x - q->a1 * y + q->z2;
	q->z2 = q->b2 * x - q->a2 * y;
	return y;
}

struct job { const int16_t *raw; long start, len; long frames; float *out; };

static void *run_job(void *p)
{
	struct job *j = p;
	void *st = dnew(100.0f);
	int n = dlen(st);
	float in[480];
	j->frames = j->len / n;
	j->out = calloc(j->frames * n, sizeof(float));
	for (long f = 0; f < j->frames; f++) {
		for (int i = 0; i < n; i++) in[i] = j->raw[j->start + f * n + i] / 32768.0f;
		dproc(st, in, j->out + f * n);
	}
	dfree(st);
	return NULL;
}

int main(int argc, char **argv)
{
	void *lib;
	if (argc < 3 || !(lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL))) { fprintf(stderr, "usage: dfn-bench LIB build N|pipe|speed (%s)\n", dlerror()); return 1; }
	dnew = dlsym(lib, "astrinoise_dfn_new"); dlen = dlsym(lib, "astrinoise_dfn_frame_len");
	dproc = dlsym(lib, "astrinoise_dfn_process"); dfree = dlsym(lib, "astrinoise_dfn_free");
	const char *(*ver)(void) = dlsym(lib, "astrinoise_dfn_version");
	if (!strcmp(argv[2], "build")) {
		int n = argc > 3 ? atoi(argv[3]) : 5;
		void **st = calloc(n, sizeof(*st));
		long r0 = rss_kb();
		fprintf(stderr, "%s\n", ver());
		for (int i = 0; i < n; i++) {
			double t = now();
			st[i] = dnew(100.0f);
			printf("build %d: %.0f ms, frame %d, rss %ld kB\n", i, (now() - t) * 1000, dlen(st[i]), rss_kb());
		}
		printf("memory per state: %ld kB (after the first)\n", n > 1 ? (rss_kb() - r0) / n : 0);
		for (int i = 0; i < n; i++) dfree(st[i]);
		printf("after free: rss %ld kB\n", rss_kb());
		return 0;
	}
	if (!strcmp(argv[2], "phone")) {
		int q = argc > 3 ? atoi(argv[3]) : 8, err = 0, n48 = 0;
		SpeexResamplerState *up = speex_resampler_init(1, 8000, 48000, q, &err), *down = speex_resampler_init(1, 48000, 8000, q, &err);
		void *st = dnew(100.0f);
		int16_t in8[160], u[1200], d8[400], o16[480];
		float f48[480], o48[480];
		float dither = getenv("DFN_DITHER") ? atof(getenv("DFN_DITHER")) : 0;   /* lab: white noise rms, -1..1 scale */
		uint32_t rng = 1;
		int hp = getenv("DFN_DITHER_HP") != NULL;     /* lab: high-pass the noise (8th order at DFN_DITHER_HP Hz) */
		struct biquad H[4];
		for (int b = 0; b < 4; b++) bq_highpass(&H[b], hp ? atof(getenv("DFN_DITHER_HP")) : 5000, 48000, (float[]){0.5098f, 0.6013f, 0.9000f, 2.5629f}[b]);
		speex_resampler_skip_zeros(down);
		while (fread(in8, 2, 160, stdin) == 160) {
			spx_uint32_t il = 160, ol = 1200;
			speex_resampler_process_int(up, 0, in8, &il, u, &ol);
			for (spx_uint32_t i = 0; i < ol; i++) {
				float w = dither * ((rng = rng * 1664525u + 1013904223u) / 4294967296.0f - 0.5f) * 3.4641f;
				if (hp) { for (int b = 0; b < 4; b++) w = bq(&H[b], w); }
				f48[n48++] = u[i] / 32768.0f + w;
				if (n48 == 480) {
					spx_uint32_t il2 = 480, ol2 = 400;
					dproc(st, f48, o48);
					for (int k = 0; k < 480; k++) { float v = o48[k] * 32768.0f; o16[k] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) lrintf(v); }
					speex_resampler_process_int(down, 0, o16, &il2, d8, &ol2);
					fwrite(d8, 2, ol2, stdout);
					n48 = 0;
				}
			}
		}
		return 0;
	}
	if (!strcmp(argv[2], "xtalk")) {
		/* Thread 1 cleans the first half with state a while thread 2 cleans the second half with
		 * state b, both flat out. a's output must match a state that cleaned the first half alone. */
		static int16_t raw[48000 * 120];
		size_t got = fread(raw, 2, sizeof(raw) / 2, stdin);
		struct job j1 = { raw, 0, (long) got / 2 }, j2 = { raw, (long) got / 2, (long) got / 2 }, solo = j1;
		pthread_t t1, t2;
		run_job(&solo);
		pthread_create(&t1, NULL, run_job, &j1);
		pthread_create(&t2, NULL, run_job, &j2);
		pthread_join(t1, NULL);
		pthread_join(t2, NULL);
		double d = 0;
		for (long i = 0; i < solo.frames * 480; i++) if (fabs(solo.out[i] - j1.out[i]) > d) d = fabs(solo.out[i] - j1.out[i]);
		printf("xtalk: %ld frames, largest difference %.3g: %s\n", solo.frames, d, d == 0 ? "independent" : "STATES INTERFERE");
		return d != 0;
	}
	void *st = dnew(100.0f);
	int n = dlen(st);
	int16_t b[4800]; float in[4800], out[4800];
	double tot = 0, mx = 0; long frames = 0;
	while (fread(b, 2, n, stdin) == (size_t) n) {
		for (int i = 0; i < n; i++) in[i] = b[i] / 32768.0f;
		double t = now();
		dproc(st, in, out);
		t = now() - t; tot += t; if (t > mx) mx = t; frames++;
		if (!strcmp(argv[2], "pipe")) {
			for (int i = 0; i < n; i++) { float v = out[i] * 32768.0f; b[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) v; }
			fwrite(b, 2, n, stdout);
		}
	}
	fprintf(stderr, "%ld frames of %d samples: avg %.3f ms, slowest %.3f ms per 10 ms frame\n", frames, n, frames ? tot / frames * 1000 : 0, mx * 1000);
	dfree(st);
	return 0;
}
