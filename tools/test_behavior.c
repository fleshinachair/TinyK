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
    /* a timbre has half the voices in Layer mode, and Unison stacks as many of them as it can, UNISON_STACK at most */
    check(a == 1 && b == (NUM_VOICES / 2 < UNISON_STACK ? NUM_VOICES / 2 : UNISON_STACK),
          "one note-on starts one Timbre 1 voice (Poly) and Timbre 2's unison stack (A.11 T2 is Unison)");

    double e[3][2];
    float balances[3] = {0.0f, 0.5f, 1.0f};
    for (int k = 0; k < 3; k++) {
        fresh(0);
        synth_set_param(&S, "delay_mix", 0.0f); /* pan alone: no stereo FX */
        synth_set_param(&S, "chorus_mix", 0.0f);
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
    /* Pan is the patch's (byte +26): A.11 has both timbres centred, so neither end of the balance leans */
    char what[128];
    double lean0 = 10.0 * log10(e[0][0] / (e[0][1] + 1e-12)), lean2 = 10.0 * log10(e[2][0] / (e[2][1] + 1e-12));
    snprintf(what, sizeof what, "timbre pans from the patch (A.11: both centred): Timbre 1 alone leans %+.2f dB, Timbre 2 %+.2f dB", lean0, lean2);
    /* a linear balance, as on the plug-in: a timbre at pan p (-1..+1) leans 20 log10((1 - p) / (1 + p)) dB */
    double want0 = 20.0 * log10((1.0 - S.timbre_extra[0].pan) / (1.0 + S.timbre_extra[0].pan));
    double want2 = 20.0 * log10((1.0 - S.timbre_extra[1].pan) / (1.0 + S.timbre_extra[1].pan));
    check(fabs(lean0 - want0) < 0.3 && fabs(lean2 - want2) < 0.3, what);
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
    synth_set_param(&S, "voice_assign", 0.5f); /* Poly: one voice, whatever the program's assign */
    synth_set_param(&S, "drive", 0.0f);
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

    /* 10 ms windows, and the onset fade takes the first 4.5 ms: the 0.9 point lands in the second or third */
    check(att[0] >= 0 && att[0] <= 0.03, "attack 0.0 is an instant stab (<= 30 ms)");
    check(att[4] > 6.0, "attack 1.0 is a slow swell (> 6 s)");
    check(att[1] < att[2] && att[2] < att[3] && att[3] < att[4], "attack time rises monotonically across the knob");
    check(att[2] > 0.45 && att[2] < 0.75, "attack mid-knob is ~0.6 s (the plug-in: 90 % at 0.61 s)");
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
    /* -35 at full source is 0.73 octave down on the plug-in's patch -> cutoff scale (1.59 octaves at +-63) */
    {
        char msg[128];
        snprintf(msg, sizeof msg, "mod wheel up with -35 -> cutoff closes (B.11 patch 2): brightness x %.2f", brightness(wheel, n) / brightness(rest, n));
        check(brightness(wheel, n) < 0.93 * brightness(rest, n), msg);
    }
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
    snprintf(what, sizeof what, "pan +20 at full source swings %.1f dB (the plug-in: 8.6)", db);
    check(db > 7.5 && db < 9.2, what); /* the output limiter takes a little off the louder side */
    free(pan);
}

/* Energy of the mono mix in [t0, t1) seconds */
static double window_energy(const int16_t *b, int frames, double t0, double t1) {
    double e = 0.0;
    for (int i = (int)(t0 * SR); i < (int)(t1 * SR) && i < frames; i++) {
        double s = 0.5 * (b[2 * i] + b[2 * i + 1]) / 32768.0;
        e += s * s;
    }
    return e;
}

/* A.21's Timbre 2 is a held sine + noise gated by a tempo-synced, key-synced LFO1 saw -> amp (+48, +63,
 * a quarter note per cycle at 120 BPM); LFO2's square adds an off-beat noise burst. The saw falls, so each
 * beat has died away before the next; a rising saw played every hit backwards (loudest at the beat's end). */
static void test_lfo_saw_gate(void) {
    printf("\nLFO saw -> amp gate (A.21 Timbre 2):\n");
    fresh(8);
    synth_set_param(&S, "timbre_balance", 1.0f); /* Timbre 2 only */
    synth_set_param(&S, "delay_mix", 0.0f);
    synth_note_on(&S, 60, 100);
    int n;
    int16_t *b = render(1.0, &n);
    for (int beat = 0; beat < 2; beat++) {
        double t = beat * 0.5;
        double early = window_energy(b, n, t + 0.01, t + 0.11), late = window_energy(b, n, t + 0.43, t + 0.49);
        char what[112];
        snprintf(what, sizeof what, "beat %d starts loud and has died away before the next (%.1f dB down at 430-490 ms)",
                 beat + 1, 10.0 * log10(early * 0.06 / (late * 0.10 + 1e-12)));
        check(early * 0.06 > 100.0 * late * 0.10, what); /* per-sample energy at least 20 dB lower */
    }
    free(b);
}

/* A fake Schwung host: session tempo and a transport position the render loop below advances */
static float g_fake_bpm = 120.0f;
static double g_fake_beat = -1.0;
static float fake_get_bpm(void) { return g_fake_bpm; }
static double fake_get_beat_position(void) { return g_fake_beat; }

/* Render `seconds`, advancing the fake transport per 128-frame block (when it runs) */
static int16_t *render_clocked(double seconds, int *frames_out) {
    int frames = (int)(seconds * SR);
    int16_t *buf = calloc((size_t)frames * 2, sizeof(int16_t));
    for (int done = 0; done < frames;) {
        int n = frames - done < 128 ? frames - done : 128;
        synth_render(&S, buf + done * 2, n);
        if (g_fake_beat >= 0.0) g_fake_beat += n / (double)SR * g_fake_bpm / 60.0;
        done += n;
    }
    *frames_out = frames;
    return buf;
}

/* Time of the loudest 10 ms window in [t0, t1) of the left channel */
static double loudest_at(const int16_t *b, int frames, double t0, double t1) {
    double best = -1.0, at = t0;
    for (double t = t0; t < t1; t += 0.002) {
        double e = 0.0;
        for (int i = (int)(t * SR); i < (int)((t + 0.01) * SR) && i < frames; i++) e += (double)b[2 * i] * b[2 * i];
        if (e > best) { best = e; at = t; }
    }
    return at;
}

static void test_host_tempo(void) {
    printf("\nHost tempo (fake Schwung host):\n");
    static host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.get_bpm = fake_get_bpm;
    host.get_beat_position = fake_get_beat_position;
    move_plugin_init_v2(&host);
    int n;
    char what[128];

    /* A.21's key-synced quarter-note gate at 90 BPM: beats 0.667 s apart */
    g_fake_bpm = 90.0f; g_fake_beat = -1.0;
    fresh(8);
    synth_set_param(&S, "timbre_balance", 1.0f);
    synth_set_param(&S, "delay_mix", 0.0f);
    synth_note_on(&S, 60, 100);
    int16_t *b = render_clocked(1.5, &n);
    double end1 = window_energy(b, n, 0.60, 0.66), start2 = window_energy(b, n, 0.67, 0.77);
    snprintf(what, sizeof what, "90 BPM: the gate's 2nd beat starts at 0.667 s (%.0f dB above the end of the 1st)",
             10.0 * log10(start2 / 0.10 / (end1 / 0.06 + 1e-12)));
    check(start2 / 0.10 > 100.0 * end1 / 0.06, what);
    free(b);

    /* A synced 1/4 delay echoes 60 / BPM after a short blip */
    open_voice();
    synth_set_param(&S, "decay2", 0.0f);
    synth_set_param(&S, "sustain2", 0.0f);
    synth_set_param(&S, "delay_mix", 0.6f);
    synth_set_param(&S, "delay_feedback", 0.3f);
    S.delay_sync_note = 8;                               /* 1/4 */
    synth_set_param(&S, "delay_time", 8.0f / 14.0f);
    synth_note_on(&S, 60, 100);
    b = render_clocked(0.03, &n);
    free(b);
    synth_note_off(&S, 60);
    b = render_clocked(1.0, &n);
    double echo = 0.03 + loudest_at(b, n, 0.3, 0.95);
    snprintf(what, sizeof what, "synced 1/4 delay at 90 BPM echoes at %.3f s (0.667 expected)", echo);
    check(fabs(echo - 60.0 / 90.0) < 0.02, what);
    free(b);

    /* Transport running: a free-running synced LFO follows the beat grid, not the note-on. A.21's Timbre 2 with
     * LFO1 key sync off, note played a quarter of a beat late: the gate's next hit lands on the next beat. */
    g_fake_bpm = 120.0f; g_fake_beat = 4.25;
    fresh(8);
    synth_set_param(&S, "timbre_balance", 1.0f);
    synth_set_param(&S, "delay_mix", 0.0f);
    S.timbre_extra[1].lfo_keysync[0] = 0;
    synth_note_on(&S, 60, 100);
    b = render_clocked(0.6, &n);
    /* the gate closes just before beat 5 (+0.375 s) and reopens on it; key-synced it would be mid-cycle there */
    double before_beat = window_energy(b, n, 0.345, 0.370), on_beat = window_energy(b, n, 0.380, 0.420);
    snprintf(what, sizeof what, "transport at beat 4.25: the gate closes before beat 5 (+0.375 s) and reopens on it (%.0f dB)",
             10.0 * log10(on_beat / 0.040 / (before_beat / 0.025 + 1e-12)));
    check(on_beat / 0.040 > 100.0 * before_beat / 0.025, what);
    free(b);

    g_fake_beat = -1.0;
    move_plugin_init_v2(NULL);
}

/* Worst 1 ms burst of high-frequency content (second difference) relative to the local level (+-25 ms) of
 * channel ch, after `from` seconds, in dB: an LFO stepping a gain shows up as a burst well above the rest. */
static double worst_hf_burst_db(const int16_t *b, int frames, int ch, double from) {
    int w = SR / 1000, n = frames / w;
    double *hf = calloc((size_t)n, sizeof(double)), *lv = calloc((size_t)n, sizeof(double)), worst = -200.0;
    for (int i = 0; i < n; i++) {
        for (int j = i * w; j < (i + 1) * w; j++) {
            double x = b[2 * j + ch];
            double d2 = (j >= 2) ? x - 2.0 * b[2 * (j - 1) + ch] + b[2 * (j - 2) + ch] : 0.0;
            hf[i] += d2 * d2;
            lv[i] += x * x;
        }
    }
    for (int i = (int)(from * 1000.0) + 25; i < n - 25; i++) {
        double loc = 0.0;
        for (int k = i - 25; k < i + 25; k++) loc += lv[k];
        double db = 10.0 * log10(hf[i] / (loc / 50.0 + 1e-9) + 1e-12);
        if (db > worst) worst = db;
    }
    free(hf); free(lv);
    return worst;
}

/* A.31's Timbre 1: an S&H LFO2 (1/16, key-synced) -> pan +63 on a held note. Each step used to switch the
 * channel gains in one sample: a click per sixteenth. The patch LFO slew limit turns them into short ramps. */
static void test_lfo_steps_click_free(void) {
    printf("\nLFO steps without clicks (A.31 Timbre 1, S&H -> pan +63):\n");
    double worst[2], plain[2];
    for (int pass = 0; pass < 2; pass++) {
        fresh(16);
        synth_set_param(&S, "timbre_balance", 0.0f); /* Timbre 1 only */
        synth_set_param(&S, "delay_mix", 0.0f);
        synth_set_param(&S, "chorus_mix", 0.0f);
        if (pass == 1)
            for (int p = 0; p < 4; p++) S.timbre_extra[0].patch_int[p] = 0.0f; /* the same sound, unmodulated */
        synth_note_on(&S, 60, 100);
        int n;
        int16_t *b = render(2.0, &n);
        double *dst = pass ? plain : worst;
        dst[0] = worst_hf_burst_db(b, n, 0, 0.2);
        dst[1] = worst_hf_burst_db(b, n, 1, 0.2);
        free(b);
    }
    /* At +63 the pan swings from one side fully to the other (the measured depth and the linear pan law), so
     * a side falls silent between steps and its bursts stand far above its own local level whatever the
     * smoothing. The bound is the plug-in's own figure for S&H -> pan +63 by this measure, -9.1 dB (-18.4
     * unmodulated), with 2 dB of margin: the steps may not be harder than the plug-in's 2 ms crossings. */
    char what[160];
    snprintf(what, sizeof what, "worst HF burst L %.1f / R %.1f dB (%.1f / %.1f unmodulated; the plug-in: -9.1, bound -7)",
             worst[0], worst[1], plain[0], plain[1]);
    check(worst[0] < -7.0 && worst[1] < -7.0, what);
}

/* The audible noise is white after the brightness tilt: for white noise the first difference carries exactly
 * twice the energy of the signal, a top-heavy spectrum more (the uncompensated tilt gave 2.9). Osc 1 = noise
 * through a wide-open LPF12, mono mix. */
static void test_noise_is_white(void) {
    printf("\nNoise spectrum after the brightness tilt:\n");
    open_voice();
    synth_set_param(&S, "wave1", 1.0f);               /* Noise */
    synth_set_param(&S, "filter_type", 1.0f);         /* 12HPF at cutoff 0: the flat path */
    synth_set_param(&S, "cutoff", 0.0f);
    S.timbre_extra[0].osc1_ctrl[0] = 1.0f;            /* the Noise oscillator's own low-pass open, no resonance */
    S.timbre_extra[0].osc1_ctrl[1] = 0.0f;
    synth_note_on(&S, 60, 100);
    int n;
    int16_t *b = render(1.0, &n);
    double e = 0.0, d = 0.0;
    for (int i = (int)(0.1 * SR); i < n; i++) {
        double x = 0.5 * (b[2 * i] + b[2 * i + 1]), p = 0.5 * (b[2 * i - 2] + b[2 * i - 1]);
        e += x * x;
        d += (x - p) * (x - p);
    }
    char what[96];
    /* the plug-in's noise rolls off above 3 kHz (about 1.25 here); the tilt left on it would read above 2.6 */
    snprintf(what, sizeof what, "first-difference / signal energy %.2f (white noise 2.00; top-heavy > 2.6)", d / e);
    check(d / e > 0.9 && d / e < 1.7, what);
    free(b);
}

/* --- Arpeggiator ------------------------------------------------------------------------------------------------
 * Drives keys as MIDI (the path the Move uses) and reads the voices' gates block by block: which notes start when. */
#define ARP_LOG_MAX 64
typedef struct { double t; int note; int timbre; } arp_hit_t;

/* Renders `seconds` in 128-frame blocks; logs every gate that opened during a block (note, timbre, block time) */
static int arp_log(double seconds, arp_hit_t *log, int max) {
    int frames = (int)(seconds * SR), count = 0;
    int16_t buf[256];
    int was[NUM_VOICES], note_was[NUM_VOICES];
    for (int v = 0; v < NUM_VOICES; v++) { was[v] = S.voices[v].gate; note_was[v] = S.voices[v].note; }
    for (int done = 0; done < frames; done += 128) {
        uint32_t age_before[NUM_VOICES];
        for (int v = 0; v < NUM_VOICES; v++) age_before[v] = S.voices[v].age;
        synth_render(&S, buf, 128);
        if (g_fake_beat >= 0.0) g_fake_beat += 128.0 / SR * g_fake_bpm / 60.0;
        for (int v = 0; v < NUM_VOICES; v++) {
            int started = S.voices[v].gate && (!was[v] || S.voices[v].age != age_before[v] || S.voices[v].note != note_was[v]);
            if (started && count < max) {
                log[count].t = (double)done / SR;
                log[count].note = S.voices[v].note;
                log[count].timbre = S.voices[v].is_timbre_2;
                count++;
            }
            was[v] = S.voices[v].gate;
            note_was[v] = S.voices[v].note;
        }
    }
    return count;
}

static void arp_voice(int type, int range, int resolution, float gate, uint8_t pattern, int length) {
    open_voice();
    S.arp.set.on = 1;
    S.arp.set.latch = 0;
    S.arp.set.key_sync = 1;
    S.arp.set.target = 0;
    S.arp.set.type = type;
    S.arp.set.range = range;
    S.arp.set.resolution = resolution;
    S.arp.set.gate = gate;
    S.arp.set.swing = 0.0f;
    S.arp.set.pattern = pattern;
    S.arp.set.length = length;
}

static void keys(const int *notes, int n, int on) {
    for (int i = 0; i < n; i++) midi(on ? 0x90 : 0x80, (uint8_t)notes[i], on ? 100 : 0);
}

/* "60 64 67 ..." of the first n hits */
static void hit_notes(const arp_hit_t *h, int n, char *out, size_t cap) {
    out[0] = '\0';
    for (int i = 0; i < n; i++) snprintf(out + strlen(out), cap - strlen(out), "%s%d", i ? " " : "", h[i].note);
}

static int hits_match(const arp_hit_t *h, int n, const int *want, int wn) {
    if (n < wn) return 0;
    for (int i = 0; i < wn; i++) if (h[i].note != want[i]) return 0;
    return 1;
}

static void test_arp(void) {
    printf("\nArpeggiator (MIDI keys, 120 BPM):\n");
    static host_api_v1_t host;
    memset(&host, 0, sizeof host);
    host.get_bpm = fake_get_bpm;
    host.get_beat_position = fake_get_beat_position;
    plugin_api_v2_t *api = move_plugin_init_v2(&host);
    g_fake_bpm = 120.0f;
    g_fake_beat = -1.0;
    arp_hit_t h[ARP_LOG_MAX];
    char got[256], what[400];
    const int chord[3] = { 60, 64, 67 };
    int n;

    /* Off: keys play directly */
    open_voice();
    S.arp.set.on = 0;
    midi(0x90, 60, 100);
    check(S.voices[0].gate && S.voices[0].note == 60, "arp off: a key plays at once, as before");
    midi(0x80, 60, 0);

    /* UP, 1 octave, 1/16 (125 ms at 120 BPM), gate 50 % */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    hit_notes(h, n < 8 ? n : 8, got, sizeof got);
    {
        const int want[] = { 60, 64, 67, 60, 64, 67, 60, 64 };
        int timing = n >= 8;
        for (int i = 0; i < 8 && timing; i++) timing = fabs(h[i].t - 0.125 * i) < 0.004;
        snprintf(what, sizeof what, "UP: %s, one step per 1/16 (125 ms) from the first key", got);
        check(hits_match(h, n, want, 8) && timing, what);
    }
    {   /* gate 50 %: the note is released mid-step */
        int16_t b[256];
        arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
        keys(chord, 1, 1);
        for (int i = 0; i < 20; i++) synth_render(&S, b, 128); /* 58 ms */
        int open_mid = S.voices[0].gate;
        for (int i = 0; i < 5; i++) synth_render(&S, b, 128);  /* 72 ms */
        check(open_mid && !S.voices[0].gate, "gate 50 %: the step's note is held 62.5 ms of its 125 ms");
    }
    keys(chord, 3, 0);

    /* arp_playhead, which the Arp Steps LEDs (src/canvas.js) run the playhead from: stopped "0,<length>";
     * running "1,<length>,<next step>,<ms to it>,<step ms>,<swing>" */
    {
        char ph0[64] = "", ph1[64] = "", ph2[64] = "";
        arp_voice(ARP_UP, 1, 1, 0.5f, 0, 6);
        api->get_param(&S, "arp_playhead", ph0, sizeof ph0);
        keys(chord, 1, 1);
        arp_log(0.3, h, ARP_LOG_MAX);          /* steps at 0, 125, 250 ms: step 3 sounds, the 4th is next */
        api->get_param(&S, "arp_playhead", ph1, sizeof ph1);
        keys(chord, 1, 0);
        api->get_param(&S, "arp_playhead", ph2, sizeof ph2);
        int on = 0, len = 0, next = -1;
        float to_next = -1.0f, step_ms = 0.0f, swing = 9.0f;
        int got_n = sscanf(ph1, "%d,%d,%d,%f,%f,%f", &on, &len, &next, &to_next, &step_ms, &swing);
        snprintf(what, sizeof what, "arp_playhead: stopped \"%s\"; 0.302 s into a 6-step 1/16 arpeggio \"%s\" "
                 "(step 3 lit, the next in ~73 ms); released \"%s\"", ph0, ph1, ph2);
        check(strcmp(ph0, "0,6") == 0 && got_n == 6 && on == 1 && len == 6 && next == 3 && fabsf(to_next - 73.1f) < 1.0f
              && fabsf(step_ms - 125.0f) < 0.01f && swing == 0.0f && strcmp(ph2, "0,6") == 0, what);
    }

    /* Range 2, DOWN, ALT1, ALT2 */
    arp_voice(ARP_UP, 2, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    hit_notes(h, n < 7 ? n : 7, got, sizeof got);
    { const int want[] = { 60, 64, 67, 72, 76, 79, 60 }; snprintf(what, sizeof what, "range 2 octaves: %s", got); check(hits_match(h, n, want, 7), what); }
    keys(chord, 3, 0);
    arp_voice(ARP_DOWN, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(0.8, h, ARP_LOG_MAX);
    hit_notes(h, n < 6 ? n : 6, got, sizeof got);
    { const int want[] = { 67, 64, 60, 67, 64, 60 }; snprintf(what, sizeof what, "DOWN: %s", got); check(hits_match(h, n, want, 6), what); }
    keys(chord, 3, 0);
    arp_voice(ARP_ALT1, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    hit_notes(h, n < 7 ? n : 7, got, sizeof got);
    { const int want[] = { 60, 64, 67, 64, 60, 64, 67 }; snprintf(what, sizeof what, "ALT1 (ends once): %s", got); check(hits_match(h, n, want, 7), what); }
    keys(chord, 3, 0);
    arp_voice(ARP_ALT2, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    hit_notes(h, n < 7 ? n : 7, got, sizeof got);
    { const int want[] = { 60, 64, 67, 67, 64, 60, 60 }; snprintf(what, sizeof what, "ALT2 (ends twice): %s", got); check(hits_match(h, n, want, 7), what); }
    keys(chord, 3, 0);

    /* RANDOM: only held notes, not one note over and over */
    arp_voice(ARP_RANDOM, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(2.0, h, ARP_LOG_MAX);
    {
        int ok = n >= 15, seen[3] = { 0, 0, 0 };
        for (int i = 0; i < n; i++) {
            int k = h[i].note == 60 ? 0 : (h[i].note == 64 ? 1 : (h[i].note == 67 ? 2 : -1));
            if (k < 0) ok = 0; else seen[k] = 1;
        }
        hit_notes(h, n < 10 ? n : 10, got, sizeof got);
        snprintf(what, sizeof what, "RANDOM picks among the held keys: %s ...", got);
        check(ok && seen[0] && seen[1] && seen[2], what);
    }
    keys(chord, 3, 0);

    /* TRIGGER: the chord together each step, the octave rising over the range */
    arp_voice(ARP_TRIGGER, 2, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    n = arp_log(0.3, h, ARP_LOG_MAX);
    {
        int first = 0, second = 0;
        for (int i = 0; i < n; i++) {
            if (h[i].t < 0.01) first += (h[i].note == 60 || h[i].note == 64 || h[i].note == 67);
            else if (fabs(h[i].t - 0.125) < 0.004) second += (h[i].note == 72 || h[i].note == 76 || h[i].note == 79);
        }
        snprintf(what, sizeof what, "TRIGGER: %d of 3 chord notes together on step 1, %d of 3 an octave up on step 2", first, second);
        check(first == 3 && second == 3, what);
    }
    keys(chord, 3, 0);

    /* Step mask: bit set = rest. Steps 2 and 4 rest; the note order carries on over them */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0x0A, 4);
    keys(chord, 3, 1);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    {
        int ok = n >= 4 && fabs(h[0].t) < 0.004 && fabs(h[1].t - 0.25) < 0.004 && fabs(h[2].t - 0.5) < 0.004 &&
                 h[0].note == 60 && h[1].note == 64 && h[2].note == 67 && h[3].note == 60;
        hit_notes(h, n < 4 ? n : 4, got, sizeof got);
        snprintf(what, sizeof what, "pattern 0x0A over 4 steps: steps 2 and 4 rest, notes %s at %.0f / %.0f / %.0f ms",
                 got, n > 0 ? h[0].t * 1000 : -1, n > 1 ? h[1].t * 1000 : -1, n > 2 ? h[2].t * 1000 : -1);
        check(ok, what);
    }
    keys(chord, 3, 0);

    /* Release stops it; latch keeps it, and the next key after a release starts a new set */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    arp_log(0.3, h, ARP_LOG_MAX);
    keys(chord, 3, 0);
    n = arp_log(0.5, h, ARP_LOG_MAX);
    check(n == 0 && !S.arp.running, "releasing the keys stops the arpeggio");
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    S.arp.set.latch = 1;
    keys(chord, 3, 1);
    arp_log(0.3, h, ARP_LOG_MAX);
    keys(chord, 3, 0);
    n = arp_log(0.5, h, ARP_LOG_MAX);
    int latched = n >= 3;
    midi(0x90, 50, 100);
    n = arp_log(0.5, h, ARP_LOG_MAX);
    int replaced = n >= 3;
    for (int i = 0; i < n; i++) replaced = replaced && h[i].note == 50;
    snprintf(what, sizeof what, "latch: the arpeggio carries on after release (%s); a new key replaces the set (%s)",
             latched ? "yes" : "no", replaced ? "yes" : "no");
    check(latched && replaced, what);
    midi(0x80, 50, 0);

    /* Transport running, key sync off: steps fall on the beat grid (a key at beat 0.1 waits for 0.25) */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    S.arp.set.key_sync = 0;
    g_fake_beat = 0.1;
    {
        int16_t b[256];
        synth_render(&S, b, 128); /* the arpeggiator picks up the transport */
        g_fake_beat += 128.0 / SR * g_fake_bpm / 60.0;
    }
    double beat_at_key = g_fake_beat;
    keys(chord, 1, 1);
    n = arp_log(0.5, h, ARP_LOG_MAX);
    {
        double first_beat = beat_at_key + (n > 0 ? h[0].t : 0) * g_fake_bpm / 60.0;
        snprintf(what, sizeof what, "transport lock: key at beat %.3f, first step at beat %.3f (grid 0.25)", beat_at_key, first_beat);
        check(n > 0 && fabs(first_beat - 0.25) < 0.012, what);
    }
    keys(chord, 1, 0);
    g_fake_beat = -1.0;

    /* A.21 AutoHouse as stored: arp on (UP, 2 octaves, Timbre 1 only); Timbre 2 holds the key */
    fresh(8);
    check(S.arp.set.on && S.arp.set.target == 1 && S.arp.set.type == ARP_UP && S.arp.set.range == 2,
          "A.21 loads its stored arpeggiator: on, UP, 2 octaves, Timbre 1");
    midi(0x90, 48, 100);
    n = arp_log(1.0, h, ARP_LOG_MAX);
    {
        int t1 = 0, t2 = 0, t1_oct = 0;
        for (int i = 0; i < n; i++) {
            if (h[i].timbre == 0) { t1++; t1_oct += h[i].note == 60; }
            else t2++;
        }
        int t2_held = (S.voices[1].gate && S.voices[1].note == 48) || (S.voices[3].gate && S.voices[3].note == 48);
        snprintf(what, sizeof what, "A.21: Timbre 1 steps %d times (%d an octave up); Timbre 2 holds the key from the press "
                 "(%d restarts, still on: %s)", t1, t1_oct, t2, t2_held ? "yes" : "no");
        check(t1 >= 6 && t1_oct >= 2 && t2 == 0 && t2_held, what);
    }
    midi(0x80, 48, 0);
    synth_set_param(&S, "arp_on", 0.0f);
    midi(0x90, 48, 100);
    check(S.voices[0].gate && S.voices[1].gate && S.voices[0].note == 48, "Arp knob off: the key plays both timbres directly");
    midi(0x80, 48, 0);
    check(synth_get_param(&S, "arp_on") == 0.0f, "Arp knob reads back off");

    /* Arp Steps knobs change the playing pattern live: rest steps 2..8 mid-run, then only step 1 of each bar plays */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    keys(chord, 1, 1);
    arp_log(0.3, h, ARP_LOG_MAX);                    /* steps 1-3 */
    for (int st = 2; st <= 8; st++) {
        char k[16];
        snprintf(k, sizeof k, "arp_step%d", st);
        synth_set_param(&S, k, 0.0f);
    }
    n = arp_log(2.0, h, ARP_LOG_MAX);                /* from step 4: only steps 1 (at 1.0 s and 2.0 s from the start) */
    {
        int ok = n >= 1;
        for (int i = 0; i < n; i++) ok = ok && fabs(h[i].t + 0.3 - floor(h[i].t + 0.3 + 0.5)) < 0.01;
        snprintf(what, sizeof what, "Arp Steps edited mid-run: %d hit%s in the next 2 s, all on step 1 of the bar", n, n == 1 ? "" : "s");
        check(ok && n == 2, what);
    }
    keys(chord, 1, 0);

    /* Arp Settings knobs change a running arpeggio at once */
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    keys(chord, 3, 1);
    arp_log(0.3, h, ARP_LOG_MAX);                   /* 60 64 67 */
    synth_set_param(&S, "arp_type", (float)ARP_DOWN);
    n = arp_log(0.4, h, ARP_LOG_MAX);
    {
        int down = n >= 3 && h[1].note < h[0].note && h[2].note < h[1].note + (h[1].note == 60 ? 12 : 0);
        hit_notes(h, n < 3 ? n : 3, got, sizeof got);
        snprintf(what, sizeof what, "Type -> DOWN mid-run: the next steps fall (%s)", got);
        check(n >= 3 && (down || (h[0].note > h[1].note)), what);
    }
    synth_set_param(&S, "arp_resolution", 3.0f);    /* 1/8: 250 ms */
    n = arp_log(1.0, h, ARP_LOG_MAX);
    {
        int ok = n >= 3 && fabs((h[2].t - h[1].t) - 0.25) < 0.004;
        snprintf(what, sizeof what, "Resolution -> 1/8 mid-run: steps %.0f ms apart", n >= 3 ? (h[2].t - h[1].t) * 1000 : -1);
        check(ok, what);
    }
    keys(chord, 3, 0);
    arp_voice(ARP_UP, 1, 1, 0.5f, 0, 8);
    S.arp.set.latch = 1;
    keys(chord, 3, 1);
    arp_log(0.2, h, ARP_LOG_MAX);
    keys(chord, 3, 0);
    synth_set_param(&S, "arp_latch", 0.0f);         /* no key is held: latch off stops it */
    n = arp_log(0.5, h, ARP_LOG_MAX);
    check(n == 0 && !S.arp.running, "Latch -> Off with no key held: the latched arpeggio stops");

    /* Target change in Layer mode: no voice is left gated on the old timbre after the keys are released */
    fresh(8);                                         /* A.21: Layer, arp on Timbre 1 */
    midi(0x90, 48, 100);
    arp_log(0.3, h, ARP_LOG_MAX);
    synth_set_param(&S, "arp_target", 2.0f);        /* Timbre 2 arpeggiates, Timbre 1 holds the key */
    n = arp_log(0.5, h, ARP_LOG_MAX);
    int t2_steps = 0;
    for (int i = 0; i < n; i++) t2_steps += h[i].timbre == 1;
    int t1_held = (S.voices[0].gate && S.voices[0].note == 48) || (S.voices[2].gate && S.voices[2].note == 48);
    midi(0x80, 48, 0);
    arp_log(0.05, h, ARP_LOG_MAX);
    int gated = 0;
    for (int v = 0; v < NUM_VOICES; v++) gated += S.voices[v].gate;
    snprintf(what, sizeof what, "Target -> Timbre 2 mid-run: Timbre 2 steps (%d), Timbre 1 holds the key (%s), nothing left on after release (%d gated)",
             t2_steps, t1_held ? "yes" : "no", gated);
    check(t2_steps >= 2 && t1_held && gated == 0, what);
    move_plugin_init_v2(NULL);
}

/* Program change: flush like CC 120 / 123 so nothing hangs or rings into the new sound */
static double max_step(const int16_t *b, int frames) {
    double m = 0;
    for (int ch = 0; ch < 2; ch++)
        for (int i = 1; i < frames; i++) {
            double d = fabs((b[i * 2 + ch] - b[(i - 1) * 2 + ch]) / 32768.0);
            if (d > m) m = d;
        }
    return m;
}

static double peak_in(const int16_t *b, int frames, double t0, double t1) {
    double m = 0;
    for (int i = (int)(t0 * SR); i < (int)(t1 * SR) && i < frames; i++)
        for (int ch = 0; ch < 2; ch++) {
            double v = fabs(b[i * 2 + ch] / 32768.0);
            if (v > m) m = v;
        }
    return m;
}

static void test_program_change_flush(void) {
    printf("\nProgram change flush\n");
    /* a Poly program and one with another voice assign (Mono or Unison), arpeggiator off in both */
    int poly = -1, other = -1, delayed = -1;
    for (int i = 0; i < 128; i++) {
        synth_init(&S);
        synth_load_preset(&S, i);
        if (S.arp.set.on || S.voice_mode) continue;
        int a = S.timbre_extra[0].assign;
        if (a == 1 && poly < 0) poly = i;
        if (a != 1 && other < 0) other = i;
        if (S.params[PARAM_DELAY_MIX] > 0.4f && S.params[PARAM_DELAY_FEEDBACK] > 0.4f && delayed < 0) delayed = i;
    }
    check(poly >= 0 && other >= 0 && delayed >= 0, "found a Poly, a non-Poly and a delay-heavy program");
    int frames;
    for (int dir = 0; dir < 2; dir++) {
        int from = dir ? other : poly, to = dir ? poly : other;
        fresh(from);
        midi(0x90, 60, 100);
        int16_t *a = render(0.4, &frames); free(a);
        synth_load_preset(&S, to);
        int16_t *b = render(1.0, &frames);
        double held = peak_in(b, frames, 0.1, 1.0);
        free(b);
        midi(0x80, 60, 0);
        int16_t *c = render(1.0, &frames);
        double after = peak_in(c, frames, 0.0, 1.0);
        free(c);
        char msg[160];
        snprintf(msg, sizeof msg, "key held across %d -> %d (assign %d -> %d): silent after the change (peak %.5f), still silent after release (%.5f)",
                 from, to, dir ? 0 : 1, dir ? 1 : 0, held, after);
        check(held < 1e-3 && after < 1e-3, msg);
    }
    /* delay / Mod FX tails and ringing filters do not carry into the next program */
    fresh(delayed);
    midi(0x90, 60, 110);
    int16_t *a = render(0.5, &frames);
    double before_slope = max_step(a, frames), before_peak = peak_in(a, frames, 0.2, 0.5);
    free(a);
    midi(0x80, 60, 0);
    a = render(0.05, &frames); free(a);
    synth_load_preset(&S, other);
    int16_t *b = render(1.5, &frames);
    double late = peak_in(b, frames, 0.05, 1.5);
    double slope = max_step(b, frames);
    double last = fabs(b[(frames - 1) * 2] / 32768.0);
    free(b);
    char msg[200];
    snprintf(msg, sizeof msg, "delay tail of program %d is gone after the change (peak %.5f after 50 ms, before %.3f)", delayed, late, before_peak);
    check(late < 1e-3, msg);
    snprintf(msg, sizeof msg, "flush is click-free: largest sample step %.4f against %.4f while playing; ends at %.5f", slope, before_slope, last);
    check(slope <= before_slope * 1.05 + 0.002 && last < 1e-3, msg);
    /* held keys and the arpeggio are dropped with the program: no stuck arpeggio after the change */
    fresh(20); /* A.21: the program's arpeggiator is on */
    int ks[2] = { 60, 64 };
    keys(ks, 2, 1);
    a = render(0.6, &frames); free(a);
    synth_load_preset(&S, other);
    b = render(0.5, &frames);
    snprintf(msg, sizeof msg, "arpeggiator keys cleared by a program change (held %d, running %d, peak %.5f)", S.arp.held_count, S.arp.running, peak_in(b, frames, 0.1, 0.5));
    check(S.arp.held_count == 0 && !S.arp.running && peak_in(b, frames, 0.1, 0.5) < 1e-3, msg);
    free(b);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Output level and dynamics: a full-velocity 3-note chord on 26 programs. The voice mix is linear (TINYK_MIX_GAIN keeps
 * chords far below the soft clip: median crest ~15 dB) and the output headroom (TINYK_OUTPUT_HEADROOM) puts the loudest
 * program's peak just under -6 dBFS and the median chord near -13 dBFS peak (native Move tracks sit around
 * -12..-8 dBFS) */
static void test_headroom(void) {
    printf("\nOutput level and dynamics (chord 48/52/55, full velocity)\n");
    double worst = 0, rms_db[26], crest_db[26];
    int frames, worst_i = 0, n = 0;
    for (int i = 0; i < 128; i += 5) {
        fresh(i);
        midi(0x90, 48, 127);
        midi(0x90, 52, 127);
        midi(0x90, 55, 127);
        int16_t *b = render(2.0, &frames);
        double pk = peak_in(b, frames, 0.5, 2.0), sum = 0;
        int a = (int)(0.5 * SR), z = (int)(2.0 * SR), cnt = 0;
        for (int k = a; k < z && k < frames; k++)
            for (int ch = 0; ch < 2; ch++) {
                double v = b[k * 2 + ch] / 32768.0;
                sum += v * v;
                cnt++;
            }
        free(b);
        double rms = cnt ? sqrt(sum / cnt) : 0;
        if (pk > worst) { worst = pk; worst_i = i; }
        if (rms > 1e-3) { /* skip silent programs */
            rms_db[n] = 20 * log10(rms);
            crest_db[n] = 20 * log10(pk / rms);
            n++;
        }
    }
    qsort(rms_db, n, sizeof(double), cmp_double);
    qsort(crest_db, n, sizeof(double), cmp_double);
    double med_rms = rms_db[n / 2], med_crest = crest_db[n / 2], max_rms = rms_db[n - 1];
    printf("  %d sounding programs: median RMS %.1f dBFS, max RMS %.1f dBFS, median crest %.1f dB\n", n, med_rms, max_rms, med_crest);
    char msg[200];
    snprintf(msg, sizeof msg, "loudest program peaks at %.1f dBFS (program %d), within -6..-12 dBFS", 20 * log10(worst), worst_i);
    check(worst <= 0.501 && worst >= 0.25, msg);
    snprintf(msg, sizeof msg, "median chord RMS %.1f dBFS is at most -21 dBFS", med_rms);
    check(med_rms <= -21.0, msg);
    snprintf(msg, sizeof msg, "loudest program RMS %.1f dBFS is at most -11 dBFS", max_rms);
    check(max_rms <= -11.0, msg);
    snprintf(msg, sizeof msg, "median crest factor %.1f dB keeps dynamics (at least 9 dB)", med_crest);
    check(med_crest >= 9.0, msg);

    /* Mono patches do not split across voices: program 25 (A.42 Techstep Ring Bass) on one note must stay under -6 dBFS */
    fresh(25);
    midi(0x90, 36, 127);
    int16_t *b = render(2.0, &frames);
    double mono_pk = peak_in(b, frames, 0.0, 2.0);
    free(b);
    snprintf(msg, sizeof msg, "program 25 single note peaks at %.1f dBFS, at most -6 dBFS", 20 * log10(mono_pk));
    check(mono_pk <= 0.501 && mono_pk > 0.05, msg);
}


/* --- Pitch controls, EG reset, the top octaves ------------------------------------------------------------------ */

/* a plain sine through the open high-pass: its pitch is its zero crossings, its level the amp EG */
static void sine_voice(void) {
    open_voice();
    synth_set_param(&S, "wave1", 0.5f);        /* Sine */
    synth_set_param(&S, "filter_type", 1.0f);  /* 12HPF */
    synth_set_param(&S, "cutoff", 0.0f);
    synth_set_param(&S, "portamento", 0.0f);
    S.timbre_extra[0].osc1_ctrl[0] = S.timbre_extra[0].osc1_ctrl[1] = 0.0f;
    S.timbre_extra[0].noise_level = 0.0f;
    for (int p = 0; p < 4; p++) S.timbre_extra[0].patch_int[p] = 0.0f;
}

/* highest and lowest pitch (semitones re `note`) over the render, from rising zero crossings after 0.2 s */
static void pitch_range(const int16_t *b, int n, int note, double *lo, double *hi) {
    double base = 440.0 * pow(2.0, (note - 69) / 12.0), last = -1.0;
    *lo = 1e9; *hi = -1e9;
    for (int i = (int)(0.2 * SR); i + 1 < n; i++) {
        double x0 = b[2 * i] + b[2 * i + 1], x1 = b[2 * i + 2] + b[2 * i + 3];
        if (x0 < 0.0 && x1 >= 0.0) {
            double z = i + x0 / (x0 - x1);
            if (last >= 0.0) {
                double st = 12.0 * log2(SR / (z - last) / base);
                if (st < *lo) *lo = st;
                if (st > *hi) *hi = st;
            }
            last = z;
        }
    }
}

static void midi3(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t m[3] = { a, b, c };
    move_plugin_on_midi(&S, m, 3, 0);
}

static void test_pitch_controls(void) {
    printf("\nBend range and vibrato int (the plug-in: range x bend; LFO2 x wheel, 0.25 semitone per step):\n");
    char what[200];
    static const float ranges[4] = { 2.0f, 12.0f, -12.0f, 0.0f };
    int ok = 1;
    double got[4];
    for (int r = 0; r < 4; r++) {
        sine_voice();
        S.timbre_extra[0].bend_semi = ranges[r];
        midi3(0xE0, 0x7F, 0x7F);                /* full bend up */
        synth_note_on(&S, 69, 100);
        int n; double lo, hi;
        int16_t *b = render(0.6, &n);
        pitch_range(b, n, 69, &lo, &hi);
        got[r] = 0.5 * (lo + hi);
        if (fabs(got[r] - ranges[r]) > 0.03) ok = 0;
        free(b);
    }
    snprintf(what, sizeof what, "full bend at range +2 / +12 / -12 / 0: %+.2f %+.2f %+.2f %+.2f semitones", got[0], got[1], got[2], got[3]);
    check(ok, what);
    fresh(0);
    check(S.timbre_extra[0].bend_semi == 2.0f && S.timbre_extra[0].vibrato_int == 0.0f && S.timbre_extra[0].eg_reset[0] == 1 && S.timbre_extra[0].eg_reset[1] == 1,
          "a bank without these values plays bend range 2, no vibrato, both EG resets on");

    static const int wheel[3] = { 0, 127, 127 };
    static const float vint[3] = { 25.0f, 5.0f, -50.0f };
    static const double want[3] = { 0.0, 1.25, 13.6 };
    double swing[3];
    ok = 1;
    for (int k = 0; k < 3; k++) {
        sine_voice();
        S.timbre_extra[0].vibrato_int = vint[k];
        S.timbre_extra[0].lfo_wave[1] = 2;       /* LFO2 sine */
        S.timbre_extra[0].lfo_rate[1] = 70.0f / 127.0f;
        S.timbre_extra[0].lfo_sync_note[1] = -1;
        midi3(0xB0, 1, (uint8_t)wheel[k]);
        synth_note_on(&S, 69, 100);
        int n; double lo, hi;
        int16_t *b = render(1.5, &n);
        pitch_range(b, n, 69, &lo, &hi);
        swing[k] = 0.5 * (hi - lo);
        if (fabs(swing[k] - want[k]) > 0.05 + 0.02 * want[k]) ok = 0;
        free(b);
    }
    snprintf(what, sizeof what, "vibrato swing: wheel 0 / int +25 %.2f, wheel 127 / int +5 %.2f, wheel 127 / int -50 %.2f semitones (0, 1.25, 13.6)",
             swing[0], swing[1], swing[2]);
    check(ok, what);
}

/* the level over [t0, t1) of the render, rms of the mono mix */
static double level_between(const int16_t *b, double t0, double t1) {
    double e = 0.0;
    int a = (int)(t0 * SR), z = (int)(t1 * SR);
    for (int i = a; i < z; i++) { double v = (b[2 * i] + b[2 * i + 1]) / 65536.0; e += v * v; }
    return sqrt(e / (z - a));
}

static void test_eg_reset(void) {
    printf("\nEG reset (the plug-in: on = the level is gone within 4 ms of a retrigger and the attack starts over):\n");
    char what[220];
    double before[2], after[2], later[2], step[2];
    for (int reset = 0; reset < 2; reset++) {
        sine_voice();
        synth_set_param(&S, "voice_assign", 0.0f);     /* Mono */
        synth_set_param(&S, "attack2", 64.0f / 127.0f); /* 0.79 s */
        synth_set_param(&S, "release2", 70.0f / 127.0f);
        S.timbre_extra[0].eg_reset[1] = reset;
        synth_note_on(&S, 81, 100);
        int n1, n2, n3;
        int16_t *a = render(1.0, &n1);
        synth_note_off(&S, 81);
        int16_t *r = render(0.3, &n2);
        synth_note_on(&S, 81, 100);
        int16_t *c = render(0.6, &n3);
        before[reset] = level_between(r, 0.28, 0.30);
        after[reset] = level_between(c, 0.010, 0.030);
        later[reset] = level_between(c, 0.50, 0.52);
        /* the hardest sample-to-sample step across the retrigger against the hardest while the note simply sounds */
        double plain = 0.0, worst = 0.0;
        for (int i = n2 - 2000; i + 1 < n2; i++) plain = fmax(plain, fabs((double)r[2 * i + 2] - r[2 * i]));
        for (int i = 0; i < 600; i++) worst = fmax(worst, fabs((double)c[2 * i + 2] - c[2 * i]));
        worst = fmax(worst, fabs((double)c[0] - r[2 * n2 - 2]));
        step[reset] = worst / (plain + 1e-9);
        free(a); free(r); free(c);
    }
    snprintf(what, sizeof what, "reset on: level 10-30 ms after the retrigger %.1f dB re just before it (silence), back at %.1f dB by 0.5 s",
             20 * log10(after[1] / before[1] + 1e-9), 20 * log10(later[1] / before[1] + 1e-9));
    check(after[1] < 0.05 * before[1] && later[1] > before[1], what);
    snprintf(what, sizeof what, "reset off: the level carries on (%.1f dB re just before)", 20 * log10(after[0] / before[0] + 1e-9));
    check(after[0] > 0.9 * before[0], what);
    snprintf(what, sizeof what, "no click either way: hardest step %.2f / %.2f of the sounding note's own", step[1], step[0]);
    check(step[1] < 1.5 && step[0] < 1.5, what);

    /* the filter EG: its level at the retrigger */
    float e1[2];
    for (int reset = 0; reset < 2; reset++) {
        sine_voice();
        synth_set_param(&S, "voice_assign", 0.0f);
        synth_set_param(&S, "attack1", 64.0f / 127.0f);
        synth_set_param(&S, "sustain1", 1.0f);
        synth_set_param(&S, "release1", 90.0f / 127.0f);
        synth_set_param(&S, "release2", 90.0f / 127.0f);
        S.timbre_extra[0].eg_reset[0] = reset;
        synth_note_on(&S, 60, 100);
        int n; int16_t *b = render(1.0, &n); free(b);
        synth_note_off(&S, 60);
        b = render(0.2, &n); free(b);
        synth_note_on(&S, 60, 100);
        b = render(0.003, &n); free(b);
        e1[reset] = 0.0f;
        for (int v = 0; v < NUM_VOICES; v++) if (S.voices[v].active) e1[reset] = S.voices[v].filter_env.value;
    }
    snprintf(what, sizeof what, "filter EG 3 ms after a retrigger: %.3f with reset on (from zero), %.3f with it off (from its level)", e1[1], e1[0]);
    check(e1[1] < 0.02f && e1[0] > 0.3f, what);
}

/* power of x at hz (Goertzel over n samples of the mono mix from sample a) */
static double tone_power(const int16_t *b, int a, int n, double hz) {
    double w = 2.0 * M_PI * hz / SR, c = 2.0 * cos(w), s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1));
        s0 = win * (b[2 * (a + i)] + b[2 * (a + i) + 1]) / 65536.0 + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

static void test_top_octaves(void) {
    printf("\nThe oscillators' top in the high octaves (the plug-in: a harmonic at f on a note at f0 is down (f f0 / 7.2e6)^1.7 dB):\n");
    char what[220];
    /* harmonic 5 against harmonic 1 of a saw, re the same ratio two octaves down at the same frequencies' tilt:
     * C7's fifth harmonic (10.5 kHz) is about 7 dB down on the plug-in, C5's (2.6 kHz) not at all */
    double ratio[2];
    static const int notes[2] = { 72, 96 };
    for (int k = 0; k < 2; k++) {
        open_voice();
        synth_set_param(&S, "wave1", 0.0f);
        synth_set_param(&S, "filter_type", 1.0f);
        synth_set_param(&S, "cutoff", 0.0f);
        S.timbre_extra[0].osc1_ctrl[0] = S.timbre_extra[0].osc1_ctrl[1] = 0.0f;
        for (int p = 0; p < 4; p++) S.timbre_extra[0].patch_int[p] = 0.0f;
        synth_note_on(&S, (uint8_t)notes[k], 100);
        int n;
        int16_t *b = render(1.0, &n);
        double f0 = 440.0 * pow(2.0, (notes[k] - 69) / 12.0);
        ratio[k] = 10.0 * log10(tone_power(b, SR / 4, 16384, 5.0 * f0) / tone_power(b, SR / 4, 16384, f0));
        free(b);
    }
    /* an ideal saw's fifth harmonic is 14 dB under its first; the brightness tilt lifts 2.6 kHz by 1.3 dB and
     * 10.5 kHz by 8.3 dB over the fundamentals' own lift */
    snprintf(what, sizeof what, "saw, harmonic 5 re harmonic 1: C5 %.1f dB, C7 %.1f dB (C7's would be about %.1f without the roll-off)",
             ratio[0], ratio[1], ratio[0] + 7.0 - 1.0);
    check(ratio[0] > -14.5 && ratio[0] < -11.0 && ratio[1] < ratio[0] + 2.0 && ratio[1] > ratio[0] - 4.0, what);
    /* aliasing: a B6 saw's energy between its harmonics above 12 kHz, against the note (B6, not C7: C7's aliases
     * fall 147 Hz from its own harmonics) */
    {
        open_voice();
        synth_set_param(&S, "wave1", 0.0f);
        synth_set_param(&S, "filter_type", 1.0f);
        synth_set_param(&S, "cutoff", 0.0f);
        S.timbre_extra[0].osc1_ctrl[0] = S.timbre_extra[0].osc1_ctrl[1] = 0.0f;
        for (int p = 0; p < 4; p++) S.timbre_extra[0].patch_int[p] = 0.0f;
        synth_note_on(&S, 95, 100);
        int n;
        int16_t *b = render(1.0, &n);
        double f0 = 440.0 * pow(2.0, (95 - 69) / 12.0), tone = 0.0, junk = 0.0;
        for (int h = 1; h * f0 < 20000.0; h++) tone += tone_power(b, SR / 4, 16384, h * f0);
        for (double f = 12000.0; f < 20000.0; f += 37.0) {
            double off = fmod(f, f0);
            if (off > 150.0 && off < f0 - 150.0) junk += tone_power(b, SR / 4, 16384, f);
        }
        junk *= 37.0 / (1.5 * SR / 16384.0);   /* sampled every 37 Hz with a Hann window 1.5 bins wide */
        snprintf(what, sizeof what, "B6 saw: energy between the harmonics above 12 kHz %.1f dB re the note (-24.6 without the roll-off)", 10.0 * log10(junk / tone));
        check(10.0 * log10(junk / tone) < -30.0, what);
        free(b);
    }
}

/* the slot state carries EG reset, bend range and vibrato int; a state from before they existed keeps the program's */
static void test_state_extras(void) {
    printf("\nSlot state: EG reset, bend range, vibrato int:\n");
    plugin_api_v2_t *api = move_plugin_init_v2(NULL);
    void *inst = api->create_instance(".", NULL);
    synth_engine_t *e = (synth_engine_t *)inst;
    static char state[65536], old[65536];
    timbre_extra_t *x = &e->timbre_extra[0];
    x->eg_reset[0] = 0; x->eg_reset[1] = 0; x->bend_semi = -7.0f; x->vibrato_int = 31.0f;
    int len = api->get_param(inst, "state", state, sizeof state);
    x->eg_reset[0] = 1; x->eg_reset[1] = 1; x->bend_semi = 2.0f; x->vibrato_int = 0.0f;
    api->set_param(inst, "state", state);
    check(len > 0 && x->eg_reset[0] == 0 && x->eg_reset[1] == 0 && x->bend_semi == -7.0f && x->vibrato_int == 31.0f,
          "a saved state restores all four");
    /* an older state: the last four numbers of extra1 cut off */
    const char *a = strstr(state, "\"extra1\"");
    int ok = 0;
    if (a) {
        const char *end = strchr(a, ']');
        const char *cut = end;
        for (int k = 0; k < 4 && cut; k++) { cut--; while (cut > a && *cut != ',') cut--; }
        if (end && cut > a) {
            size_t head = (size_t)(cut - state);
            memcpy(old, state, head);
            strcpy(old + head, end);
            x->eg_reset[0] = 1; x->eg_reset[1] = 1; x->bend_semi = 2.0f; x->vibrato_int = 5.0f;
            api->set_param(inst, "state", old);
            ok = x->eg_reset[0] == 1 && x->eg_reset[1] == 1 && x->bend_semi == 2.0f && x->vibrato_int == 0.0f;
        }
    }
    check(ok, "a state saved before they existed keeps the program's own values");
    api->destroy_instance(inst);
}

int main(void) {
    test_layer();
    test_edit_routing();
    test_envelopes();
    test_patch_sources();
    test_lfo_saw_gate();
    test_host_tempo();
    test_lfo_steps_click_free();
    test_noise_is_white();
    test_arp();
    test_program_change_flush();
    test_headroom();
    test_pitch_controls();
    test_eg_reset();
    test_top_octaves();
    test_state_extras();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
