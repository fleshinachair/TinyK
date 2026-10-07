/*
 * Offline renderer for the TinyK DSP engine (links src/dsp/dsp.c and its presets.h).
 *
 *   test_render <preset> <out.wav> [note[,note...]=60] [gate_s=2.0] [tail_s=1.5]
 *               [--set tuning=value ...] [--param engine_param=value ...]
 *   test_render --list-tuning
 *
 *   <preset>  index 0-127, or a case-insensitive part of the label ("A.21", "acid 303").
 *             It must match exactly one preset.
 *   note      MIDI note(s) played together (60 = C3 in the Korg convention used by the references).
 *   gate_s    how long the note is held; tail_s: how long to keep rendering after note-off.
 *   --param   applies synth_set_param(name, value) after the preset loads, e.g.
 *             --param voice_mode=1 --param timbre_balance=1  (Timbre 2 only), --param cutoff=0.5.
 *
 * Writes 44.1 kHz stereo 16-bit PCM of length gate_s + tail_s. Build (from the repo root):
 *
 *   gcc   -O2 -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o tools/test_render
 *   clang -O2 -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o tools/test_render
 *   zig cc -O2 -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o tools/test_render
 *
 * Optional defines:
 *   -DTINYK_DIAG    also report non-finite values the engine produced ("nonfinite=<n>").
 *   -DTINYK_TUNING  make the engine's global constants adjustable (--set, or the library API).
 *   -DTINYK_LIB     build as a shared library for tools/calibrate_dsp.py instead of an executable
 *                   (exports tinyk_render, tinyk_render_patch, tinyk_render_patch_events, tinyk_set_tuning).
 */
#include "dsp.h"
#include "presets.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static synth_engine_t synth;

#define MAX_PARAM_OVERRIDES 16
static const char *override_name[MAX_PARAM_OVERRIDES];
static float override_value[MAX_PARAM_OVERRIDES];
static int num_overrides = 0;

#ifdef TINYK_DIAG
extern unsigned tinyk_diag_nonfinite;
#define NONFINITE_COUNT tinyk_diag_nonfinite
#else
#define NONFINITE_COUNT 0u
#endif

#ifdef TINYK_TUNING
static const struct { const char *name; float *value; } TUNING_TABLE[] = {
    { "cutoff_base_hz",     &tinyk_tuning.cutoff_base_hz },
    { "cutoff_octaves",     &tinyk_tuning.cutoff_octaves },
    { "cutoff_floor_hz",    &tinyk_tuning.cutoff_floor_hz },
    { "cutoff_ceil_hz",     &tinyk_tuning.cutoff_ceil_hz },
    { "bpf_k0",             &tinyk_tuning.bpf_k0 },
    { "env_octaves",        &tinyk_tuning.env_octaves },
    { "res_damping_range",  &tinyk_tuning.res_damping_range },
    { "lp24_res_scale",     &tinyk_tuning.lp24_res_scale },
    { "drive_gain",         &tinyk_tuning.drive_gain },
    { "attack_scale",       &tinyk_tuning.attack_scale },
    { "decay_scale",        &tinyk_tuning.decay_scale },
    { "release_scale",      &tinyk_tuning.release_scale },
    { "mixer_trim",         &tinyk_tuning.mixer_trim },
    { "delay_send_scale",   &tinyk_tuning.delay_send_scale },
    { "patch_cutoff_octaves", &tinyk_tuning.patch_cutoff_octaves },
    { "patch_pitch_scale",  &tinyk_tuning.patch_pitch_scale },
    { "lfo_tempo_bpm",      &tinyk_tuning.lfo_tempo_bpm },
    { "patch_int_curve",    &tinyk_tuning.patch_int_curve },
    { "tilt_db",            &tinyk_tuning.tilt_db },
    { "tilt_hz",            &tinyk_tuning.tilt_hz },
    { "patch_pan_curve",    &tinyk_tuning.patch_pan_curve },
    { "bpf_cutoff_octaves", &tinyk_tuning.bpf_cutoff_octaves },
    { "bpf_cutoff_offset",  &tinyk_tuning.bpf_cutoff_offset },
    { "dist_ceiling",       &tinyk_tuning.dist_ceiling },
    { "noise_tilt_db",      &tinyk_tuning.noise_tilt_db },
    { "xmod_semitones",     &tinyk_tuning.xmod_semitones },
    { "xmod_offset_semitones", &tinyk_tuning.xmod_offset_semitones },
    { "hpf_ceil_hz",        &tinyk_tuning.hpf_ceil_hz },
    { "unison_spread",      &tinyk_tuning.unison_spread },
    { "unison_cents_scale", &tinyk_tuning.unison_cents_scale },
};
#define TUNING_COUNT ((int)(sizeof TUNING_TABLE / sizeof TUNING_TABLE[0]))

static int set_tuning(const char *name, double v) {
    for (int i = 0; i < TUNING_COUNT; i++) {
        if (strcmp(TUNING_TABLE[i].name, name) == 0) {
            *TUNING_TABLE[i].value = (float)v;
            return 1;
        }
    }
    return 0;
}
#endif

#ifdef TINYK_TUNING
void tinyk_load_patch(synth_engine_t *synth, const struct Preset *p, int slot);
#endif

/* Plays `num_notes` for gate_s, releases, renders total_frames of audio into out (interleaved L/R).
 * With patch != NULL (TINYK_TUNING builds) that patch is played from slot `preset` instead of the bank's. */
static void render_notes(int preset, const struct Preset *patch, const int *notes, int num_notes, double gate_s,
                         int total_frames, int16_t *out) {
    int gate = (int)(gate_s * MOVE_SAMPLE_RATE);
    synth_init(&synth);
#ifdef TINYK_TUNING
    if (patch) tinyk_load_patch(&synth, patch, preset);
    else
#endif
    synth_load_preset(&synth, preset);
    (void)patch;
    for (int i = 0; i < num_overrides; i++) synth_set_param(&synth, override_name[i], override_value[i]);
    for (int i = 0; i < num_notes; i++) synth_note_on(&synth, (uint8_t)notes[i], 100);

    int done = 0, released = 0;
    while (done < total_frames) {
        if (!released && done >= gate) {
            for (int i = 0; i < num_notes; i++) synth_note_off(&synth, (uint8_t)notes[i]);
            released = 1;
        }
        int n = MOVE_FRAMES_PER_BLOCK;
        if (done + n > total_frames) n = total_frames - done;
        if (!released && done + n > gate) n = gate - done; /* land exactly on the note-off */
        if (n <= 0) n = 1;
        synth_render(&synth, out + done * 2, n);
        done += n;
    }
}

#ifdef TINYK_LIB
#ifdef _WIN32
#define TK_EXPORT __declspec(dllexport)
#else
#define TK_EXPORT __attribute__((visibility("default")))
#endif

static int render_float(int preset, const struct Preset *patch, int note, double gate_s, double total_s,
                        float *out_lr, int max_frames) {
    int frames = (int)(total_s * MOVE_SAMPLE_RATE);
    if (preset < 0 || preset > 127 || frames <= 0 || frames > max_frames) return -1;
    int16_t *tmp = calloc((size_t)frames * 2, sizeof(int16_t));
    if (!tmp) return -1;
    render_notes(preset, patch, &note, 1, gate_s, frames, tmp);
    for (int i = 0; i < frames * 2; i++) out_lr[i] = (float)tmp[i] / 32768.0f;
    free(tmp);
    return frames;
}

/* Renders one note into out_lr (interleaved float, -1..1, capacity max_frames); returns frames written. */
TK_EXPORT int tinyk_render(int preset, int note, double gate_s, double total_s, float *out_lr, int max_frames) {
    return render_float(preset, NULL, note, gate_s, total_s, out_lr, max_frames);
}

/* As tinyk_render, but plays an external patch: t1/t2 hold the 26 struct TimbreParams floats in
 * declaration order, fx the 5 FX floats (chorus_mix, delay_time, delay_feedback, delay_mix, delay_sync). */
TK_EXPORT int tinyk_render_patch(int slot, int voice_mode, const float *t1, const float *t2, const float *fx,
                                 int note, double gate_s, double total_s, float *out_lr, int max_frames) {
#ifdef TINYK_TUNING
    struct Preset p;
    memset(&p, 0, sizeof p);
    p.label = "external";
    p.voice_mode = voice_mode;
    memcpy(&p.t1, t1, sizeof p.t1);
    memcpy(&p.t2, t2, sizeof p.t2);
    p.chorus_mix = fx[0];
    p.delay_time = fx[1];
    p.delay_feedback = fx[2];
    p.delay_mix = fx[3];
    p.delay_sync = fx[4];
    return render_float(slot, &p, note, gate_s, total_s, out_lr, max_frames);
#else
    (void)slot; (void)voice_mode; (void)t1; (void)t2; (void)fx; (void)note; (void)gate_s; (void)total_s;
    (void)out_lr; (void)max_frames;
    return -1;
#endif
}

TK_EXPORT int tinyk_timbre_floats(void) { return (int)(sizeof(struct TimbreParams) / sizeof(float)); }

#ifdef TINYK_TUNING
int tinyk_dsp_bank_scan(const char *dir);
int tinyk_dsp_bank_count(void);
const char *tinyk_dsp_bank_name(int b);
int tinyk_dsp_bank_preset(int b, int idx, float *t1, float *t2, float *fx, char *label, int label_len);
TK_EXPORT int tinyk_bank_scan(const char *dir) { return tinyk_dsp_bank_scan(dir); }
TK_EXPORT int tinyk_bank_count(void) { return tinyk_dsp_bank_count(); }
int tinyk_dsp_v2_create(const char *module_dir);
void tinyk_dsp_v2_set(const char *key, const char *val);
int tinyk_dsp_v2_get(const char *key, char *buf, int buf_len);
TK_EXPORT int tinyk_v2_create(const char *module_dir) { return tinyk_dsp_v2_create(module_dir); }
TK_EXPORT void tinyk_v2_set(const char *key, const char *val) { tinyk_dsp_v2_set(key, val); }
TK_EXPORT int tinyk_v2_get(const char *key, char *buf, int buf_len) { return tinyk_dsp_v2_get(key, buf, buf_len); }
TK_EXPORT const char *tinyk_bank_name(int b) { return tinyk_dsp_bank_name(b); }
TK_EXPORT int tinyk_bank_preset(int b, int idx, float *t1, float *t2, float *fx, char *label, int label_len) {
    return tinyk_dsp_bank_preset(b, idx, t1, t2, fx, label, label_len);
}
int tinyk_dsp_bank_arp(int b, int idx, float *arp);
TK_EXPORT int tinyk_bank_arp(int b, int idx, float *arp) { return tinyk_dsp_bank_arp(b, idx, arp); }
/* Bank / program selection through the public parameter API, then the active preset's name */
TK_EXPORT int tinyk_select(const char *key, const char *val, char *name, int name_len) {
    synth_set_param(&synth, key, (float)atof(val));
    return snprintf(name, (size_t)name_len, "%d", synth.current_preset);
}
#endif

/* Sets the cutoff knob (0..1) of every timbre that plays, the way a user turning it would. */
static void set_cutoff_all(float v, int layer) {
    if (layer) {
        synth_set_param(&synth, "timbre_edit", 1.0f);
        synth_set_param(&synth, "cutoff", v);
        synth_set_param(&synth, "timbre_edit", 0.0f);
    }
    synth_set_param(&synth, "cutoff", v);
}

/* Plays an external patch (as tinyk_render_patch) with timed note events and cutoff automation.
 *   ev_frame/ev_note/ev_vel: n_events events sorted by frame; velocity 0 = note off.
 *   cutoff: curve_len knob values (0..1) spread evenly over the render, applied at every block start;
 *           curve_len 0 leaves the patch's own cutoff.
 * Writes `frames` interleaved stereo floats (-1..1) to out_lr; returns frames, or -1. */
TK_EXPORT int tinyk_render_patch_events(int slot, int voice_mode, const float *t1, const float *t2, const float *fx,
                                        const int *ev_frame, const int *ev_note, const int *ev_vel, int n_events,
                                        const float *cutoff, int curve_len, int frames, float *out_lr) {
#ifdef TINYK_TUNING
    if (frames <= 0 || slot < 0 || slot > 127) return -1;
    struct Preset p;
    memset(&p, 0, sizeof p);
    p.label = "external";
    p.voice_mode = voice_mode;
    memcpy(&p.t1, t1, sizeof p.t1);
    memcpy(&p.t2, t2, sizeof p.t2);
    p.chorus_mix = fx[0];
    p.delay_time = fx[1];
    p.delay_feedback = fx[2];
    p.delay_mix = fx[3];
    p.delay_sync = fx[4];

    int16_t *tmp = calloc((size_t)frames * 2, sizeof(int16_t));
    if (!tmp) return -1;
    synth_init(&synth);
    tinyk_load_patch(&synth, &p, slot);

    int done = 0, ev = 0;
    while (done < frames) {
        while (ev < n_events && ev_frame[ev] <= done) {
            if (ev_vel[ev] > 0) synth_note_on(&synth, (uint8_t)ev_note[ev], (uint8_t)ev_vel[ev]);
            else synth_note_off(&synth, (uint8_t)ev_note[ev]);
            ev++;
        }
        if (curve_len > 0) {
            int i = (int)((long long)done * curve_len / frames);
            set_cutoff_all(cutoff[i < curve_len ? i : curve_len - 1], voice_mode == 1);
        }
        int n = MOVE_FRAMES_PER_BLOCK;
        if (done + n > frames) n = frames - done;
        if (ev < n_events && ev_frame[ev] < done + n) n = ev_frame[ev] - done; /* land exactly on the next event */
        if (n <= 0) n = 1;
        synth_render(&synth, tmp + done * 2, n);
        done += n;
    }
    for (int i = 0; i < frames * 2; i++) out_lr[i] = (float)tmp[i] / 32768.0f;
    free(tmp);
    return frames;
#else
    (void)slot; (void)voice_mode; (void)t1; (void)t2; (void)fx; (void)ev_frame; (void)ev_note; (void)ev_vel;
    (void)n_events; (void)cutoff; (void)curve_len; (void)frames; (void)out_lr;
    return -1;
#endif
}

TK_EXPORT int tinyk_set_tuning(const char *name, double v) {
#ifdef TINYK_TUNING
    return set_tuning(name, v);
#else
    (void)name; (void)v;
    return 0;
#endif
}

TK_EXPORT unsigned tinyk_nonfinite(void) { return NONFINITE_COUNT; }

#else /* executable */

static void put_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_u16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static int write_wav(const char *path, const int16_t *lr, int frames) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t data_bytes = (uint32_t)frames * 4;
    fwrite("RIFF", 1, 4, f); put_u32(f, 36 + data_bytes); fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16); put_u16(f, 1); put_u16(f, 2);
    put_u32(f, MOVE_SAMPLE_RATE); put_u32(f, MOVE_SAMPLE_RATE * 4); put_u16(f, 4); put_u16(f, 16);
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
    fwrite(lr, 4, (size_t)frames, f);
    fclose(f);
    return 1;
}

static int contains_nocase(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

/* Index or label fragment -> preset index, or -1 (message printed). */
static int find_preset(const char *spec) {
    char *end;
    long idx = strtol(spec, &end, 10);
    if (*spec && *end == '\0') return (idx >= 0 && idx < 128) ? (int)idx : -1;

    int found = -1, matches = 0;
    for (int i = 0; i < 128; i++) {
        if (contains_nocase(FACTORY_PRESETS[i].label, spec)) { found = i; matches++; }
    }
    if (matches == 1) return found;
    fprintf(stderr, "preset \"%s\" matches %d presets", spec, matches);
    if (matches > 1) {
        fprintf(stderr, ":");
        for (int i = 0; i < 128; i++)
            if (contains_nocase(FACTORY_PRESETS[i].label, spec)) fprintf(stderr, " [%d] %s;", i, FACTORY_PRESETS[i].label);
    }
    fprintf(stderr, "\n");
    return -1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s <preset index|name> <out.wav> [note[,note...]=60] [gate_s=2.0] [tail_s=1.5]\n"
            "          [--set tuning=value ...] [--param engine_param=value ...]\n"
            "       %s --list-tuning\n", prog, prog);
}

int main(int argc, char **argv) {
    const char *pos[5];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list-tuning") == 0) {
#ifdef TINYK_TUNING
            for (int t = 0; t < TUNING_COUNT; t++) printf("%s=%g\n", TUNING_TABLE[t].name, *TUNING_TABLE[t].value);
            return 0;
#else
            fprintf(stderr, "built without -DTINYK_TUNING\n");
            return 2;
#endif
        } else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc) {
#ifdef TINYK_TUNING
            char *eq = strchr(argv[++i], '=');
            if (!eq) { usage(argv[0]); return 2; }
            *eq = '\0';
            if (!set_tuning(argv[i], atof(eq + 1))) { fprintf(stderr, "unknown tuning constant \"%s\"\n", argv[i]); return 2; }
#else
            fprintf(stderr, "--set needs a build with -DTINYK_TUNING\n");
            return 2;
#endif
        } else if (strcmp(argv[i], "--param") == 0 && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (!eq || num_overrides >= MAX_PARAM_OVERRIDES) { usage(argv[0]); return 2; }
            *eq = '\0';
            override_name[num_overrides] = argv[i];
            override_value[num_overrides++] = (float)atof(eq + 1);
        } else if (npos < 5) {
            pos[npos++] = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (npos < 2) { usage(argv[0]); return 2; }

    int preset = find_preset(pos[0]);
    if (preset < 0) return 2;

    int notes[8], num_notes = 0;
    char note_arg[64];
    snprintf(note_arg, sizeof note_arg, "%s", npos > 2 ? pos[2] : "60");
    for (char *tok = strtok(note_arg, ","); tok && num_notes < 8; tok = strtok(NULL, ",")) notes[num_notes++] = atoi(tok);
    double gate_s = npos > 3 ? atof(pos[3]) : 2.0;
    double tail_s = npos > 4 ? atof(pos[4]) : 1.5;

    int notes_ok = num_notes > 0;
    for (int i = 0; i < num_notes; i++) notes_ok = notes_ok && notes[i] >= 0 && notes[i] <= 127;
    if (!notes_ok || gate_s < 0 || tail_s < 0 || gate_s + tail_s <= 0) {
        fprintf(stderr, "invalid note/gate/tail\n");
        return 2;
    }

    int total = (int)((gate_s + tail_s) * MOVE_SAMPLE_RATE);
    int16_t *out = calloc((size_t)total * 2, sizeof(int16_t));
    if (!out) return 1;
    render_notes(preset, NULL, notes, num_notes, gate_s, total, out);

    int ok = write_wav(pos[1], out, total);
    free(out);
    if (!ok) {
        fprintf(stderr, "cannot write %s\n", pos[1]);
        return 1;
    }
    printf("preset %d (%s): %d note%s, gate %.2fs + tail %.2fs -> %s nonfinite=%u\n", preset,
           FACTORY_PRESETS[preset].label, num_notes, num_notes == 1 ? "" : "s", gate_s, tail_s, pos[1],
           NONFINITE_COUNT);
    return 0;
}
#endif
