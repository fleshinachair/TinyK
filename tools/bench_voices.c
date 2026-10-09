/* How much CPU the engine takes with every voice sounding: each built-in program, 8 keys held, 128-frame blocks.
 *
 *     cc -O3 -Isrc/dsp tools/bench_voices.c src/dsp/dsp.c -lm -o /tmp/bench && /tmp/bench
 *     (-DNUM_VOICES=4 for the old voice count; build for the Move with the flags in scripts/build.sh)
 *
 * Prints the mean and the slowest block per program as a share of the block's own duration (128 frames at
 * MOVE_SAMPLE_RATE), worst programs first. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsp.h"

static synth_engine_t S;

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec * 1e-3;
}

typedef struct { int preset; double mean, worst; } row_t;

static int by_mean(const void *a, const void *b) {
    double d = ((const row_t *)b)->mean - ((const row_t *)a)->mean;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

int main(void) {
    static const uint8_t keys[8] = { 36, 43, 48, 52, 55, 60, 64, 67 };
    const double block_us = 128.0 * 1e6 / (double)MOVE_SAMPLE_RATE;
    const int blocks = (int)(4.0 * MOVE_SAMPLE_RATE / 128.0);
    static row_t rows[128];
    int16_t buf[256];
    double all = 0.0;
    for (int p = 0; p < 128; p++) {
        synth_init(&S);
        synth_load_preset(&S, p);
        for (int k = 0; k < 8; k++) synth_note_on(&S, keys[k], 100);
        double sum = 0.0, worst = 0.0;
        for (int b = 0; b < blocks; b++) {
            if (b == blocks / 2) for (int k = 0; k < 8; k++) synth_note_on(&S, keys[k], 100); /* retrigger: decayed programs sound again */
            double t0 = now_us();
            synth_render(&S, buf, 128);
            double dt = now_us() - t0;
            sum += dt;
            if (dt > worst) worst = dt;
        }
        rows[p].preset = p;
        rows[p].mean = sum / blocks / block_us;
        rows[p].worst = worst / block_us;
        all += rows[p].mean;
    }
    qsort(rows, 128, sizeof rows[0], by_mean);
    printf("NUM_VOICES %d, block %.0f us. Share of real time, all 128 programs: mean %.2f %%, heaviest program %.2f %%\n",
           NUM_VOICES, block_us, 100.0 * all / 128.0, 100.0 * rows[0].mean);
    for (int i = 0; i < 5; i++) printf("  program %3d: mean %.2f %%, slowest block %.2f %%\n", rows[i].preset, 100.0 * rows[i].mean, 100.0 * rows[i].worst);
    return 0;
}
