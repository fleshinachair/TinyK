/* How hard the programs drive the engine's two non-linear stages: the level into the voice-mix clip (tanh) and into
 * the output limiter (linear to 0.65), per built-in program, for 1, 4 and 8 held keys.
 *     cc -O2 -DTINYK_LEVEL_PROBE -Isrc/dsp tools/probe_levels.c src/dsp/dsp.c -lm -o /tmp/probe && /tmp/probe */
#include <stdio.h>
#include <stdlib.h>
#include "dsp.h"
extern float tinyk_probe_mix, tinyk_probe_out;
static synth_engine_t S;
static int cmp(const void *a, const void *b) { float d = *(const float *)a - *(const float *)b; return d > 0 ? 1 : d < 0 ? -1 : 0; }
int main(void) {
    static const uint8_t keys[8] = { 48, 55, 60, 64, 67, 72, 76, 79 };
    static const int counts[3] = { 1, 4, 8 };
    int16_t buf[256];
    for (int c = 0; c < 3; c++) {
        static float mix[128], out[128];
        for (int p = 0; p < 128; p++) {
            synth_init(&S);
            synth_load_preset(&S, p);
            for (int k = 0; k < counts[c]; k++) synth_note_on(&S, keys[k], 100);
            tinyk_probe_mix = tinyk_probe_out = 0.0f;
            for (int b = 0; b < 700; b++) synth_render(&S, buf, 128);   /* 2 s */
            mix[p] = tinyk_probe_mix; out[p] = tinyk_probe_out;
        }
        int over_mix = 0, hard_mix = 0, over_out = 0;
        for (int p = 0; p < 128; p++) { over_mix += mix[p] > 0.5f; hard_mix += mix[p] > 1.0f; over_out += out[p] > 0.65f; }
        qsort(mix, 128, sizeof mix[0], cmp); qsort(out, 128, sizeof out[0], cmp);
        printf("%d key%s: into the mix clip: median %.2f, 90th pct %.2f, max %.2f; programs past 0.5 (tanh 8 %% down): %d, past 1.0 (24 %% down): %d | "
               "into the limiter: median %.2f, max %.2f; programs past its 0.65 knee: %d\n",
               counts[c], counts[c] > 1 ? "s" : "", mix[64], mix[115], mix[127], over_mix, hard_mix, out[64], out[127], over_out);
    }
    return 0;
}
