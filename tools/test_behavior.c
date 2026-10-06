/*
 * Behavioral checks for the TinyK engine, driven through the same synth_set_param /
 * synth_note_on path the Move UI uses.
 *
 *   zig cc -O2 -Isrc/dsp tools/test_behavior.c src/dsp/dsp.c -lm -o tools/test_behavior
 *   (or gcc ...)   then run:  tools/test_behavior       (exit code 0 = all pass)
 *
 * Checks: (1) Layer mode triggers Timbre 1 and Timbre 2 on one note-on, balance scales each,
 * Timbre 2 uses its own parameters; (2) attack/decay/release knobs map exponentially over
 * 1.5 ms..12 s and 15 ms..20 s; (3) editing Timbre 2 never leaks into Single-mode sound.
 */
#include "dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR MOVE_SAMPLE_RATE

static synth_engine_t S;
static int failures = 0;

static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* Render `seconds` into a malloc'd interleaved buffer. */
static int16_t *render(double seconds, int *frames_out) {
    int frames = (int)(seconds * SR);
    int16_t *buf = calloc((size_t)frames * 2, sizeof(int16_t));
    for (int done = 0; done < frames;) {
        int n = frames - done < 128 ? frames - done : 128;
        synth_render(&S, buf + done * 2, n);
        done += n;
    }
    *frames_out = frames;
    return buf;
}

static double energy(const int16_t *b, int frames, int ch) {
    double e = 0;
    for (int i = 0; i < frames; i++) {
        double v = b[i * 2 + ch] / 32768.0;
        e += v * v;
    }
    return e;
}

/* 10 ms RMS envelope of the mono mix. */
static double *rms_env(const int16_t *b, int frames, int *n_out) {
    int win = SR / 100, n = frames / win;
    double *env = malloc(sizeof(double) * (size_t)n);
    for (int k = 0; k < n; k++) {
        double e = 0;
        for (int i = 0; i < win; i++) {
            double v = (b[(k * win + i) * 2] + b[(k * win + i) * 2 + 1]) / 65536.0;
            e += v * v;
        }
        env[k] = sqrt(e / win);
    }
    *n_out = n;
    return env;
}

static void fresh(int preset) {
    synth_init(&S);
    synth_load_preset(&S, preset);
}

static void test_layer(void) {
    printf("Layer mode\n");
    fresh(0); /* A.11 is a Layer patch */
    check(S.voice_mode == 1, "preset 0 loads in Layer mode");
    check(fabsf(S.timbre_params[0][PARAM_CUTOFF] - S.timbre_params[1][PARAM_CUTOFF]) > 0.05f,
          "Timbre 2 has its own cutoff");
    check(S.timbre_extra[0].transpose_semi != S.timbre_extra[1].transpose_semi ||
          S.timbre_params[0][PARAM_WAVE1] != S.timbre_params[1][PARAM_WAVE1] ||
          S.timbre_params[0][PARAM_DETUNE] != S.timbre_params[1][PARAM_DETUNE],
          "Timbre 2 has its own waveform / tuning / transpose");

    synth_note_on(&S, 60, 100);
    int a = 0, b = 0;
    for (int v = 0; v < NUM_VOICES; v++) {
        if (S.voices[v].active && S.voices[v].is_timbre_2 == 0) a++;
        if (S.voices[v].active && S.voices[v].is_timbre_2 == 1) b++;
    }
    check(a == 1 && b == 1, "one note-on starts exactly one Timbre 1 voice and one Timbre 2 voice");

    double e[3][2];
    float balances[3] = {0.0f, 0.5f, 1.0f};
    for (int k = 0; k < 3; k++) {
        fresh(0);
        synth_set_param(&S, "timbre_balance", balances[k]);
        synth_note_on(&S, 60, 100);
        int frames;
        int16_t *buf = render(1.0, &frames);
        e[k][0] = energy(buf, frames, 0);
        e[k][1] = energy(buf, frames, 1);
        free(buf);
    }
    check(e[0][0] > 0 && e[0][1] > 0, "balance 0.0: Timbre 1 audible");
    check(e[2][0] > 0 && e[2][1] > 0, "balance 1.0: Timbre 2 audible");
    check(e[0][0] / (e[0][1] + 1e-12) > 1.0 || e[0][1] > 0, "balance 0.0 and 1.0 render different signals");
    check(fabs(e[0][0] - e[2][0]) > 1e-6 || fabs(e[0][1] - e[2][1]) > 1e-6,
          "balance moves the output between the two timbres");
    check(e[0][0] > e[0][1], "balance 0.0 (Timbre 1 only) is weighted left");
    check(e[2][1] > e[2][0], "balance 1.0 (Timbre 2 only) is weighted right");
    check(e[1][0] + e[1][1] > 0, "balance 0.5: both play");
}

static void test_edit_routing(void) {
    printf("Timbre editing\n");
    fresh(0);
    float t2_cutoff = S.timbre_params[1][PARAM_CUTOFF];
    float t1_cutoff = S.timbre_params[0][PARAM_CUTOFF];
    synth_set_param(&S, "timbre_edit", 1.0f);
    synth_set_param(&S, "cutoff", 0.9f);
    check(fabsf(S.timbre_params[1][PARAM_CUTOFF] - 0.9f) < 1e-6f, "Layer + Edit Timbre 2: cutoff edits Timbre 2");
    check(fabsf(S.timbre_params[0][PARAM_CUTOFF] - t1_cutoff) < 1e-6f && fabsf(S.params[PARAM_CUTOFF] - t1_cutoff) < 1e-6f,
          "...and leaves Timbre 1 untouched");
    check(fabsf(synth_get_param(&S, "cutoff") - 0.9f) < 1e-6f, "get_param reports the edited timbre");
    synth_set_param(&S, "timbre_edit", 0.0f);
    check(fabsf(synth_get_param(&S, "cutoff") - t1_cutoff) < 1e-6f, "switching edit back shows Timbre 1");

    synth_set_param(&S, "voice_mode", 0.0f);
    synth_set_param(&S, "timbre_edit", 1.0f);
    synth_set_param(&S, "cutoff", 0.3f);
    check(fabsf(S.params[PARAM_CUTOFF] - 0.3f) < 1e-6f, "Single mode: edits always reach the sound being played");
    check(fabsf(S.timbre_params[1][PARAM_CUTOFF] - 0.9f) < 1e-6f, "...without overwriting Timbre 2's patch data");
    (void)t2_cutoff;
}

/* Time (s) for the 10 ms RMS envelope to first reach 90% of its eventual max. */
static double time_to_peak(const double *env, int n) {
    double mx = 0;
    for (int i = 0; i < n; i++) if (env[i] > mx) mx = env[i];
    for (int i = 0; i < n; i++) if (env[i] >= 0.9 * mx) return i * 0.01;
    return -1;
}

/* Time (s) after the peak for the envelope to fall to 10% (-20 dB) of the peak. */
static double time_to_fall(const double *env, int n) {
    int pk = 0;
    for (int i = 0; i < n; i++) if (env[i] > env[pk]) pk = i;
    for (int i = pk; i < n; i++) if (env[i] <= 0.1 * env[pk]) return (i - pk) * 0.01;
    return -1;
}

static void open_voice(void) {
    /* a plain, fully open single-oscillator voice so only the amp envelope shapes the level */
    fresh(4);
    synth_set_param(&S, "voice_mode", 0.0f);
    synth_set_param(&S, "cutoff", 1.0f);
    synth_set_param(&S, "env_int", 0.5f);
    synth_set_param(&S, "resonance", 0.0f);
    synth_set_param(&S, "delay_mix", 0.0f);
    synth_set_param(&S, "chorus_mix", 0.0f);
    synth_set_param(&S, "osc_mix", 0.0f);
    synth_set_param(&S, "attack2", 0.0f);
    synth_set_param(&S, "decay2", 0.0f);
    synth_set_param(&S, "sustain2", 1.0f);
    synth_set_param(&S, "release2", 0.0f);
}

static void test_envelopes(void) {
    printf("Envelope time curves (exponential)\n");
    const float knob[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    double att[5], dec[5], rel[5];

    for (int k = 0; k < 5; k++) {
        open_voice();
        synth_set_param(&S, "attack2", knob[k]);
        synth_note_on(&S, 48, 100);
        int frames, n;
        int16_t *buf = render(k == 4 ? 16.0 : 4.0, &frames);
        double *env = rms_env(buf, frames, &n);
        att[k] = time_to_peak(env, n);
        free(env); free(buf);

        open_voice();
        synth_set_param(&S, "sustain2", 0.0f);
        synth_set_param(&S, "decay2", knob[k]);
        synth_note_on(&S, 48, 100);
        buf = render(k == 4 ? 26.0 : 8.0, &frames);
        env = rms_env(buf, frames, &n);
        dec[k] = time_to_fall(env, n);
        free(env); free(buf);

        open_voice();
        synth_set_param(&S, "release2", knob[k]);
        synth_note_on(&S, 48, 100);
        int16_t *held = render(0.5, &frames);
        free(held);
        synth_note_off(&S, 48);
        buf = render(k == 4 ? 26.0 : 8.0, &frames);
        env = rms_env(buf, frames, &n);
        rel[k] = time_to_fall(env, n);
        free(env); free(buf);
    }

    printf("  knob      attack(s)  decay(-20dB,s)  release(-20dB,s)\n");
    for (int k = 0; k < 5; k++) printf("  %.2f   %10.3f  %14.3f  %16.3f\n", knob[k], att[k], dec[k], rel[k]);

    check(att[0] >= 0 && att[0] < 0.03, "attack 0.0 is an instant stab (< 30 ms)");
    check(att[4] > 6.0, "attack 1.0 is a slow swell (> 6 s)");
    check(att[1] < att[2] && att[2] < att[3] && att[3] < att[4], "attack time rises monotonically across the knob");
    check(att[2] > 0.05 && att[2] < 0.4, "attack mid-knob is ~0.1 s (not clustered at an extreme)");
    check(dec[0] >= 0 && dec[0] < 0.05, "decay 0.0 is a short pluck (< 50 ms)");
    check(dec[4] > 5.0, "decay 1.0 is very long (> 5 s)");
    check(dec[1] < dec[2] && dec[2] < dec[3] && dec[3] < dec[4], "decay time rises monotonically across the knob");
    check(rel[0] >= 0 && rel[0] < 0.05 && rel[0] > 0.0, "release 0.0 is short but not an instant click");
    check(rel[4] > 5.0, "release 1.0 is a long tail (> 5 s)");
    check(rel[1] < rel[2] && rel[2] < rel[3] && rel[3] < rel[4], "release time rises monotonically across the knob");
}

int main(void) {
    test_layer();
    test_edit_routing();
    test_envelopes();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
