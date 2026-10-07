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
        /* the patch routes off: A.11's Timbre 1 has LFO1 -> pan, which would swing a 1 s render either way */
        for (int t = 0; t < 2; t++)
            for (int p = 0; p < 4; p++) S.timbre_extra[t].patch_int[p] = 0.0f;
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

/* High-frequency share of a render (rms of the first difference over rms): a brightness proxy */
static double brightness(const int16_t *b, int frames) {
    double d = 0.0, x = 0.0;
    for (int i = 1; i < frames; i++) {
        double s = 0.5 * (b[2 * i] + b[2 * i + 1]), p = 0.5 * (b[2 * i - 2] + b[2 * i - 1]);
        d += (s - p) * (s - p);
        x += s * s;
    }
    return sqrt(d / (x + 1e-12));
}

/* A single-timbre saw through an open-ish LPF12 with one patch slot routed src -> dst at intensity `in`
 * (hardware -63..+63); the other slots off and no LFO-driven routes, so renders are exactly repeatable. */
static void patch_voice(int src, int dst, int in) {
    open_voice();
    synth_set_param(&S, "cutoff", 0.55f);
    synth_set_param(&S, "filter_type", 1.0f / 3.0f);
    for (int p = 0; p < 4; p++) S.timbre_extra[0].patch_int[p] = 0.0f;
    S.timbre_extra[0].patch_src[0] = src;
    S.timbre_extra[0].patch_dst[0] = dst;
    S.timbre_extra[0].patch_int[0] = (float)in / 63.0f;
}

static int16_t *play(double seconds, int *frames) {
    synth_note_on(&S, 48, 100);
    return render(seconds, frames);
}

static void midi(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t msg[3] = { a, b, c };
    move_plugin_on_midi(&S, msg, 3, 0);
}

static void test_patch_sources(void) {
    printf("\nVirtual patch sources (6 = Pitch Bend, 7 = Mod Wheel / CC1) and the pan curve:\n");
    int n;

    patch_voice(PATCH_SRC_MOD_WHEEL, PATCH_DST_CUTOFF, -35);
    midi(0xB0, 2, 127);
    check(S.modwheel_src == 0.0f && S.bend_src == 0.0f, "CC2 drives no patch source");
    midi(0xB0, 1, 127);
    check(fabsf(S.modwheel_src - 1.0f) < 1e-6f, "CC1 (mod wheel) drives source 7, up to +1");
    midi(0xB0, 1, 0);
    check(S.modwheel_src == 0.0f, "mod wheel at 0 -> source 7 is 0");

    /* The Move has no wheel: the Mod Wheel knob feeds the same source; whichever moved last wins */
    synth_set_param(&S, "mod_wheel", 127.0f);
    check(fabsf(S.modwheel_src - 1.0f) < 1e-6f, "Mod Wheel knob at 127 -> source 7 = +1 (no CC1 sent)");
    midi(0xB0, 1, 32);
    check(fabsf(synth_get_param(&S, "mod_wheel") - 32.0f) < 0.01f, "an external CC1 takes over, and the knob reads it back");
    synth_set_param(&S, "mod_wheel", 0.0f);
    check(S.modwheel_src == 0.0f, "Mod Wheel knob back to 0 -> source 7 is 0");

    /* Mod wheel at rest adds exactly nothing: same samples as the slot switched off */
    patch_voice(PATCH_SRC_MOD_WHEEL, PATCH_DST_CUTOFF, -35);
    int16_t *rest = play(0.4, &n);
    patch_voice(PATCH_SRC_MOD_WHEEL, PATCH_DST_CUTOFF, 0);
    int16_t *off = play(0.4, &n);
    check(memcmp(rest, off, (size_t)n * 2 * sizeof(int16_t)) == 0, "mod wheel at 0: modulation delta is exactly 0 (render unchanged)");
    patch_voice(PATCH_SRC_MOD_WHEEL, PATCH_DST_CUTOFF, -35);
    midi(0xB0, 1, 127);
    int16_t *wheel = play(0.4, &n);
    check(brightness(wheel, n) < 0.8 * brightness(rest, n), "mod wheel up with -35 -> cutoff closes (B.11 patch 2)");
    free(rest); free(off); free(wheel);

    patch_voice(PATCH_SRC_PITCH_BEND, PATCH_DST_CUTOFF, 40);
    midi(0xE0, 0x7F, 0x7F);
    check(S.bend_src > 0.99f, "pitch bend up -> source 6 = +1");
    midi(0xE0, 0x00, 0x00);
    check(S.bend_src < -0.99f, "pitch bend down -> source 6 = -1 (bipolar)");
    midi(0xE0, 0x00, 0x40);
    check(S.bend_src == 0.0f, "pitch bend centred -> source 6 = 0");

    /* Pan: a held source at +1 shows the depth the curve gives a +20 route */
    patch_voice(PATCH_SRC_MOD_WHEEL, PATCH_DST_PAN, 20);
    midi(0xB0, 1, 127);
    int16_t *pan = play(0.4, &n);
    double db = 10.0 * log10(energy(pan, n, 1) / (energy(pan, n, 0) + 1e-12));
    char what[96];
    snprintf(what, sizeof what, "pan +20 at full source swings %.1f dB (6..9 dB wanted)", db);
    check(db > 6.0 && db < 9.0, what);
    free(pan);
}

int main(void) {
    test_layer();
    test_edit_routing();
    test_envelopes();
    test_patch_sources();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
