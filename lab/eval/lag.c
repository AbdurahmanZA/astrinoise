/* Measure RNNoise 0.2's output delay: feed 48 kHz s16 mono from stdin in 480-sample frames,
   write the processed frames to stdout. Then cross-correlate in Python. */
#include <stdio.h>
#include <stdint.h>
#include "rnnoise.h"
int main(void) {
    DenoiseState *st = rnnoise_create(NULL);
    int16_t b[480]; float in[480], out[480];
    while (fread(b, 2, 480, stdin) == 480) {
        for (int i = 0; i < 480; i++) in[i] = b[i];
        rnnoise_process_frame(st, out, in);
        for (int i = 0; i < 480; i++) { float v = out[i]; b[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t) v; }
        fwrite(b, 2, 480, stdout);
    }
    rnnoise_destroy(st);
    return 0;
}
