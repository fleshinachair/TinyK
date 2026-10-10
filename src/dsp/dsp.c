#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "dsp.h"
#include "presets.h"
#include "syx_bank.h"
#include "dwgs_waves.h"
#include "vox_pulse.h"

/* An instance's active bank (synth->bank_file): 0 = built-in (presets.h), 1..g_syx_bank_count = .syx dumps found
 * in <module>/banks/. The decoded banks are shared by all instances (read-only once scanned); the choice is not. */
static const struct Preset *active_presets(const synth_engine_t *synth) {
    int b = synth->bank_file;
    return (b > 0 && b <= g_syx_bank_count) ? g_syx_banks[b - 1].presets : FACTORY_PRESETS;
}

static const char *active_bank_name(const synth_engine_t *synth) {
    int b = synth->bank_file;
    return (b > 0 && b <= g_syx_bank_count) ? g_syx_banks[b - 1].name : "Built-in";
}

static void category_names(const synth_engine_t *synth, const char *names[8]);

/* --- Program list ---------------------------------------------------------------------------------------------------
 * TinyK has no vocoder (no modulator input), so programs the bank flags as vocoder (Preset.voice_mode 2, decoded from
 * the program's own voice mode byte, not from their slot) are left out of what the instance offers. Every bank keeps
 * the hardware matrix: Category = matrix row (Trance ... SE/Hit, row 8 "Other"), Program = the category's playable
 * programs in matrix order (A side, then B side), each keeping its own A.11-B.88 coordinates and label. A category
 * holds however many playable programs its row has (0-16); a row without any is not offered, so a factory-layout
 * bank shows 7 categories and 112 programs. The jog wheel counts the playable programs in matrix order (ordinals,
 * 0..play_n-1). A bank of nothing but vocoder programs keeps all 128 rather than offering nothing. Per instance,
 * rebuilt on a bank change; the decoded banks stay read-only. */
static void rebuild_playlist(synth_engine_t *synth) {
    const struct Preset *pr = active_presets(synth);
    int voc = 0, n = 0, ncat = 0;
    for (int r = 0; r < NUM_PRESETS; r++) if (pr[r].voice_mode == 2) voc++;
    if (voc >= NUM_PRESETS) voc = 0; /* nothing but vocoder programs: offer them all */
    for (int r = 0; r < NUM_PRESETS; r++) {
        synth->slot_cat[r] = -1;
        synth->slot_pos[r] = 0;
        if (voc && pr[r].voice_mode == 2) {
            synth->play_ord[r] = -1;
        } else {
            synth->play_ord[r] = (int8_t)n;
            synth->play_map[n++] = (uint8_t)r;
        }
    }
    for (int row = 0; row < 8; row++) {
        int cnt = 0;
        for (int side = 0; side < 2; side++) {
            for (int col = 0; col < 8; col++) {
                int slot = side * 64 + row * 8 + col;
                if (synth->play_ord[slot] < 0) continue;
                synth->cat_slot[ncat][cnt] = (uint8_t)slot;
                synth->slot_cat[slot] = (int8_t)ncat;
                synth->slot_pos[slot] = (int8_t)cnt;
                cnt++;
            }
        }
        if (cnt) {
            synth->cat_row[ncat] = (uint8_t)row;
            synth->cat_n[ncat] = (uint8_t)cnt;
            ncat++;
        }
    }
    synth->play_n = n;
    synth->play_ncat = ncat;
}

/* The nearest playable slot at or after `raw` (else before it) */
static int playable_raw(const synth_engine_t *synth, int raw) {
    if (raw < 0) raw = 0;
    if (raw >= NUM_PRESETS) raw = NUM_PRESETS - 1;
    if (synth->play_ord[raw] >= 0) return raw;
    for (int r = raw + 1; r < NUM_PRESETS; r++) if (synth->play_ord[r] >= 0) return r;
    for (int r = raw - 1; r >= 0; r--) if (synth->play_ord[r] >= 0) return r;
    return 0;
}

static int category_clamp(const synth_engine_t *synth, int cat) {
    return cat < 0 ? 0 : (cat > synth->play_ncat - 1 ? synth->play_ncat - 1 : cat);
}

/* Programs in category `cat` */
static int category_patch_count(const synth_engine_t *synth, int cat) {
    return synth->cat_n[category_clamp(synth, cat)];
}

/* The slot of position `patch` in category `cat`; both clamp to what exists */
static int program_raw(const synth_engine_t *synth, int cat, int patch) {
    cat = category_clamp(synth, cat);
    if (patch < 0) patch = 0;
    if (patch > synth->cat_n[cat] - 1) patch = synth->cat_n[cat] - 1;
    return synth->cat_slot[cat][patch];
}

/* Category and position that address slot `raw` (0, 0 for a slot that is not offered) */
static void program_layout(const synth_engine_t *synth, int raw, int *cat, int *patch) {
    int c = synth->slot_cat[raw];
    *cat = c < 0 ? 0 : c;
    *patch = c < 0 ? 0 : synth->slot_pos[raw];
}

/* "A3" / "B8": side and column of a slot */
static void slot_coord(int slot, char *buf, size_t cap) {
    snprintf(buf, cap, "%c%d", slot >= 64 ? 'B' : 'A', slot % 8 + 1);
}

/* "A.11 Name" for slot idx of the active bank ("A.11" alone when the bank has no name for it) */
static int format_preset_name(const synth_engine_t *synth, int idx, char *buf, int buf_len) {
    const char *label = active_presets(synth)[idx].label;
    const char *name = label;
    if ((label[0] == 'A' || label[0] == 'B') && label[1] == '.') { /* label carries its own code: drop it */
        const char *sp = strchr(label, ' ');
        name = sp ? sp + 1 : "";
    }
    return snprintf(buf, buf_len, "%c.%d%d%s%s", idx >= 64 ? 'B' : 'A', (idx % 64) / 8 + 1, idx % 8 + 1,
                    *name ? " " : "", name);
}
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__linux__)
#include <dlfcn.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "arp.c" /* the arpeggiator: one translation unit with dsp.c (see arp.h) */

#define TINYK_TUNING_DEFAULTS { 37.46f, 10.61f, 20.0f, 19000.0f, 2.13f, 8.88f, 1.92f, 0.7f, 2.0f, 1.0f, 1.0f, 1.0f, 0.7f, 0.4f, 10.61f, 24.0f, 120.0f, 2.4f, 12.43f, 12726.0f, 0.6f, 10.36f, 0.56f, 2.0f, 18.0f, 360.0f, -24.5f, 15000.0f, 0.7f, 1.0f, 0.05f, 600.0f, 5.0f, 3.7f, 1.0f }
#ifdef TINYK_TUNING
tinyk_tuning_t tinyk_tuning = TINYK_TUNING_DEFAULTS;
#else
static const tinyk_tuning_t tinyk_tuning = TINYK_TUNING_DEFAULTS;
#endif

/* Audit builds (-DTINYK_DIAG) count non-finite values before they are sanitized, so
 * tools/audit_all_presets.py can detect NaN/Inf that the int16 output would hide. */
#ifdef TINYK_DIAG
unsigned tinyk_diag_nonfinite = 0;
#define DIAG_CHECK(x) do { if (!isfinite(x)) tinyk_diag_nonfinite++; } while (0)
#else
#define DIAG_CHECK(x) ((void)0)
#endif

/* Parameter names and short names */
typedef struct {
    const char *id;
    const char *name;
    const char *short_name;
    int page;
    float def_val;
} param_meta_t;

static const param_meta_t PARAM_METAS[NUM_PARAMS] = {
    /* Page 1: OSC */
    { "wave1",        "Wave 1",         "Wave1",  1, 0.0f },
    { "pulse_width",  "Pulse Width",    "Width",  1, 0.0f },
    { "wave2",        "Wave 2",         "Wave2",  1, 0.0f },
    { "detune",       "Detune",         "Detun",  1, 0.5f },
    { "sync_ring",    "Sync / Ring",    "SyncR",  1, 0.0f },
    { "osc_mix",      "Osc Mix",        "Mix",    1, 0.5f },
    { "sub_level",    "Sub Level",      "Sub",    1, 0.0f },
    { "portamento",   "Portamento",     "Porta",  1, 0.0f },

    /* Page 2: FILTER */
    { "cutoff",       "Cutoff",         "Cut",    2, 0.75f },
    { "resonance",    "Resonance",      "Res",    2, 0.2f },
    { "filter_type",  "Filter Type",    "Type",   2, 0.0f },
    { "keytrack",     "Key Track",      "KeyTr",  2, 0.5f },
    { "env_int",      "Env Intensity",  "EnvIn",  2, 0.5f },
    { "drive",        "Drive",          "Drive",  2, 0.2f },
    { "mod_int",      "Mod Intensity",  "ModIn",  2, 0.0f },
    { "vel_sens",     "Vel Sensitivity","VelSn",  2, 0.0f }, /* velocity -> filter EG depth: 0, as on the microKORG */

    /* Page 3: ENVs */
    { "attack1",      "Filter Attack",  "Atk1",   3, 0.01f },
    { "decay1",       "Filter Decay",   "Dcy1",   3, 0.4f },
    { "sustain1",     "Filter Sustain", "Sus1",   3, 0.5f },
    { "release1",     "Filter Release", "Rel1",   3, 0.2f },
    { "attack2",      "Amp Attack",     "Atk2",   3, 0.01f },
    { "decay2",       "Amp Decay",      "Dcy2",   3, 0.5f },
    { "sustain2",     "Amp Sustain",    "Sus2",   3, 0.8f },
    { "release2",     "Amp Release",    "Rel2",   3, 0.2f },

    /* Page 4: FX/MOD */
    { "lfo1_rate",    "LFO1 Rate",      "LFO1",   4, 0.3f },
    { "lfo2_rate",    "LFO2 Rate",      "LFO2",   4, 0.3f },
    { "chorus_mix",   "Mod FX Depth",   "Depth",  4, 0.0f },
    { "delay_time",   "Delay Time",     "Time",   4, 0.3f },
    { "delay_feedback","Delay Feedback","Fdbk",   4, 0.3f },
    { "delay_mix",    "Delay Mix",      "D.Mix",  4, 0.0f },
    { "master_vol",   "Master Vol",     "Vol",    4, 0.8f },
    { "pan",          "Pan",            "Pan",    4, 0.5f },

    /* Global / Voice Mode & Timbre controls */
    { "voice_mode",     "Mode",           "Mode",   0, 0.0f },
    { "timbre_edit",    "Layer",          "Edit",   0, 0.0f },
    { "timbre_balance", "Layer Balance",  "Bal",    0, 0.5f }
};

/* Real-time audio constraint: zero dynamic allocations in the audio paths. Each instance (one per slot) is one
 * synth_engine_t, allocated whole by v2_create_instance, so slots share nothing but the read-only banks, the
 * tuning constants and the host API. g_synth is only the instance of the legacy raw entry points (and of a NULL
 * instance), initialised on first use. */
static synth_engine_t g_synth;
static int g_synth_ready = 0;

/* What the host's instance pointer points to: the engine first (so every entry point that takes the instance as a
 * synth_engine_t * still does), then this slot's get_param scratch, so no two slots share a buffer either. */
#define TINYK_JSON_SCRATCH 16384
typedef struct {
    synth_engine_t synth;
    char json[TINYK_JSON_SCRATCH];
} tinyk_instance_t;
static const host_api_v1_t *g_host = NULL;

/* The host callbacks below sit at fixed offsets in Schwung's host_api_v1 (get_bpm at +88 is the one modules
 * have long called); a field out of place would call into someone else's memory. */
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(offsetof(host_api_v1_t, get_bpm) == 88, "host_api_v1_t layout differs from Schwung's");
_Static_assert(offsetof(host_api_v1_t, get_beat_position) == 112, "host_api_v1_t layout differs from Schwung's");
#endif

/* Schwung's move-info reader (see move_info_t in dsp.h), looked up once at init, off the audio thread. NULL on
 * hosts without it and in the native tools. */
#if defined(__linux__) && UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(offsetof(move_info_t, tempo) == 24, "move_info_t layout differs from Schwung's");
_Static_assert(offsetof(move_info_t, song_beats) == 88, "move_info_t layout differs from Schwung's");
_Static_assert(sizeof(move_info_t) == 272, "move_info_t layout differs from Schwung's");
#endif
static move_info_fn g_move_info = NULL;

void synth_init(synth_engine_t *synth);

static synth_engine_t *default_synth(void) {
    if (!g_synth_ready) {
        g_synth_ready = 1;
        synth_init(&g_synth);
    }
    return &g_synth;
}

static void find_move_info(void) {
#if defined(__linux__)
    g_move_info = (move_info_fn)dlsym(RTLD_DEFAULT, "schwung_move_info");
#endif
}

/* Session tempo, called once per block on the audio thread: the Set tempo from move-info (the tempo knob,
 * playing or stopped) unless Move follows external clock or the document is not being read; then the host's
 * get_bpm (MIDI clock -> last measured -> Set file -> settings), else lfo_tempo_bpm. Out-of-range answers are
 * ignored. *exact is 1 when the value is the Set tempo itself, which needs no jitter smoothing. */
static float host_tempo_bpm(int *exact) {
    *exact = 0;
    if (g_move_info) {
        move_info_t mi;
        if (g_move_info(&mi, sizeof mi) && mi.valid && !mi.midi_clock_sync && mi.tempo >= 20.0f && mi.tempo <= 400.0f) {
            *exact = 1;
            return mi.tempo;
        }
    }
    if (g_host && g_host->get_bpm) {
        float bpm = g_host->get_bpm();
        if (bpm >= 20.0f && bpm <= 400.0f) return bpm;
    }
    return tinyk_tuning.lfo_tempo_bpm;
}

/* Beats since transport start, or < 0 when the transport is stopped or the host cannot say */
static double host_beat_position(void) {
    return (g_host && g_host->get_beat_position) ? g_host->get_beat_position() : -1.0;
}

/* Filter key tracking: +63 = KEYTRACK_SLOPE octaves of cutoff per octave of pitch about KEYTRACK_PIVOT. The VST measured
 * 2.0 about ~62 at +63 (tools/reference/kt*), but that made the +48 presets (A11, A12) much worse, so this stays 1:1
 * about C4 until intermediate key-track values are measured. */
#define KEYTRACK_PIVOT 60.0f
#define KEYTRACK_SLOPE 1.0f

/* Tempo-sync note lengths as fractions of a whole note (microKORG order: 1/1 .. 1/32) */
static const float LFO_SYNC_NOTES[15] = {
    1.0f, 3.0f / 4.0f, 2.0f / 3.0f, 1.0f / 2.0f, 3.0f / 8.0f, 1.0f / 3.0f, 1.0f / 4.0f, 3.0f / 16.0f,
    1.0f / 6.0f, 1.0f / 8.0f, 3.0f / 32.0f, 1.0f / 12.0f, 1.0f / 16.0f, 1.0f / 24.0f, 1.0f / 32.0f
};

/* Shortest time a patch LFO may take for a full -1 -> +1 swing (slew limit on its output) */
#ifndef LFO_SLEW_S
#define LFO_SLEW_S 0.0015f
#endif
/* ...and a one-pole after it that rounds the ramp's corners, where its slope used to jump */
#ifndef LFO_SMOOTH_S
#define LFO_SMOOTH_S 0.0005f
#endif

/* Note-on de-click: a voice starting from silence fades in over this many samples (a raised cosine, squared: 4.5 ms
 * at 44.1 kHz, the VST's own onset with the attack at 0: 0.5 %, 8 %, 40 %, 90 % of full level in its first four
 * milliseconds; the 1 ms fade used before left a click 2-5 dB over the plug-in's on a plain sine), on top of its amp EG. The EG's fastest attack (1.5 ms) starts at its steepest slope, and with random start phases
 * the waveform is rarely at zero there: that corner, lifted by the brightness tilt, was a click on every fast-attack
 * note. */
#ifndef DECLICK_SAMPLES
#define DECLICK_SAMPLES 200
#endif
/* A steal or retrigger glides the voice's velocity gain over this time constant instead of stepping it */
#define VEL_GLIDE_S 0.0005f
/* All notes off (CC 120 / 123, voice mode change, state restore) fades the voices out over this (to -40 dB) */
#define KILL_RELEASE_S 0.003f

/* Delay time base when the delay is tempo-synced, the other way round (1/32 .. 1/1, as the Time knob turns):
 * the factory programs' most used values 7, 5, 8 are then 3/16, 1/8, 1/4 (the LFO order would make 5 and 8
 * a 1/3 and a 1/6). */
static const float DELAY_SYNC_NOTES[15] = {
    1.0f / 32.0f, 1.0f / 24.0f, 1.0f / 16.0f, 1.0f / 12.0f, 3.0f / 32.0f, 1.0f / 8.0f, 1.0f / 6.0f, 3.0f / 16.0f,
    1.0f / 4.0f, 1.0f / 3.0f, 3.0f / 8.0f, 1.0f / 2.0f, 2.0f / 3.0f, 3.0f / 4.0f, 1.0f
};

/* One patch LFO sample in -1..1. LFO1 waves: saw, square, triangle, S&H; LFO2: saw, square, sine, S&H. */
static inline float patch_lfo_value(const lfo_t *l, int which, int wave) {
    switch (wave) {
        case 0: return 1.0f - 2.0f * l->phase; /* saw falls: +LFO -> amp gates decaying hits (A.21 AutoHouse) */
        case 1: return (l->phase < 0.5f) ? 1.0f : (which == 1 ? 0.0f : -1.0f); /* LFO2's square is 0..+1 (measured) */
        case 2: return which == 0 ? 2.0f * fabsf(2.0f * l->phase - 1.0f) - 1.0f
                                  : sinf(2.0f * (float)M_PI * l->phase);
        default: return l->sh_value;
    }
}

/* Osc 1 Sine cross modulation used to be a fitted pitch law for one synced case (A.21's kick: the sine's pitch
 * against depth). It is now what the VST does everywhere, frequency modulation by Osc 2 (see the oscillator
 * loop, XMOD_MAX_HZ); with Osc 2 synced, the modulator is locked to the sine and its net effect is that pitch
 * shift. The A.21 takes have not been re-checked against it (they are not in this checkout);
 * xmod_semitones / xmod_offset_semitones in the tuning table are unused. */

/* White-noise generator state (xorshift32); reset by synth_init so renders are repeatable */
#define NOISE_SEED 0x1234ABCDu

/* Fast positive fmod for phase wrapping [0, 1) */
static inline float fmod_pos(float v) {
    v -= (int)v;
    if (v < 0.0f) v += 1.0f;
    return v;
}

/* PolyBLEP residual function for antialiasing */
static inline float poly_blep(float t, float dt) {
    if (dt <= 1e-9f) return 0.0f;
    if (t < dt) {
        float x = t / dt;
        return x + x - x * x - 1.0f;
    } else if (t > 1.0f - dt) {
        float x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

/* The plug-in's oscillators lose their top as the note rises: measured on its saw and square, 26 notes from C4 to
 * D8, each harmonic against the same frequency on a low note (tools/measure/rolloff.py). Below about 700 Hz
 * nothing is missing; above, a harmonic at f on a note at f0 is down by about (f f0 / 7.2e6 Hz^2)^1.7 dB (C6:
 * 1.5 dB at 10 kHz; C7: 2 dB at 6 kHz, 8 at 10; C8: 5 dB on the fundamental), which is also why its top octaves
 * do not alias. One bilinear one-pole low-pass per oscillator with g = OSC_ROLLOFF_C / dt^2 follows that to
 * 1.5 dB rms; dt is the oscillator's own phase increment. While both oscillators are under OSC_ROLLOFF_MIN_DT
 * (660 Hz: the filter would take 0.1 dB at 10 kHz and 0.4 at 15) nothing is filtered, and a voice crossing the
 * line starts its filters on the signal as it is (voice_t.osc_lp_on), so the crossing is seamless. */
#define OSC_ROLLOFF_C 0.00127f
#define OSC_ROLLOFF_MIN_DT 0.015f
static inline float osc_rolloff(float *state, float x, float dt) {
    float v = (x - *state) * (OSC_ROLLOFF_C / (OSC_ROLLOFF_C + dt * dt)); /* g / (1 + g) */
    float y = v + *state;
    *state = y + v;
    return y;
}

/* MIDI Note to Frequency */
static inline float note_to_freq(float note) {
    return 440.0f * powf(2.0f, (note - 69.0f) * (1.0f / 12.0f));
}

static inline float clamp01f(float v) {
    return fmaxf(0.0f, fminf(1.0f, v));
}

/* Envelope time ranges. Knob 0..1 maps exponentially (min * (max/min)^x), so the whole
 * range is usable: instant stabs at 0, slow swells at 1. The minimums also keep plucks and
 * note-offs from collapsing into clicks. */
#define EG_ATTACK_MIN_S 0.0015f
#define EG_ATTACK_MAX_S 12.0f
#define EG_DECAY_MIN_S  0.015f   /* decay and release */
#define EG_DECAY_MAX_S  20.0f

/* ---- Envelopes: measured on the VST (2026-10, a sine through the open high-pass) -------------------------------------
 * Every segment is a finite ramp, not an exponential, and all three stages share ONE time table:
 *   attack   E = 1 - (1 - x)^2            x = time / T(attack knob)
 *   decay    E = S + (1 - S) (1 - x)^2    x = time / T(decay knob),   S = sustain knob / 127
 *   release  E = L (1 - x)^2              x = time / T(release knob), L = the level it left from
 * and the AMP follows E^2 (sustain 64 holds 0.254 of full level, 96 holds 0.571; a decay is 6 / 12 / 20 / 40 /
 * 60 dB down at 0.166 / 0.296 / 0.438 / 0.684 / 0.822 of T and silent at T; an attack is 90 % up at 0.7735 T
 * and 99 % at 0.929 T: all ten measured points of the attack curve fit to 0.01). The old exponential stages
 * with a 1.35 overshoot were 2-15 x too fast on the attack below knob 80 and 2-5 x off on decay and release.
 * EG_TIME_S is T every 4 knob steps, from the attack's time to 90 % (less the plug-in's 4.5 ms of note latency,
 * / 0.7735); decay and release give the same T from knob 8 up: instant at 0, 0.79 s at 64, 1.6 s at 100, then
 * steeply up to 30 s at 127. Below knob 8 a decay or release is slower than an attack, a floor against clicks:
 * 8 ms at 0, then 15.5 + 2.54 x knob ms (18 ms at 1, 26.6 at 4, 31 at 6), measured through the release.
 * The filter EG and the patch sources take E itself; that part is assumed, not measured. */
#define EG_TIME_POINTS 33
static const float EG_TIME_S[EG_TIME_POINTS] = {
    0.00020f, 0.01470f, 0.03583f, 0.05561f, 0.07565f, 0.09543f, 0.12710f, 0.16304f, 0.19885f, 0.23867f, 0.27849f,
    0.35400f, 0.44178f, 0.52917f, 0.61670f, 0.70435f, 0.79174f, 0.87953f, 0.96705f, 1.05509f, 1.14262f, 1.22962f,
    1.31818f, 1.40467f, 1.49207f, 1.58024f, 1.98942f, 3.18968f, 4.87991f, 6.91935f, 10.37920f, 19.58920f, 30.23570f
};

/* A stage's knob (0..1) as its step per sample: 1 / (T x fs). min_sec / max_sec are kept for the callers'
 * tuning scales (attack_scale ...): only their ratio to the stock range is used. */
static inline float eg_step(float knob01, float scale, float fs, int attack) {
    float k = clamp01f(knob01) * 127.0f, pos = k / 4.0f, t;
    int i = (int)pos;
    if (i >= EG_TIME_POINTS - 2) {   /* 124 .. 127 is a three-step segment */
        t = EG_TIME_S[EG_TIME_POINTS - 2] + (EG_TIME_S[EG_TIME_POINTS - 1] - EG_TIME_S[EG_TIME_POINTS - 2]) * fminf(1.0f, (k - 124.0f) / 3.0f);
    } else {
        t = EG_TIME_S[i] + (EG_TIME_S[i + 1] - EG_TIME_S[i]) * (pos - (float)i);
    }
    if (!attack) {
        float floor_s = k < 1.0f ? 0.008f + 0.010f * k : 0.0155f + 0.00254f * k;
        if (t < floor_s) t = floor_s;
    }
    return 1.0f / fmaxf(1.0f, t * scale * fs);
}

static inline float time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    (void)max_sec;
    return eg_step(time_val_01, min_sec / EG_DECAY_MIN_S, fs, 0);
}

static inline float attack_time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    (void)max_sec;
    return eg_step(time_val_01, min_sec / EG_ATTACK_MIN_S, fs, 1);
}

static inline void adsr_gate_on(adsr_t *env, float attack_step) {
    /* a voice still sounding goes on up the attack curve from where its level is */
    float e = fmaxf(0.0f, fminf(1.0f, env->value));
    env->stage = ENV_ATTACK;
    env->target = 1.0f;
    env->rate = attack_step;
    env->pos = 1.0f - sqrtf(1.0f - e);
}

static inline void adsr_gate_off(adsr_t *env) {
    env->stage = ENV_RELEASE;
    env->target = 0.0f;
    env->from = env->value;
    env->pos = 0.0f;
}

/* One sample of E (0..1); decay_step / release_step as from eg_step */
static inline float adsr_process(adsr_t *env, float decay_step, float sustain_level, float release_step) {
    float r;
    switch (env->stage) {
        case ENV_IDLE:
            env->value = 0.0f;
            break;
        case ENV_ATTACK:
            env->pos += env->rate;
            if (env->pos >= 1.0f) {
                env->value = 1.0f;
                env->stage = ENV_DECAY;
                env->target = sustain_level;
                env->pos = 0.0f;
            } else {
                r = 1.0f - env->pos;
                env->value = 1.0f - r * r;
            }
            break;
        case ENV_DECAY:
            env->pos += decay_step;
            if (env->pos >= 1.0f) {
                env->value = sustain_level;
                env->stage = ENV_SUSTAIN;
            } else {
                r = 1.0f - env->pos;
                env->value = sustain_level + (1.0f - sustain_level) * r * r;
            }
            break;
        case ENV_SUSTAIN:
            env->value = sustain_level;
            break;
        case ENV_RELEASE:
            env->pos += release_step;
            if (env->pos >= 1.0f) {
                env->value = 0.0f;
                env->stage = ENV_IDLE;
            } else {
                r = 1.0f - env->pos;
                env->value = env->from * r * r;
            }
            break;
    }

    if (!isfinite(env->value)) env->value = 0.0f;
    return fmaxf(0.0f, fminf(1.0f, env->value));
}

/* ---- Filter: measured on the VST (2026-10; tools/vst_ab.py filter) ------------------------------------------------
 * Method: a 55 Hz saw's harmonics through every type over cutoff x resonance, each curve relative to the open
 * high-pass, and ring-down times for the highest resonances.
 * The plug-in's filter is a Chamberlin state-variable filter run once per sample, and that exact recurrence
 *     lp += F * bp;   hp = x - lp - q * bp;   bp += F * hp
 * reproduces its 12HPF and 12BPF curves to 0.06-0.13 dB at every setting tried (an analogue-matched filter,
 * which the engine had, is 2 dB off near the top of the range and far more above it: the plug-in passes much
 * more through a nearly-open high-pass). Its laws:
 *   F  = Fc(cutoff) x FLT_F_RATIO[resonance]. Fc is the knob's coefficient at resonance 0: 2 pi fc / fs with fc
 *        doubling every 12.03 knob steps (10.56 octaves over the knob, 32.7 Hz at 0) up to knob 70, then
 *        levelling off to 0.915 at 110 (knob_fc). Resonance multiplies it: x 1.72 at 60, x 2.0 at
 *        127, AFTER the bend (F reaches 1.9), which is how resonance raises the corner by an octave.
 *   q  = FLT_DAMP[resonance]: 1.47 at 0, falling straight to 0.98 at 4, then about 1.05 (1 - res / 127)^2:
 *        0.30 at 60, 0.05 at 100, 0.004 at 120, 0.00026 at 126 and 127 (a ring of seconds; flt_damp above 100).
 *   gain = FLT_GAIN_DB[resonance], the same for every type: within 2 dB of flat to 84, then falling as fast as
 *        the resonance rises, so the peak stops near +17 dB while everything else sinks (-40 dB at 127).
 *   12HPF = gain x hp;   12BPF = gain x 1.32 x bp;
 *   12LPF = gain x 2.94 x lp through the low-pass types' extra stage (FLT_LP_A0 below: a pole near 2.3-4.6 kHz
 *           wherever the cutoff is, so an open 12LPF is 10 dB down at 10 kHz against an open 12HPF);
 *   24LPF = the first core's lp x 2.19 through that stage with its gain held at 1, then a second identical
 *           core's lp x FLT_STAGE2_DB[resonance] (the table's level is set by a sine in the passband).
 * The filter is linear (the same curve at every input level), so the old input soft clip is gone. */
#define SVF_STATE_LIMIT 1.0e6f /* a safety net only: at q 0.00026 the band state legitimately reaches thousands */
#define FLT_POINTS 26
static const float FLT_RES[FLT_POINTS] = { 0, 4, 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66, 72, 78, 84, 90, 96, 100, 104, 108, 112, 116, 120, 124, 126 };   /* 127 is 126: the ring-down is the same to 3 figures */
static const float FLT_F_RATIO[FLT_POINTS] = { 1.0000f, 1.1262f, 1.1526f, 1.2308f, 1.3053f, 1.3762f, 1.4423f, 1.5060f, 1.5649f, 1.6214f, 1.6731f,
    1.7212f, 1.7656f, 1.8065f, 1.8438f, 1.8774f, 1.9075f, 1.9327f, 1.9483f, 1.9615f, 1.9736f, 1.9832f, 1.9916f, 1.9988f, 2.0024f, 2.0048f };
static const float FLT_DAMP[FLT_POINTS] = { 1.4741f, 0.9828f, 0.9512f, 0.8599f, 0.7733f, 0.6912f, 0.6138f, 0.5409f, 0.4727f, 0.4090f, 0.3500f,
    0.2955f, 0.2456f, 0.2004f, 0.1597f, 0.1237f, 0.0922f, 0.0653f, 0.0500f, 0.0368f, 0.0257f, 0.0164f, 0.0088f, 0.00413f, 0.00103f, 0.000255f };
static const float FLT_GAIN_DB[FLT_POINTS] = { -0.08f, -0.06f, -0.07f, -0.17f, -0.37f, -0.64f, -0.96f, -1.28f, -1.56f, -1.75f, -1.81f,
    -1.75f, -1.62f, -1.50f, -1.52f, -1.83f, -3.10f, -5.92f, -8.13f, -10.69f, -13.72f, -17.38f, -21.99f, -28.05f, -36.26f, -40.41f };
static const float FLT_STAGE2_DB[FLT_POINTS] = { -5.50f, -5.56f, -5.54f, -5.58f, -5.62f, -5.68f, -5.85f, -6.22f, -6.58f, -7.22f, -8.26f,
    -9.60f, -11.25f, -13.40f, -15.75f, -18.96f, -22.55f, -25.24f, -27.10f, -29.60f, -32.70f, -36.30f, -40.80f, -46.80f, -55.10f, -62.00f };
#define FLT_BP_GAIN 1.3213f       /* +2.42 dB */
#define FLT_LP_GAIN 2.1878f       /* +6.8 dB: the 24LPF's first stage */
#define FLT_LP12_GAIN 2.938f      /* +9.4 dB: the 12LPF's core output, which is also what the distortion sees */
/* The low-pass types' extra stage: an integrator with feedback, y += k (x - a y), with a = FLT_LP_A0 +
 * FLT_LP_AF x F (F = the core's coefficient, resonance and modulation included). So its low-frequency gain is
 * 1 / a, falling as the filter opens (-1.4 dB at knob 0, -2.9 at 80, -4.7 from 110 up, another 2.3 dB at high
 * resonance), while above its corner (a x 2.0 kHz) the level stays put. Measured with a sine in the passband,
 * clean against distorted (the distortion taps the core before this stage), over 8 cutoffs x 6 resonances and
 * with the corner moved by the EG and by key track: 1 / a holds to 0.1 dB, 0.25 at knob 110. Its curve was
 * checked with sines to 8.4 kHz: the 12LPF follows the step-invariant pole (0.5 dB; plain Euler is 2 dB bright,
 * the trapezoid rule 1.7 dB dark at 8 kHz), the 24LPF the Euler step with the gain held at 1. */
#define FLT_KICK 0.115f           /* the note-on kick to the core's band state, per unit of Q (a saw's peak = 1) */
#define FLT_KICK_HZ 477.0f
#define FLT_LP_A0 1.173f
#define FLT_LP_AF 0.595f
#define FLT_LP_STAGE_HZ 1990.0f
/* With distortion on, the clipper is fed more than the clean output for every type but the high-pass (a sine
 * small enough to stay linear, the same grid: exact to 0.01 dB): the band-pass x 1.75, the 12LPF's core alone
 * (before its extra stage), the 24LPF x 3.285 / a. */
#define DIST_BP_GAIN 1.750f
#define DIST_LP24_GAIN 3.285f
#define FLT_BASE_HZ 32.67f        /* the cutoff knob at 0 */
#define FLT_KNOB_OCTAVES 10.56f   /* ... and its whole travel */
#define FLT_EG_OCTAVES 7.98f      /* filter EG int +-63 = 96 knob steps (1.52 per unit, linear) */
#define FLT_PATCH_OCTAVES 7.83f   /* an EG / LFO -> cutoff at +-63 and full source (94 knob steps) */
#define FLT_F_BEND 0.915f
#define FLT_F_MAX 1.95f

static float flt_table(const float *xs, const float *ys, int n, float x) {
    if (x <= xs[0]) return ys[0];
    for (int i = 1; i < n; i++) {
        if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    }
    return ys[n - 1];
}

/* The Noise oscillator's own low-pass, fitted on the plug-in at 15 settings of Control 2 (a Chamberlin core fits
 * each within 0.45 dB): frequency ratio and damping are the main filter's at resonance 4.5 + 122.5 c2 / 127 (so
 * it never has the main filter's heavily damped first steps: q 0.96 and ratio 1.12 at 0), with a level of its own. */
#define NOISE_POINTS 15
static const float NOISE_RES[NOISE_POINTS] = { 0, 8, 16, 24, 32, 40, 48, 64, 80, 96, 104, 112, 120, 124, 127 };
static const float NOISE_GAIN_DB[NOISE_POINTS] = { 0.00f, -0.06f, 0.01f, 0.05f, 0.20f, 0.27f, 0.25f, -0.12f, -2.05f, -4.33f, -7.18f,
    -9.95f, -17.33f, -26.0f, -36.5f };

/* The damping above resonance 100, where the table's straight lines between points are too coarse for a ring that
 * lasts seconds: the plug-in's ring-down gives q = (0.00803 (128 - res))^2 at every cutoff, res stopping at 126
 * (measured 100 .. 127: 0.0508, 0.0259, 0.0166, 0.0093, 0.00414, 0.00232, 0.00103, 0.000255, 0.000255). */
static float flt_damp(float r127) {
    if (r127 < 100.0f) return flt_table(FLT_RES, FLT_DAMP, FLT_POINTS, r127);
    float u = 0.00803f * (128.0f - fminf(r127, 126.0f));
    return u * u;
}

/* The cutoff knob's coefficient at resonance 0. Measured on the VST from the band-pass peak at resonance 110, 13
 * knob positions from 50 up (sharp peaks, read to 0.1 %): the exponential law holds to knob 70, from 80 to 105 the
 * coefficient rises in a straight line (0.019 a step), and from 110 it stays at 0.914. The smooth bend used
 * before, Fn / sqrt(1 + (Fn / 0.915)^2), was 2 % high at 90 and 5-6 % low at 105-110 (a resonant peak at
 * 15.8 kHz for the plug-in's 18.0). The table is in 44.1 kHz coefficients (the rate of every measurement here
 * and of the Move), scaled to this rate. */
static float knob_fc(float knob127, float fs) {
    static const float K[8] = { 70, 80, 85, 90, 95, 100, 105, 110 };
    static const float FC[8] = { 0.2606f, 0.4192f, 0.5141f, 0.6054f, 0.7006f, 0.7959f, 0.8804f, 0.9140f };
    if (knob127 < 70.0f) return (2.0f * (float)M_PI * FLT_BASE_HZ / fs) * exp2f(knob127 * (FLT_KNOB_OCTAVES / 127.0f));
    return fminf(0.99f, flt_table(K, FC, 8, knob127) * (44100.0f / fs));
}

/* One sample of the Chamberlin state-variable filter (s1 = band, s2 = low) */
static inline void svf_core(svf_t *svf, float in, float F, float q, float *hp, float *bp, float *lp) {
    if (!isfinite(svf->s1)) svf->s1 = 0.0;
    if (!isfinite(svf->s2)) svf->s2 = 0.0;
    if (!isfinite(in)) in = 0.0f;
    double low = svf->s2 + (double)F * svf->s1;
    double high = (double)in - low - (double)q * svf->s1;
    double band = svf->s1 + (double)F * high;
    if (fabs(low) < 1e-30) low = 0.0;
    if (fabs(band) < 1e-30) band = 0.0;
    svf->s2 = fmax(-(double)SVF_STATE_LIMIT, fmin((double)SVF_STATE_LIMIT, low));
    svf->s1 = fmax(-(double)SVF_STATE_LIMIT, fmin((double)SVF_STATE_LIMIT, band));
    *hp = (float)high; *bp = (float)svf->s1; *lp = (float)svf->s2;
    DIAG_CHECK(*hp);
    DIAG_CHECK(*bp);
    DIAG_CHECK(*lp);
}



/* Exponential cutoff map: 0..1 -> ~15 Hz .. ~19.6 kHz (5.1 octaves per half-turn) */

/* Soft-clipping saturation for the voice mix: unity gain for small signals, bounded at +/-1 */
#ifdef TINYK_LEVEL_PROBE
/* tools only (tools/probe_levels.c): the largest level into the voice-mix clip and into the output limiter */
float tinyk_probe_mix, tinyk_probe_out;
#endif
static inline float soft_clip(float x) {
    return tanhf(x);
}

/* Build the single-cycle table of the Vox (formant) oscillator: an additive approximation, not the
 * hardware's sampled wave. */
static void build_wavetable(float *table) {
    enum { MAX_HARMONICS = 16 };
    float amp[MAX_HARMONICS + 1];

    for (int h = 1; h <= MAX_HARMONICS; h++) {
        /* Two formant peaks over a saw-like rolloff */
        float fh = (float)h;
        float f1 = (fh - 3.0f) / 1.2f;
        float f2 = (fh - 9.0f) / 1.5f;
        amp[h] = (1.0f / fh) * (0.3f + 4.0f * expf(-0.5f * f1 * f1) + 2.5f * expf(-0.5f * f2 * f2));
    }

    float peak_abs = 1e-6f;
    for (int i = 0; i < WAVETABLE_SIZE; i++) {
        float ph = 2.0f * (float)M_PI * (float)i / (float)WAVETABLE_SIZE;
        float acc = 0.0f;
        for (int h = 1; h <= MAX_HARMONICS; h++) acc += amp[h] * sinf(ph * (float)h);
        table[i] = acc;
        if (fabsf(acc) > peak_abs) peak_abs = fabsf(acc);
    }
    for (int i = 0; i < WAVETABLE_SIZE; i++) table[i] /= peak_abs;
}

/* DWGS: the 64 digital waveforms (dwgs_waves.h, Fourier series traced from the hardware's output) as
 * band-limited tables. Each wave has DWGS_LEVELS tables, level l keeping the note's harmonics up to
 * DWGS_HARMONICS >> l at dwgs_level_size(l) samples per period of the note; the oscillator reads the
 * richest level whose top harmonic stays under Nyquist, with a 4-point Hermite interpolation (linear left
 * images about 40 dB under the top harmonics, heard as aliasing on bright waves in the top octaves). A
 * table spans the wave's `periods` (1 for most; a few waves repeat only every 2, 3 or 5 periods of the note)
 * and wraps into DWGS_GUARD samples, one before and two after. Built once, at the first synth_init, and
 * read-only afterwards: shared by every instance like the decoded banks. */
#define DWGS_LEVELS 7
#define DWGS_SAMPLES_PER_PERIOD 2048 /* the sum of dwgs_level_size over the levels */
#define DWGS_GUARD 3

static inline int dwgs_level_size(int level) {
    int n = 1024 >> level;
    return n < 32 ? 32 : n;
}

static float g_dwgs_pool[DWGS_TOTAL_PERIODS * DWGS_SAMPLES_PER_PERIOD + DWGS_WAVE_COUNT * DWGS_LEVELS * DWGS_GUARD];
static const float *g_dwgs_table[DWGS_WAVE_COUNT][DWGS_LEVELS];
static int g_dwgs_ready = 0;

static void dwgs_build_tables(void) {
    if (g_dwgs_ready) return;
    float *out = g_dwgs_pool + 1;
    for (int w = 0; w < DWGS_WAVE_COUNT; w++) {
        const short *coef = DWGS_WAVES[w].coef;
        int periods = DWGS_WAVES[w].periods;
        for (int level = 0; level < DWGS_LEVELS; level++) {
            int n = dwgs_level_size(level) * periods;
            int top = (DWGS_HARMONICS >> level) * periods;
            if (top > DWGS_WAVES[w].count) top = DWGS_WAVES[w].count;
            for (int i = 0; i < n; i++) out[i] = 0.0f;
            for (int h = 1; h <= top; h++) {
                double a = (double)coef[2 * h - 2] * (double)DWGS_COEF_SCALE;
                double b = (double)coef[2 * h - 1] * (double)DWGS_COEF_SCALE;
                if (a == 0.0 && b == 0.0) continue;
                /* a cos(h x) + b sin(h x), stepped by rotating (cos, sin) through one sample */
                double step = 2.0 * M_PI * (double)h / (double)n;
                double rc = cos(step), rs = sin(step), c = 1.0, sn = 0.0;
                for (int i = 0; i < n; i++) {
                    out[i] += (float)(a * c + b * sn);
                    double nc = c * rc - sn * rs;
                    sn = sn * rc + c * rs;
                    c = nc;
                }
            }
            out[-1] = out[n - 1];
            out[n] = out[0];
            out[n + 1] = out[1];
            g_dwgs_table[w][level] = out;
            out += n + DWGS_GUARD;
        }
    }
    g_dwgs_ready = 1;
}

/* DWGS waveform number 0..63 of a timbre */
static inline int dwgs_index(const timbre_extra_t *extra) {
    int i = (int)(extra->dwgs * (float)(DWGS_WAVE_COUNT - 1) + 0.5f);
    return i < 0 ? 0 : (i >= DWGS_WAVE_COUNT ? DWGS_WAVE_COUNT - 1 : i);
}

/* Copy one timbre's patch data into a parameter array and its extras */
static void apply_timbre(float *dst, timbre_extra_t *extra, const struct TimbreParams *t) {
    dst[PARAM_WAVE1] = clamp01f(t->wave1);
    dst[PARAM_PULSE_WIDTH] = clamp01f(t->pulse_width);
    dst[PARAM_WAVE2] = clamp01f(t->wave2);
    dst[PARAM_DETUNE] = clamp01f(t->detune);
    dst[PARAM_SYNC_RING] = clamp01f(t->sync_ring);
    dst[PARAM_OSC_MIX] = clamp01f(t->osc_mix);
    dst[PARAM_SUB_LEVEL] = clamp01f(t->sub_level);
    dst[PARAM_PORTAMENTO] = clamp01f(t->portamento);
    dst[PARAM_CUTOFF] = clamp01f(t->cutoff);
    dst[PARAM_RESONANCE] = clamp01f(t->resonance);
    dst[PARAM_FILTER_TYPE] = clamp01f(t->filter_type);
    dst[PARAM_KEYTRACK] = clamp01f(t->keytrack);
    dst[PARAM_ENV_INT] = clamp01f(t->env_int);
    dst[PARAM_DRIVE] = clamp01f(t->drive);
    dst[PARAM_ATTACK1] = clamp01f(t->attack1);
    dst[PARAM_DECAY1] = clamp01f(t->decay1);
    dst[PARAM_SUSTAIN1] = clamp01f(t->sustain1);
    dst[PARAM_RELEASE1] = clamp01f(t->release1);
    dst[PARAM_ATTACK2] = clamp01f(t->attack2);
    dst[PARAM_DECAY2] = clamp01f(t->decay2);
    dst[PARAM_SUSTAIN2] = clamp01f(t->sustain2);
    dst[PARAM_RELEASE2] = clamp01f(t->release2);

    extra->transpose_semi = (clamp01f(t->transpose) - 0.5f) * 48.0f;
    extra->noise_level = clamp01f(t->noise_level);
    extra->level = clamp01f(t->level);
    if (t->amp_level > 0.0f) { /* the program records its three level knobs */
        extra->lvl_osc1 = clamp01f((t->osc1_level * 128.0f - 1.0f) / 127.0f);
        extra->lvl_osc2 = clamp01f((t->osc2_level * 128.0f - 1.0f) / 127.0f);
        extra->lvl_amp = clamp01f((t->amp_level * 128.0f - 1.0f) / 127.0f);
    } else {                   /* the built-in bank: only the mix and their product, so the louder one is taken as full */
        float mix = clamp01f(t->osc_mix);
        extra->lvl_osc1 = fminf(1.0f, 2.0f * (1.0f - mix));
        extra->lvl_osc2 = fminf(1.0f, 2.0f * mix);
        extra->lvl_amp = extra->level;
    }
    extra->mix_seen = clamp01f(t->osc_mix);
    extra->level_seen = extra->level;
    extra->dwgs = clamp01f(t->dwgs);
    extra->osc1_ctrl[0] = clamp01f(t->osc1_ctrl1);
    extra->osc1_ctrl[1] = clamp01f(t->osc1_ctrl2);
    extra->assign = (int)lroundf(clamp01f(t->assign) * 2.0f);
    extra->unison_cents = clamp01f(t->unison_detune) * 127.0f;
    extra->pan = (clamp01f(t->pan) - 0.5f) * 2.0f;
    extra->multi_trigger = t->trigger_multi >= 0.5f;
    /* stored as 0 when a bank does not record them (the built-in one): EG reset then plays as on, the bend
     * range as 2 semitones and the vibrato as off */
    extra->eg_reset[0] = !(t->eg1_reset > 0.25f && t->eg1_reset < 0.75f);
    extra->eg_reset[1] = !(t->eg2_reset > 0.25f && t->eg2_reset < 0.75f);
    extra->bend_semi = t->bend_range > 0.0f ? fmaxf(-12.0f, fminf(12.0f, roundf(t->bend_range * 25.0f) - 13.0f)) : 2.0f;
    extra->vibrato_int = t->vibrato_int > 0.0f ? fmaxf(-63.0f, fminf(63.0f, roundf(t->vibrato_int * 127.0f) - 64.0f)) : 0.0f;

    const float lfo_wave[2] = { t->lfo1_wave, t->lfo2_wave };
    const float lfo_keysync[2] = { t->lfo1_keysync, t->lfo2_keysync };
    const float lfo_rate[2] = { t->lfo1_rate, t->lfo2_rate };
    const float lfo_sync[2] = { t->lfo1_sync_note, t->lfo2_sync_note };
    for (int i = 0; i < 2; i++) {
        extra->lfo_wave[i] = (int)(clamp01f(lfo_wave[i]) * 3.0f + 0.5f);
        extra->lfo_keysync[i] = (int)(clamp01f(lfo_keysync[i]) * 2.0f + 0.5f);
        extra->lfo_rate[i] = clamp01f(lfo_rate[i]);
        extra->lfo_sync_note[i] = (int)(clamp01f(lfo_sync[i]) * 15.0f + 0.5f) - 1; /* 0 -> -1 = free running */
    }
    const float p_src[4] = { t->patch1_src, t->patch2_src, t->patch3_src, t->patch4_src };
    const float p_dst[4] = { t->patch1_dst, t->patch2_dst, t->patch3_dst, t->patch4_dst };
    const float p_int[4] = { t->patch1_int, t->patch2_int, t->patch3_int, t->patch4_int };
    for (int i = 0; i < 4; i++) {
        extra->patch_src[i] = (int)(clamp01f(p_src[i]) * 7.0f + 0.5f);
        extra->patch_dst[i] = (int)(clamp01f(p_dst[i]) * 7.0f + 0.5f);
        extra->patch_int[i] = (clamp01f(p_int[i]) - 0.5f) * 2.0f;
    }
}

static void arp_emit_voice(void *ctx, int on, uint8_t note, uint8_t vel);
static void set_arp_on(synth_engine_t *synth, int on);
static void set_arp_step(synth_engine_t *synth, int step, int play);
static int arp_param_index(const char *key);
static int arp_param_get(const synth_engine_t *synth, int i);
static void arp_param_set(synth_engine_t *synth, int i, int v);

/* A program change flushes the engine like CC 120 / 123 and clears what could ring into the new sound.
 * Held keys, key stacks and the arpeggiator (steps, held chord, latch queue) are dropped by synth_all_notes_off;
 * the sounding voices fade over KILL_RELEASE_S and each wipes its filter state and envelopes when it ends (zeroing
 * them under a sounding voice would be a step); idle voices are wiped here. The effect memories (Mod FX line,
 * phaser, delay line and its feedback filter, DC blockers) are cleared at once when nothing is audible, else
 * the output ducks over FLUSH_FADE_S, they are cleared at silence and the output comes back (no click). */
#define FLUSH_FADE_S 0.006f
#define FLUSH_AUDIBLE 0.0001f
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define NOISE_MIX_GAIN 5.48f   /* the mixer's noise at 127: its level below 3 kHz against a full-level saw, band by band */
#define NOISE_LP_HZ 5500.0f
#define SAW1_FUND_HZ 18.4f     /* Osc 1's saw (not Osc 2's): extra fundamental = this / f of a full-level sine */
#define NOISE_OSC_GAIN 3.005f /* the Noise oscillator, open: 5.2 dB under the mixer's noise at 127 */
#define DIST_GAIN 2.69f
#define DIST_CLIP 1.55f /* clipped right down the fundamental is 1.97 x a full-level sine: 4 / pi x 1.55 */
/* The voice mix runs at the plug-in's own level, where it is LINEAR: at 0.35 one held key reached the mix clip
 * (a tanh) at a median 1.95 and eight keys at 4.8 over the 128 built-in programs (tools/probe_levels.c): every
 * program was audibly saturated on the Move, the brightness tilt's edges most of all. At 0.03 one key peaks at a
 * median 0.17 and eight keys reach 1.0 only in the loudest tenth of the programs. TINYK_OUTPUT_MAKEUP brings the
 * level back up after the effects, in front of the output limiter, which is linear to 0.65. */
#ifndef TINYK_MIX_GAIN
#define TINYK_MIX_GAIN 0.03f
#endif
#ifndef TINYK_OUTPUT_MAKEUP
#define TINYK_OUTPUT_MAKEUP 2.8f
#endif

/* The Osc 1, Osc 2 and Noise level knobs (0..1 = 0..127) as amplitude, measured on the VST: 0.082 at 16, 0.177
 * at 32, 0.404 at 64, 0.682 at 96 (straight lines between). The Amp level knob is a plain square (0.254 at 64). */
static float level_curve(float knob01) {
    static const float K[6] = { 0.0f, 16.0f, 32.0f, 64.0f, 96.0f, 127.0f }, A[6] = { 0.0f, 0.082f, 0.177f, 0.404f, 0.682f, 1.0f };
    float k = clamp01f(knob01) * 127.0f;
    for (int i = 1; i < 6; i++) {
        if (k <= K[i]) return A[i - 1] + (A[i] - A[i - 1]) * (k - K[i - 1]) / (K[i] - K[i - 1]);
    }
    return 1.0f;
}

/* The waves' own levels against a full-level sine (= 1.0), from their fundamentals on the VST: a square runs
 * +-0.667 and a triangle +-1.333 (the plug-in's waves are matched in loudness, not in peak); a saw +-1. */
static inline float osc_wave_gain(int square, int triangle) {
    return square ? 0.667f : (triangle ? 1.333f : 1.0f);
}
#define VOX_MAX_PULSES 24      /* Vox: pulses summed per sample */
#define XMOD_MAX_HZ 24000.0f   /* Osc 1 Sine cross modulation: the frequency deviation at full depth */
#define PAN_SLEW_PER_S 1000.0f /* pan position, -1 .. +1, per second: left to right in 2 ms */

/* Delay, measured on the VST with noise bursts (tools/vst_ab.py delay).
 * Time (knob 0..127, not tempo-synced) is a fraction of a 65536-sample line at 48 kHz, 1.3653 s, in five straight
 * segments: 0.01 at 0, 0.16 at 64, 0.40 at 110, 0.60 at 120, 0.80 at 125, then 1.0 at 126 and 1.2 at 127
 * (13.7 ms ... 218 ms at the centre ... 1.64 s). Every one of the 128 values was measured.
 * Depth (0..127) is one gain for both the first repeat and each further one (repeat n = dry * gain^n): straight
 * from 0 to 0.300 at 64, then straight to 0.990 at 127, and scaled down as the delay gets longer, by
 * 1 - (time as a fraction of the 1.3653 s line) / 4: the same factor at every depth, measured from 46 ms
 * to 1.64 s (0.982 of the dry level at depth 127 and 46 ms, 0.693 at 1.64 s). */
static float delay_time_s(float knob01) {
    float k = clamp01f(knob01) * 127.0f, frac;
    if (k <= 64.0f) frac = 0.01f + 0.15f * k / 64.0f;
    else if (k <= 110.0f) frac = 0.16f + 0.24f * (k - 64.0f) / 46.0f;
    else if (k <= 120.0f) frac = 0.40f + 0.02f * (k - 110.0f);
    else if (k <= 125.0f) frac = 0.60f + 0.04f * (k - 120.0f);
    else frac = 1.0f + 0.2f * (k - 126.0f);
    return frac * (65536.0f / 48000.0f);
}

static float delay_gain(float depth01, float time_s) {
    float d = clamp01f(depth01) * 127.0f;
    float g = d <= 64.0f ? d * (0.3000f / 64.0f) : 0.3000f + (d - 64.0f) * ((0.9900f - 0.3000f) / 63.0f);
    float frac = time_s * (48000.0f / 65536.0f);
    return g * (1.0f - 0.25f * (frac > 1.2f ? 1.2f : frac));
}

/* EQ (program bytes 26-29): a Low and a Hi shelf on the stereo output, after the delay, as on the hardware.
 * The frequencies are Korg's tables (MIDI implementation, T-11 / T-10). The shape is measured on the VST
 * (noise through its EQ, tools/vst_ab.py eq): first-order shelves whose table frequency is the corner, 3 dB
 * short of the full gain (+12 dB at 200 Hz reads +9.3 at 200, +6.0 at 400, +2.7 at 800), and a cut is the
 * mirror image of the boost. A band at 0 dB is skipped, so a flat EQ leaves the output bit-identical. */
static const float EQ_LOW_HZ[30] = {
    40, 50, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300,
    320, 340, 360, 380, 400, 420, 440, 460, 480, 500, 600, 700, 800, 900, 1000
};
static const float EQ_HI_HZ[30] = {
    1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000, 3250, 3500, 3750, 4000, 4250, 4500,
    4750, 5000, 5250, 5500, 5750, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000
};

static void eq_update(synth_engine_t *synth, float fs) {
    int key = 1 + synth->eq_freq[0] + 30 * (synth->eq_freq[1] + 30 * ((synth->eq_gain[0] + 12) + 25 * (synth->eq_gain[1] + 12)));
    if (key == synth->eq_key) return;
    synth->eq_key = key;
    for (int band = 0; band < 2; band++) {
        float f0 = band ? EQ_HI_HZ[synth->eq_freq[1]] : EQ_LOW_HZ[synth->eq_freq[0]];
        int db = synth->eq_gain[band];
        /* Boost: H(s) = (s + G) / (s + 1) for the Low shelf, (G s + 1) / (s + 1) for the Hi shelf, s in units of
         * the corner; bilinear transform, pre-warped at the corner. A cut is 1 / (the boost of the same size). */
        float G = powf(10.0f, (float)(db < 0 ? -db : db) / 20.0f);
        float K = 1.0f / tanf((float)M_PI * f0 / fs);
        float n1 = band ? G : 1.0f, n0 = band ? 1.0f : G;
        float num[2] = { n1 * K + n0, n0 - n1 * K }, den[2] = { K + 1.0f, 1.0f - K };
        const float *b = db < 0 ? den : num, *a = db < 0 ? num : den;
        float *c = synth->eq_coef[band];
        c[0] = b[0] / a[0]; c[1] = b[1] / a[0]; c[2] = 0.0f; c[3] = a[1] / a[0]; c[4] = 0.0f;
    }
}

static inline float eq_band(const float *c, float *z, float x) {
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    if (fabsf(z[0]) < 1e-20f) z[0] = 0.0f;
    if (fabsf(z[1]) < 1e-20f) z[1] = 0.0f;
    return y;
}

/* A program's EQ into the instance (struct EqParams: index / 29, 0.5 + dB / 24) */
static void eq_from_preset(synth_engine_t *synth, const struct EqParams *e) {
    synth->eq_freq[0] = clampi((int)lroundf(e->low_freq * 29.0f), 0, 29);
    synth->eq_freq[1] = clampi((int)lroundf(e->hi_freq * 29.0f), 0, 29);
    synth->eq_gain[0] = clampi((int)lroundf((e->low_gain - 0.5f) * 24.0f), -12, 12);
    synth->eq_gain[1] = clampi((int)lroundf((e->hi_gain - 0.5f) * 24.0f), -12, 12);
}

static void flush_effects(synth_engine_t *synth) {
    memset(synth->eq_z, 0, sizeof(synth->eq_z));
    memset(synth->chorus_buf_l, 0, sizeof(synth->chorus_buf_l));
    memset(synth->chorus_buf_r, 0, sizeof(synth->chorus_buf_r));
    memset(synth->delay_buf_l, 0, sizeof(synth->delay_buf_l));
    memset(synth->delay_buf_r, 0, sizeof(synth->delay_buf_r));
    synth->delay_filter_l = synth->delay_filter_r = 0.0f;
    memset(synth->phaser_ap, 0, sizeof(synth->phaser_ap));
    synth->phaser_fb[0] = synth->phaser_fb[1] = 0.0f;
    synth->dc_x[0] = synth->dc_x[1] = synth->dc_y[0] = synth->dc_y[1] = 0.0f;
    synth->out_dc_x[0] = synth->out_dc_x[1] = synth->out_dc_y[0] = synth->out_dc_y[1] = 0.0f;
}

static void flush_for_program(synth_engine_t *synth) {
    synth_all_notes_off(synth);
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_t *v = &synth->voices[i];
        if (v->active && v->amp_env.stage != ENV_IDLE) continue; /* fading out, wiped when it ends */
        v->active = false;
        v->gate = false;
        v->kill = false;
        v->amp_env.stage = ENV_IDLE;
        v->amp_env.value = 0.0f;
        v->filter_env.stage = ENV_IDLE;
        v->filter_env.value = 0.0f;
        v->filter_svf[0].s1 = v->filter_svf[0].s2 = 0.0f;
        v->filter_svf[1].s1 = v->filter_svf[1].s2 = 0.0f;
    }
    if (synth->out_level > FLUSH_AUDIBLE) {
        if (synth->flush_stage == 0) synth->flush_gain = 1.0f;
        synth->flush_stage = 1;
    } else {
        flush_effects(synth);
    }
}

/* Load patch data into the active engine and voices; index is the bank slot it occupies */
static void load_preset_from(synth_engine_t *synth, const struct Preset *p, int index) {
    float saved_timbre_balance = (synth->params[PARAM_TIMBRE_BALANCE] > 0.0f) ? synth->params[PARAM_TIMBRE_BALANCE] : synth->timbre_balance;

    flush_for_program(synth);
    synth->current_preset = index;
    int genre_category, patch;
    program_layout(synth, index, &genre_category, &patch);
    int bank_side = patch / 8;
    int program_num = patch + 1;

    synth->bank_side = bank_side;
    synth->genre_category = genre_category;
    synth->program_num = program_num;

    /* Timbre 1 into the active parameter set, then the shared FX section */
    apply_timbre(synth->params, &synth->timbre_extra[0], &p->t1);
    synth->params[PARAM_CHORUS_MIX] = clamp01f(p->chorus_mix);
    synth->modfx_speed = clamp01f(p->modfx_speed);
    synth->modfx_type = (int)lroundf(clamp01f(p->modfx_type) * 2.0f);
    eq_from_preset(synth, &p->eq);
    synth->delay_type = clampi((int)lroundf(p->delay_type * 2.0f), 0, 2);
    synth->params[PARAM_DELAY_TIME] = clamp01f(p->delay_time);
    synth->params[PARAM_DELAY_FEEDBACK] = clamp01f(p->delay_feedback);
    synth->params[PARAM_DELAY_MIX] = clamp01f(p->delay_mix);
    /* A tempo-synced delay: the Delay Time knob then steps the time base, starting on the program's */
    synth->delay_sync_note = (p->delay_sync > 0.0f) ? (int)lroundf(p->delay_sync * 15.0f) - 1 : -1;
    if (synth->delay_sync_note > 14) synth->delay_sync_note = 14;
    if (synth->delay_sync_note >= 0) synth->params[PARAM_DELAY_TIME] = (float)synth->delay_sync_note / 14.0f;

    /* Parameters not stored in Preset struct: guarantee valid audible defaults */
    if (synth->params[PARAM_MASTER_VOL] <= 0.05f) {
        synth->params[PARAM_MASTER_VOL] = 0.8f;
    }
    if (synth->params[PARAM_PAN] <= 0.001f && synth->params[PARAM_PAN] >= -0.001f) {
        synth->params[PARAM_PAN] = 0.5f;
    }
    if (synth->params[PARAM_LFO1_RATE] <= 0.001f) {
        synth->params[PARAM_LFO1_RATE] = 0.3f;
    }
    if (synth->params[PARAM_LFO2_RATE] <= 0.001f) {
        synth->params[PARAM_LFO2_RATE] = 0.3f;
    }

    /* Preset 0 specific audible defaults guarantee */
    if (index == 0) {
        if (synth->params[PARAM_MASTER_VOL] <= 0.05f) synth->params[PARAM_MASTER_VOL] = 0.8f;
        if (synth->params[PARAM_PAN] <= 0.001f && synth->params[PARAM_PAN] >= -0.001f) synth->params[PARAM_PAN] = 0.5f;
    }

    /* Copy loaded preset parameters into both timbre states */
    memcpy(synth->timbre_params[0], synth->params, sizeof(synth->params));
    memcpy(synth->timbre_params[1], synth->params, sizeof(synth->params));

    /* Timbre 2 gets its own complete parameter set (waves, tuning, filter, envelopes) */
    apply_timbre(synth->timbre_params[1], &synth->timbre_extra[1], &p->t2);

    synth->timbre_edit = 0;

    /* The program's arpeggiator, on or off as stored (the Arp knob overrides it until the next program). A running
     * arpeggio carries on with the new settings, or stops if the new program's is off. */
    arp_settings_from_program(&synth->arp.set, &p->arp);
    if (synth->arp.running) {
        if (!synth->arp.set.on) {
            arp_release(&synth->arp, arp_emit_voice, synth);
            arp_reset(&synth->arp);
        } else {
            arp_resync(&synth->arp);
        }
    }

    /* Set active voice_mode to the preset's native mode */
    int native_vm = (p->voice_mode == 1) ? 1 : 0;
    synth->voice_mode = native_vm;
    synth->params[PARAM_VOICE_MODE] = (native_vm == 1) ? 1.0f : 0.0f;
    synth->timbre_params[0][PARAM_VOICE_MODE] = synth->params[PARAM_VOICE_MODE];
    synth->timbre_params[1][PARAM_VOICE_MODE] = synth->params[PARAM_VOICE_MODE];

    float tb = (saved_timbre_balance > 0.0f) ? saved_timbre_balance : 0.5f;
    synth->timbre_balance = tb;
    synth->params[PARAM_TIMBRE_BALANCE] = tb;
    synth->timbre_params[0][PARAM_TIMBRE_BALANCE] = tb;
    synth->timbre_params[1][PARAM_TIMBRE_BALANCE] = tb;

    /* Smooth transition without audio pops: reset filter states of idle voices,
     * sanitize active voices without hard-zeroing in the middle of active oscillation */
    for (int v = 0; v < NUM_VOICES; v++) {
        if (!synth->voices[v].active || synth->voices[v].amp_env.stage == ENV_IDLE) {
            synth->voices[v].filter_svf[0].s1 = 0.0f;
            synth->voices[v].filter_svf[0].s2 = 0.0f;
            synth->voices[v].filter_svf[1].s1 = 0.0f;
            synth->voices[v].filter_svf[1].s2 = 0.0f;
        } else {
            if (isnan(synth->voices[v].filter_svf[0].s1) || isinf(synth->voices[v].filter_svf[0].s1)) synth->voices[v].filter_svf[0].s1 = 0.0f;
            if (isnan(synth->voices[v].filter_svf[0].s2) || isinf(synth->voices[v].filter_svf[0].s2)) synth->voices[v].filter_svf[0].s2 = 0.0f;
            if (isnan(synth->voices[v].filter_svf[1].s1) || isinf(synth->voices[v].filter_svf[1].s1)) synth->voices[v].filter_svf[1].s1 = 0.0f;
            if (isnan(synth->voices[v].filter_svf[1].s2) || isinf(synth->voices[v].filter_svf[1].s2)) synth->voices[v].filter_svf[1].s2 = 0.0f;
        }
    }
    if (isnan(synth->delay_filter_l) || isinf(synth->delay_filter_l)) synth->delay_filter_l = 0.0f;
    if (isnan(synth->delay_filter_r) || isinf(synth->delay_filter_r)) synth->delay_filter_r = 0.0f;
}

/* Load preset from the active bank (built-in FACTORY_PRESETS or a .syx bank) into the engine and voices */
static void load_preset(synth_engine_t *synth, int index) {
    if (index < 0) index = 0;
    if (index >= 128) index = 127;
    index = playable_raw(synth, index); /* a vocoder program is never loaded: the next playable one is */
    load_preset_from(synth, &active_presets(synth)[index], index);
}

void synth_load_preset(synth_engine_t *synth, int preset_idx) {
    if (!synth) synth = default_synth();
    load_preset(synth, preset_idx);
}

#ifdef TINYK_TUNING
/* Calibration only: load a patch that is not in FACTORY_PRESETS (e.g. decoded from a single-program dump) */
void tinyk_load_patch(synth_engine_t *synth, const struct Preset *p, int slot) {
    if (slot < 0) slot = 0;
    if (slot >= 128) slot = 127;
    load_preset_from(synth, p, slot);
}
#endif

#ifdef TINYK_TUNING
/* Test hooks for the runtime bank loader (tools/test_bank_loader.py via the calibration library) */
int tinyk_dsp_bank_scan(const char *dir) { return syx_scan_dir(dir); }
int tinyk_dsp_bank_count(void) { return g_syx_bank_count; }
const char *tinyk_dsp_bank_name(int b) { return (b >= 1 && b <= g_syx_bank_count) ? g_syx_banks[b - 1].name : "Built-in"; }
/* Copies preset idx of bank b (0 = built-in) into t1/t2 (TimbreParams floats) and fx[5]; returns voice_mode */
int tinyk_dsp_bank_preset(int b, int idx, float *t1, float *t2, float *fx, char *label, int label_len) {
    const struct Preset *p = (b >= 1 && b <= g_syx_bank_count) ? &g_syx_banks[b - 1].presets[idx] : &FACTORY_PRESETS[idx];
    memcpy(t1, &p->t1, sizeof p->t1);
    memcpy(t2, &p->t2, sizeof p->t2);
    fx[0] = p->chorus_mix; fx[1] = p->delay_time; fx[2] = p->delay_feedback; fx[3] = p->delay_mix; fx[4] = p->delay_sync;
    fx[5] = p->modfx_speed; fx[6] = p->modfx_type;
    snprintf(label, (size_t)label_len, "%s", p->label);
    return p->voice_mode;
}
/* Copies preset idx's struct EqParams floats (EQ_FIELDS order) and its delay_type into eq[5]; returns how many */
int tinyk_dsp_bank_eq(int b, int idx, float *eq) {
    const struct Preset *p = (b >= 1 && b <= g_syx_bank_count) ? &g_syx_banks[b - 1].presets[idx] : &FACTORY_PRESETS[idx];
    memcpy(eq, &p->eq, sizeof p->eq);
    eq[4] = p->delay_type;
    return (int)(sizeof p->eq / sizeof(float)) + 1;
}
/* Copies preset idx's struct ArpParams floats (ARP_FIELDS order) into arp; returns how many */
int tinyk_dsp_bank_arp(int b, int idx, float *arp) {
    const struct Preset *p = (b >= 1 && b <= g_syx_bank_count) ? &g_syx_banks[b - 1].presets[idx] : &FACTORY_PRESETS[idx];
    memcpy(arp, &p->arp, sizeof p->arp);
    return (int)(sizeof p->arp / sizeof(float));
}
#endif

/* Synthesizer Initialization */
void synth_init(synth_engine_t *synth) {
    if (!synth) synth = default_synth();
    dwgs_build_tables();
    memset(synth, 0, sizeof(*synth));
    synth->tempo_bpm = 0.0f;  /* unknown: the first block takes the host's tempo as is */
    synth->delay_sync_note = -1;
    synth->noise_state = NOISE_SEED;
    synth->labels_reported = -1;

    /* Initialize all default parameters from PARAM_METAS */
    for (int i = 0; i < NUM_PARAMS; i++) {
        synth->params[i] = PARAM_METAS[i].def_val;
    }

    synth->params[PARAM_MASTER_VOL] = 0.8f;
    synth->params[PARAM_PAN] = 0.5f;
    synth->params[PARAM_OSC_MIX] = 0.5f;
    synth->params[PARAM_CUTOFF] = 0.75f;
    synth->params[PARAM_SUSTAIN2] = 0.8f;
    synth->params[PARAM_DECAY2] = 0.5f;
    synth->params[PARAM_FILTER_TYPE] = 0.0f;

    synth->bank_side = 0;
    synth->genre_category = 0;
    synth->program_num = 1;
    synth->voice_mode = 0;
    synth->timbre_edit = 0;
    synth->timbre_balance = 0.5f;
    synth->params[PARAM_VOICE_MODE] = 0.0f;
    synth->params[PARAM_TIMBRE_EDIT] = 0.0f;
    synth->params[PARAM_TIMBRE_BALANCE] = 0.5f;

    for (int v = 0; v < NUM_VOICES; v++) {
        synth->voices[v].timbre_index = 0;
        synth->voices[v].is_timbre_2 = 0;
        synth->voices[v].layer_partner = -1;
    }

    /* Initialize to Default Preset 0 (A.11 Saw Lead) */
    rebuild_playlist(synth);
    load_preset(synth, 0);
    arp_reset(&synth->arp);

    /* Enforce defaults after load_preset(0) */
    synth->params[PARAM_MASTER_VOL] = 0.8f;
    synth->params[PARAM_PAN] = 0.5f;
    synth->params[PARAM_OSC_MIX] = 0.5f;
    synth->params[PARAM_CUTOFF] = 0.75f;
    synth->params[PARAM_SUSTAIN2] = 0.8f;
    synth->params[PARAM_DECAY2] = 0.5f;
    synth->params[PARAM_FILTER_TYPE] = 0.0f;

    memcpy(synth->timbre_params[0], synth->params, sizeof(synth->params));
    memcpy(synth->timbre_params[1], synth->params, sizeof(synth->params));

    /* Initialize LFOs */
    synth->lfo1.sh_value = 0.0f;
    synth->lfo2.sh_value = 0.0f;
}

/* microKORG program matrix: bank A/B x program 11..88. The first digit is the row (the genre_category,
 * 1..8) and the second the column (1..8), so preset index = bank * 64 + (row - 1) * 8 + (column - 1). */
static void select_program(synth_engine_t *synth, int side, int row, int col) {
    if (side < 0) side = 0;
    if (side > 1) side = 1;
    if (row < 0) row = 0;
    if (row > 7) row = 7;
    if (col < 0) col = 0;
    if (col > 7) col = 7;
    load_preset(synth, side * 64 + row * 8 + col); /* a slot that is not offered loads the next one that is */
}

/* Position `patch` of category `cat` in the Category / Program lists (sets bank_side, genre_category, program_num) */
static void select_category_pos(synth_engine_t *synth, int cat, int patch) {
    load_preset(synth, program_raw(synth, cat, patch));
}

/* The other bank side, same row and column (or the next program that is offered) */
static void select_side(synth_engine_t *synth, int side) {
    load_preset(synth, (side ? 64 : 0) + synth->current_preset % 64);
}

/* Switches the active bank (0 = built-in, 1..N = .syx files) and reloads the current program from it. */
static void select_bank_file(synth_engine_t *synth, int b) {
    if (b < 0) b = 0;
    if (b > g_syx_bank_count) b = g_syx_bank_count;
    synth->bank_file = b;
    rebuild_playlist(synth);
    load_preset(synth, synth->current_preset);
}

/* Category (matrix row) names for the Category control; the Program control lists the category's playable
 * programs, each with its own A1..B8 coordinate (bank sides A and B of that row). */
static const char *const CATEGORY_NAMES[8] = {
    "Trance", "Techno/House", "Electronica", "DnB/Breaks", "Hiphop/Vintage", "Retro", "SE/Hit", "Vocoder"
};

/* Program within the category: its position in the category's list (0..cat_n-1) */
static int current_patch(const synth_engine_t *synth) {
    return synth->program_num - 1;
}


/* Index of `val` in names[], or -1 */
static int name_index(const char *val, const char *const *names, int n) {
    for (int i = 0; i < n; i++) {
        if (strcmp(val, names[i]) == 0) return i;
    }
    return -1;
}

/* Index of `val` among the offered Category names, or -1 */
static int category_index_by_name(const synth_engine_t *synth, const char *val) {
    const char *names[8];
    category_names(synth, names);
    return name_index(val, names, synth->play_ncat);
}

/* Index of `val` ("A3", "B8") among the current category's programs, or -1 */
static int patch_coord_index(const synth_engine_t *synth, const char *val) {
    char coord[8];
    for (int i = 0; i < category_patch_count(synth, synth->genre_category); i++) {
        slot_coord(program_raw(synth, synth->genre_category, i), coord, sizeof coord);
        if (strcmp(val, coord) == 0) return i;
    }
    return -1;
}

/* Index of `val` among the current category's Program labels ("B.12 ARPEJMATR"), or -1 */
static int patch_label_index(const synth_engine_t *synth, const char *val) {
    char label[48];
    for (int i = 0; i < category_patch_count(synth, synth->genre_category); i++) {
        format_preset_name(synth, program_raw(synth, synth->genre_category, i), label, sizeof label);
        if (strcmp(val, label) == 0) return i;
    }
    return -1;
}

/* Some served labels follow the engine state: the Program options are the current category's patch names
 * (Category, Bank), and the per-timbre page names and cell labels show the edited timbre (Timbre Edit,
 * Voice Mode). The host re-reads chain_params and ui_hierarchy on a preset jog or a list pick, but not on a
 * knob turn; it does re-read on is_loading's 1 -> 0 edge. So is_loading answers "1" once for each label
 * change it has not reported yet (synth->labels_reported): a turn reads "1" on the next poll and "0" on the one after, and a knob
 * still turning keeps answering "1", so the labels are re-read once, when it stops. */
/* The timbre the per-timbre controls edit: Timbre 2 only when it is selected in Layer mode */
static int edit_timbre(const synth_engine_t *synth) {
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    return (is_layer && synth->timbre_edit == 1) ? 1 : 0;
}

/* Which timbre the per-timbre labels name: 0 in Single mode (one timbre, plain labels, Timbre Edit "N/A"),
 * 1 or 2 in Layer mode (labels "L1.CUT" / "L2.CUT", the pages' header badge [L1] / [L2]) */
static int timbre_badge(const synth_engine_t *synth) {
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    return is_layer ? edit_timbre(synth) + 1 : 0;
}

/* In-place edits of the served ui_hierarchy: replace the first `from` by `to`, and cut the {...} object that
 * starts with `head` together with one adjoining comma (its entries hold no nested objects or braces). */
static void json_swap(char *buf, const char *from, const char *to) {
    char *p = strstr(buf, from);
    size_t lf = strlen(from), lt = strlen(to);
    if (!p || lt > lf) return;
    memcpy(p, to, lt);
    memmove(p + lt, p + lf, strlen(p + lf) + 1);
}
static void json_cut_object(char *buf, const char *head) {
    char *p = strstr(buf, head);
    if (!p) return;
    char *e = strchr(p, '}');
    if (!e) return;
    e++;
    if (*e == ',') e++;
    else if (p > buf && p[-1] == ',') p--;
    memmove(p, e, strlen(e) + 1);
}

/* Osc page, second knob: Control 1, or the DWGS waveform selector while the edited layer's Wave 1 is DWGS
 * (the hardware's Control knobs change role with the wave the same way) */
static int dwgs_knob_shown(const synth_engine_t *synth) {
    const float *p = synth->timbre_params[edit_timbre(synth)];
    return (int)(p[PARAM_WAVE1] * (float)(OSC1_WAVE_COUNT - 1) + 0.5f) == OSC1_WAVE_DWGS;
}

/* The Osc page's Control 1 reads "Pulse Width" while the edited layer's Wave 1 is the Pulse (Square) wave */
static int pulse_knob_shown(const synth_engine_t *synth) {
    const float *p = synth->timbre_params[edit_timbre(synth)];
    return (int)(p[PARAM_WAVE1] * (float)(OSC1_WAVE_COUNT - 1) + 0.5f) == OSC1_WAVE_SQUARE;
}

/* Everything the served labels depend on: the Program names (bank, category), the timbre badge and the
 * Osc page's second knob (Control 1, its Pulse Width reading, or the DWGS selector) */
static int label_context(const synth_engine_t *synth) {
    return (((synth->bank_file * 8 + synth->genre_category) * 3 + timbre_badge(synth)) * 2 + dwgs_knob_shown(synth)) * 2
           + pulse_knob_shown(synth);
}

/* Selector params served to the host as enum knobs over a 0..1 engine value: options are the names the
 * overlay shows, short_options what the knob's option square shows (<= 4 characters fit on one line;
 * the host draws a waveform icon for the wave enums). They travel as option indices; `to_engine` maps an
 * option index to the engine's mode (NULL = the same), and the engine value is mode / (count - 1). */
static const char *const WAVE1_NAMES[7] = { "Saw", "Square", "Triangle", "Sine", "Vox", "DWGS", "Noise" };
static const char *const WAVE1_SHORT[7] = { "SAW", "SQR", "TRI", "SIN", "VOX", "DWG", "NZ" };
static const char *const WAVE2_NAMES[3] = { "Saw", "Square", "Triangle" };
static const char *const WAVE2_SHORT[3] = { "SAW", "SQR", "TRI" };
static const char *const FILTER_NAMES[4] = { "LPF24", "LPF12", "BPF12", "HPF12" };
/* Osc 2 mod in the hardware's order (off, ring, sync, ring sync); the engine's is off, sync, ring, both */
static const char *const SYNC_RING_NAMES[4] = { "Off", "Ring", "Sync", "Ring Sync" };
static const char *const SYNC_RING_SHORT[4] = { "OFF", "RING", "SYNC", "R.SNC" };
static const int SYNC_RING_TO_ENGINE[4] = { SYNC_RING_OFF, SYNC_RING_RING, SYNC_RING_SYNC, SYNC_RING_BOTH };

/* Voice assign (the layer's polyphony): the engine value is assign / 2 over ASSIGN_MONO, POLY, UNISON */
static const char *const ASSIGN_NAMES[3] = { "Mono", "Poly", "Unison" };
static const char *const ASSIGN_SHORT[3] = { "MONO", "POLY", "UNIS" };

typedef struct {
    const char *key, *name;
    const char *const *options, *const *shorts;
    int count;
    const int *to_engine; /* an involution here, so it also maps engine mode -> option index */
} selector_t;

static const char *const ONOFF_NAMES[2] = { "Off", "On" };
static const char *const ONOFF_SHORT[2] = { "OFF", "ON" };
static const selector_t SELECTORS[] = {
    { "wave1", "Wave 1", WAVE1_NAMES, WAVE1_SHORT, 7, NULL },
    { "wave2", "Wave 2", WAVE2_NAMES, WAVE2_SHORT, 3, NULL },
    { "sync_ring", "Sync / Ring", SYNC_RING_NAMES, SYNC_RING_SHORT, 4, SYNC_RING_TO_ENGINE },
    { "filter_type", "Filter Type", FILTER_NAMES, FILTER_NAMES, 4, NULL },
    { "voice_assign", "Voice", ASSIGN_NAMES, ASSIGN_SHORT, 3, NULL },
    /* the microKORG's distortion is a switch (timbre byte 27 bit 0); programs store it as 0 / 0.5, any value above 0 is on */
    { "drive", "Distortion", ONOFF_NAMES, ONOFF_SHORT, 2, NULL },
};

static const selector_t *find_selector(const char *key) {
    for (size_t i = 0; i < sizeof SELECTORS / sizeof SELECTORS[0]; i++) {
        if (strcmp(key, SELECTORS[i].key) == 0) return &SELECTORS[i];
    }
    return NULL;
}

/* Osc 2 pitch is one value (detune: +-24 semitones over 0..1); Semi and Tune split it into whole
 * semitones and cents, the way the hardware stores it (semitone +-24, tune +-50 cents) */
static void osc2_semi_tune(float detune, int *semi, int *cents) {
    float d = (detune - 0.5f) * 48.0f;
    *semi = (int)lroundf(d);
    *cents = (int)lroundf((d - (float)*semi) * 100.0f);
}

static float osc2_detune(int semi, int cents) {
    return clamp01f(0.5f + ((float)semi + (float)cents / 100.0f) / 48.0f);
}

/* An index control's value: integers are indices; a value strictly between 0 and 1 is a normalized
 * position from a generic host control (scaled to 0..max). */
static int index_from_value(float val, int max) {
    int i = (val > 0.0f && val < 1.0f) ? (int)roundf(val * (float)max) : (int)roundf(val);
    return i < 0 ? 0 : (i > max ? max : i);
}

/* Current program number 11..88 */
static int program_number(const synth_engine_t *synth) {
    int slot = synth->current_preset;
    return ((slot % 64) / 8 + 1) * 10 + slot % 8 + 1;
}

/* Selects program n (11..88) in the current bank. Numbers between rows come from stepping a plain integer
 * control and are snapped the way stepping means them: x9 moves on to the next row's first program, x0
 * back to the previous row's last (18 -> 19 = 21, 21 -> 20 = 18). */
static void set_program_number(synth_engine_t *synth, int n) {
    int row = n / 10, col = n % 10;
    if (col == 9) { row++; col = 1; }
    if (col == 0) { row--; col = 8; }
    if (row < 1) { row = 1; col = 1; }
    if (row > 8) { row = 8; col = 8; }
    select_program(synth, synth->bank_side, row - 1, col - 1);
}

void synth_set_param(synth_engine_t *synth, const char *key, float val) {
    if (!synth) synth = default_synth();

    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        int mode = (val >= 0.5f) ? 1 : 0;
        synth->voice_mode = mode;
        synth->params[PARAM_VOICE_MODE] = (mode == 1) ? 1.0f : 0.0f;
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 || strcmp(key, "Layer") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        int t = (val >= 0.5f) ? 1 : 0;
        synth->timbre_edit = t;
        synth->params[PARAM_TIMBRE_EDIT] = (t == 1) ? 1.0f : 0.0f;
        return;
    }

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 || strcmp(key, "Layer Bal") == 0 ||
        strcmp(key, "Balance") == 0 || strcmp(key, "4") == 0 || strcmp(key, "param_4") == 0) {
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        synth->timbre_balance = val;
        synth->params[PARAM_TIMBRE_BALANCE] = val;
        return;
    }

    /* Voice assign of the edited layer (Mono / Poly / Unison); the sounding notes are cut over to the new rule */
    if (strcmp(key, "voice_assign") == 0) {
        int a = (int)lroundf(clamp01f(val) * 2.0f), t = edit_timbre(synth);
        if (synth->timbre_extra[t].assign != a) {
            synth->timbre_extra[t].assign = a;
            synth_all_notes_off(synth);
        }
        return;
    }

    /* Per-timbre controls kept outside params[]: amp level and noise level (timbre extras), and Osc 2
     * Semi (-24..24) / Tune (-50..50 cents), which both edit detune */
    if (strcmp(key, "level") == 0) {
        synth->timbre_extra[edit_timbre(synth)].level = clamp01f(val);
        return;
    }
    if (strcmp(key, "noise_level") == 0) {
        synth->timbre_extra[edit_timbre(synth)].noise_level = clamp01f(val);
        return;
    }
    /* DWGS waveform 0..63 of the edited layer; dwgs_pick (the DWGS Waves list) also turns Wave 1 to DWGS */
    if (strcmp(key, "dwgs_wave") == 0 || strcmp(key, "dwgs_pick") == 0) {
        int i = (int)lroundf(val), t = edit_timbre(synth);
        if (i < 0) i = 0;
        if (i > DWGS_WAVE_COUNT - 1) i = DWGS_WAVE_COUNT - 1;
        synth->timbre_extra[t].dwgs = (float)i / (float)(DWGS_WAVE_COUNT - 1);
        if (key[5] == 'p') synth_set_param(synth, "wave1", (float)OSC1_WAVE_DWGS / (float)(OSC1_WAVE_COUNT - 1));
        return;
    }
    /* Mod Wheel knob (0..127) for a Move without a wheel: it sets the same patch source (7) as CC1, so
     * whichever moved last wins and an external wheel takes over as soon as it sends */
    if (strcmp(key, "mod_wheel") == 0) {
        synth->modwheel_src = clamp01f(val / 127.0f);
        return;
    }
    if (strcmp(key, "delay_type") == 0) { /* 0 Stereo, 1 Cross, 2 L/R (program byte 22) */
        synth->delay_type = clampi((int)lroundf(val), 0, 2);
        return;
    }
    /* Osc 1's Control 1 / 2 of the edited layer, 0..127 as on the hardware: what they do depends on the wave
     * (pulse width and its LFO1 depth, the Vox formant, the Noise oscillator's cutoff and resonance, the sine's
     * cross-modulation ...). Control 1 is also the pulse's width parameter. */
    if (strcmp(key, "osc1_ctrl1") == 0 || strcmp(key, "osc1_ctrl2") == 0) {
        int t = edit_timbre(synth), which = key[9] == '2';
        float v01 = clamp01f(val / 127.0f);
        synth->timbre_extra[t].osc1_ctrl[which] = v01;
        if (!which) synth_set_param(synth, "pulse_width", v01);
        return;
    }
    /* EQ: frequency index 0..29 (Low 40..1000 Hz, Hi 1..18 kHz), gain -12..+12 dB */
    if (strncmp(key, "eq_", 3) == 0) {
        int band = strncmp(key + 3, "hi_", 3) == 0 ? 1 : (strncmp(key + 3, "low_", 4) == 0 ? 0 : -1);
        const char *field = band < 0 ? "" : key + (band ? 6 : 7);
        if (strcmp(field, "freq") == 0) { synth->eq_freq[band] = clampi((int)lroundf(val), 0, 29); return; }
        if (strcmp(field, "gain") == 0) { synth->eq_gain[band] = clampi((int)lroundf(val), -12, 12); return; }
    }
    if (strcmp(key, "modfx_speed") == 0) { /* Mod FX LFO speed, 0..1 (program byte 23 / 127) */
        synth->modfx_speed = clamp01f(val);
        return;
    }
    if (strcmp(key, "modfx_type") == 0) { /* 0 Chorus/Flanger, 1 Ensemble, 2 Phaser */
        synth->modfx_type = arp_clampi((int)lroundf(val), 0, 2);
        return;
    }
    if (strcmp(key, "arp_on") == 0) { /* the Arp knob: 0 off, 1 on (the program's setting until changed) */
        set_arp_on(synth, val >= 0.5f);
        return;
    }
    if (strncmp(key, "arp_step", 8) == 0 && key[8] >= '1' && key[8] <= '8' && key[9] == '\0') {
        set_arp_step(synth, key[8] - '1', val >= 0.5f);
        return;
    }
    {
        int ai = arp_param_index(key);
        if (ai >= 0) {
            arp_param_set(synth, ai, (int)lroundf(val));
            return;
        }
    }
    if (strcmp(key, "osc2_semi") == 0 || strcmp(key, "osc2_tune") == 0) {
        int semi, cents;
        osc2_semi_tune(synth_get_param(synth, "detune"), &semi, &cents);
        int v = (int)lroundf(val);
        if (key[5] == 's') semi = v < -24 ? -24 : (v > 24 ? 24 : v);
        else cents = v < -50 ? -50 : (v > 50 ? 50 : v);
        synth_set_param(synth, "detune", osc2_detune(semi, cents));
        return;
    }

    if (strcmp(key, "genre_category") == 0) {
        int g = (val > 1.0f) ? (int)roundf(val) : (int)roundf(val * 7.0f);
        if (g < 0) g = 0;
        select_category_pos(synth, g, current_patch(synth));
        return;
    }

    if (strcmp(key, "program_num") == 0) {
        int p = (val >= 1.0f && val <= 16.0f) ? (int)roundf(val) : (1 + (int)roundf(val * 15.0f));
        if (p < 1) p = 1;
        if (p > 16) p = 16;
        select_category_pos(synth, synth->genre_category, p - 1);
        return;
    }

    if (strcmp(key, "bank_side") == 0) {
        select_side(synth, (val >= 0.5f) ? 1 : 0);
        return;
    }

    if (strcmp(key, "bank_file") == 0) {
        select_bank_file(synth, (int)roundf(val));
        return;
    }

    if (strcmp(key, "category") == 0) {
        select_category_pos(synth, index_from_value(val, synth->play_ncat - 1), current_patch(synth));
        return;
    }

    if (strcmp(key, "patch") == 0) {
        select_category_pos(synth, synth->genre_category,
                            index_from_value(val, category_patch_count(synth, synth->genre_category) - 1));
        return;
    }

    if (strcmp(key, "program") == 0) {
        if (val >= 0.0f && val < 1.0f) {
            int idx = (int)roundf(val * 63.0f); /* normalized 0..1 from a generic host control */
            select_program(synth, synth->bank_side, idx / 8, idx % 8);
        } else {
            set_program_number(synth, (int)roundf(val));
        }
        return;
    }

    if (strcmp(key, "preset") == 0) {
        /* 0..127; "1" is preset 1 (not a 1/127 fraction): the host's preset browser sends indices */
        load_preset(synth, synth->play_map[index_from_value(val, synth->play_n - 1)]); /* an ordinal, see rebuild_playlist */
        return;
    }

    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;

    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(PARAM_METAS[i].id, key) == 0) {
            if (i == PARAM_VOICE_MODE) {
                synth->params[i] = val;
                synth->voice_mode = (val >= 0.5f) ? 1 : 0;
                synth_all_notes_off(synth);
                return;
            }
            if (i == PARAM_TIMBRE_EDIT) {
                synth->params[i] = val;
                synth->timbre_edit = (val >= 0.5f) ? 1 : 0;
                return;
            }
            if (i == PARAM_TIMBRE_BALANCE) {
                synth->params[i] = val;
                synth->timbre_balance = val;
                return;
            }
            int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
            if (i < PARAM_LFO1_RATE) {
                /* params[] always mirrors Timbre 1 (it is what Single mode plays); Timbre 2 is only
                 * touched when it is the timbre being edited in Layer mode, so its patch data survives. */
                if (is_layer && synth->timbre_edit == 1) {
                    synth->timbre_params[1][i] = val;
                } else {
                    synth->params[i] = val;
                    synth->timbre_params[0][i] = val;
                }
            } else {
                /* Global FX/Master params are shared by both timbres */
                synth->params[i] = val;
                synth->timbre_params[0][i] = val;
                synth->timbre_params[1][i] = val;
            }
            return;
        }
    }
}

float synth_get_param(const synth_engine_t *synth, const char *key) {
    if (!synth) synth = default_synth();
    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        return (float)synth->voice_mode;
    }
    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 || strcmp(key, "Layer") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        return (float)synth->timbre_edit;
    }
    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 || strcmp(key, "Layer Bal") == 0 ||
        strcmp(key, "Balance") == 0 || strcmp(key, "4") == 0 || strcmp(key, "param_4") == 0) {
        return synth->timbre_balance;
    }
    if (strcmp(key, "bank_side") == 0) {
        return (float)synth->bank_side;
    }
    if (strcmp(key, "genre_category") == 0) {
        return (float)synth->genre_category;
    }
    if (strcmp(key, "program_num") == 0) {
        return (float)synth->program_num;
    }
    if (strcmp(key, "program") == 0) {
        return (float)program_number(synth);
    }
    if (strcmp(key, "bank_file") == 0) {
        return (float)synth->bank_file;
    }
    if (strcmp(key, "category") == 0) {
        return (float)synth->genre_category;
    }
    if (strcmp(key, "patch") == 0) {
        return (float)current_patch(synth);
    }
    if (strcmp(key, "preset") == 0) {
        return (float)synth->play_ord[synth->current_preset] / (float)(NUM_PRESETS - 1);
    }
    if (strcmp(key, "voice_assign") == 0) {
        return (float)synth->timbre_extra[edit_timbre(synth)].assign / 2.0f;
    }
    if (strcmp(key, "level") == 0) {
        return synth->timbre_extra[edit_timbre(synth)].level;
    }
    if (strcmp(key, "noise_level") == 0) {
        return synth->timbre_extra[edit_timbre(synth)].noise_level;
    }
    if (strcmp(key, "dwgs_wave") == 0 || strcmp(key, "dwgs_pick") == 0) {
        return (float)dwgs_index(&synth->timbre_extra[edit_timbre(synth)]);
    }
    if (strcmp(key, "mod_wheel") == 0) { /* follows CC1 too */
        return synth->modwheel_src * 127.0f;
    }
    if (strcmp(key, "arp_on") == 0) {
        return (float)synth->arp.set.on;
    }
    if (strcmp(key, "delay_type") == 0) return (float)synth->delay_type;
    if (strcmp(key, "osc1_ctrl1") == 0 || strcmp(key, "osc1_ctrl2") == 0) {
        return roundf(synth->timbre_extra[edit_timbre(synth)].osc1_ctrl[key[9] == '2'] * 127.0f);
    }
    if (strcmp(key, "eq_low_freq") == 0) return (float)synth->eq_freq[0];
    if (strcmp(key, "eq_hi_freq") == 0) return (float)synth->eq_freq[1];
    if (strcmp(key, "eq_low_gain") == 0) return (float)synth->eq_gain[0];
    if (strcmp(key, "eq_hi_gain") == 0) return (float)synth->eq_gain[1];
    if (strcmp(key, "modfx_speed") == 0) return synth->modfx_speed;
    if (strcmp(key, "modfx_type") == 0) return (float)synth->modfx_type;
    if (strncmp(key, "arp_step", 8) == 0 && key[8] >= '1' && key[8] <= '8' && key[9] == '\0') {
        return ((synth->arp.set.pattern >> (key[8] - '1')) & 1) ? 0.0f : 1.0f;
    }
    {
        int ai = arp_param_index(key);
        if (ai >= 0) return (float)arp_param_get(synth, ai);
    }
    if (strcmp(key, "osc2_semi") == 0 || strcmp(key, "osc2_tune") == 0) {
        int semi, cents;
        osc2_semi_tune(synth_get_param(synth, "detune"), &semi, &cents);
        return (float)(key[5] == 's' ? semi : cents);
    }
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(PARAM_METAS[i].id, key) == 0) {
            int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
            if (is_layer && i < PARAM_LFO1_RATE) {
                return synth->timbre_params[synth->timbre_edit][i];
            }
            return synth->params[i];
        }
    }
    return -1.0f;
}

static void voice_note_off(synth_engine_t *synth, uint8_t note, int mask);

#define ASSIGN_MONO   0
#define ASSIGN_POLY   1
#define ASSIGN_UNISON 2

/* An oscillator start phase for a voice starting from silence: random, so detuned oscillators and unison voices beat
 * from a different point each note (xorshift, seeded by synth_init so renders repeat) */
static float voice_rand_phase(synth_engine_t *synth) {
    uint32_t x = synth->phase_rng ? synth->phase_rng : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    synth->phase_rng = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

static void voice_fresh_state(synth_engine_t *synth, voice_t *v) {
    v->filter_svf[0].s1 = v->filter_svf[0].s2 = v->filter_svf[1].s1 = v->filter_svf[1].s2 = 0.0f;
    v->flt_shelf_x1 = v->flt_shelf_y1 = 0.0f;
    v->noise_svf.s1 = v->noise_svf.s2 = 0.0f;
    v->osc_lp[0] = v->osc_lp[1] = 0.0f;
    v->osc_lp_on = 0;
    v->osc1_phase = voice_rand_phase(synth);
    v->osc1_cycle = 0;
    v->pan_fresh = 1;
    v->osc2_phase = voice_rand_phase(synth);
    v->sub_phase = voice_rand_phase(synth);
    v->amp_env.value = 0.0f;
    v->filter_env.value = 0.0f;
    v->declick_pos = 0;
    v->reset_from = v->amp_heard = 0.0f;
    v->vel_gain = 1.0f;
    /* the note-on kick to the filter (FLT_KICK): 0.5 .. 2 times its typical size, at any phase of the ring */
    v->flt_kick = exp2f(2.0f * voice_rand_phase(synth) - 1.0f);
    v->flt_kick_phase = 2.0f * (float)M_PI * voice_rand_phase(synth);
}

/* Starts a voice's envelopes for a note-on. EG Reset (timbre byte +1, bits 4 and 5; on in every factory program),
 * measured on the VST with a key pressed again 0.3 s into its release: with the amp EG's reset on, the sounding
 * level is gone within 4 ms and the attack starts again from zero (with a slow attack the note dips to silence and
 * swells; with an instant one nothing is heard of it); with it off the attack goes on from where the level was.
 * The filter EG likewise drops to zero at once or carries on. Here a sounding voice fades its old level out over
 * the note-on fade (DECLICK_SAMPLES, the plug-in's own onset reversed) while the new attack fades in. A voice
 * that was silent starts from zero either way. */
static void voice_gate_on(const timbre_extra_t *x, voice_t *v, bool was_active, float atk1_coef, float atk2_coef) {
    if (was_active && x->eg_reset[0]) v->filter_env.value = 0.0f;
    if (was_active && x->eg_reset[1]) {
        v->reset_from = v->amp_heard;
        v->declick_pos = 0;
        v->amp_env.value = 0.0f;
    }
    adsr_gate_on(&v->filter_env, atk1_coef);
    adsr_gate_on(&v->amp_env, atk2_coef);
}

/* A timbre's voices: in Layer mode every second one (t, t + 2, ...: half each), in Single mode all of them */
static int timbre_voices(const synth_engine_t *synth, int t, int *idx) {
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    if (is_layer) {
        int n = 0;
        for (int i = t; i < NUM_VOICES; i += 2) idx[n++] = i;
        return n;
    }
    for (int i = 0; i < NUM_VOICES; i++) idx[i] = i;
    return NUM_VOICES;
}

/* Mono / Unison: starts voice v (k of n stacked) on a note, or moves it there legato (retrigger 0, the EGs run on).
 * The n voices spread evenly over the unison detune (lowest to highest) and over the stereo field (left to right). */
static void group_voice_start(synth_engine_t *synth, voice_t *v, int t, uint8_t note, float vel01, float target_pitch,
                              float atk1_coef, float atk2_coef, int k, int n, int retrigger) {
    const timbre_extra_t *x = &synth->timbre_extra[t];
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    bool was_active = v->active && (v->amp_env.stage != ENV_IDLE);
    bool was_gated = was_active && v->gate;
    /* Unison, measured on the VST (4 voices): the voices sit at -d, -d/2, +d/2, +d cents for a detune of d,
     * each at a full voice's level (the stack is 6 dB over one voice), all in the centre; the same four in
     * Layer mode (measured: a layered unison timbre is 5.8 dB over its poly level). */
    float place = 0.0f;
    if (n > 1) {
        int half = n / 2, step = k < half ? k - half : k - half + 1 - (n & 1); /* 4: -2 -1 +1 +2; 2: -1 +1 */
        place = (float)step / (float)half;
    }
    v->active = true;
    v->gate = true;
    v->note = note;
    v->velocity = vel01;
    v->age = synth->voice_counter;
    v->timbre_index = t;
    v->is_timbre_2 = is_layer ? t : 0;
    v->layer_partner = -1;
    v->unison_cents = place * x->unison_cents;
    v->unison_pan = 0.0f;
    v->unison_gain = 1.0f;
    v->kill = false;
    if (!was_active || synth->timbre_params[t][PARAM_PORTAMENTO] < 0.005f) v->current_pitch = target_pitch;
    v->target_pitch = target_pitch;
    if (!was_active) voice_fresh_state(synth, v);
    if (retrigger || !was_gated) voice_gate_on(x, v, was_active, atk1_coef, atk2_coef);
}

/* Mono / Unison note on: the key goes on top of the timbre's key stack; one voice (Mono) or all the timbre's voices
 * (Unison) play it. With single trigger, a key pressed while another is held glides there without restarting the EGs. */
static void group_note_on(synth_engine_t *synth, int t, uint8_t note, float vel01, float target_pitch,
                          float atk1_coef, float atk2_coef) {
    const timbre_extra_t *x = &synth->timbre_extra[t];
    int held_before = synth->mono_count[t] > 0;
    int c = 0;
    for (int i = 0; i < synth->mono_count[t]; i++) {
        if (synth->mono_keys[t][i] == note) continue;
        synth->mono_keys[t][c] = synth->mono_keys[t][i];
        synth->mono_vel[t][c++] = synth->mono_vel[t][i];
    }
    if (c >= 16) { /* full: drop the oldest */
        memmove(&synth->mono_keys[t][0], &synth->mono_keys[t][1], 15);
        memmove(&synth->mono_vel[t][0], &synth->mono_vel[t][1], 15);
        c = 15;
    }
    synth->mono_keys[t][c] = note;
    synth->mono_vel[t][c++] = (uint8_t)lroundf(vel01 * 127.0f);
    synth->mono_count[t] = c;

    int idx[NUM_VOICES], n = timbre_voices(synth, t, idx);
    int stack = (x->assign == ASSIGN_UNISON) ? (n < UNISON_STACK ? n : UNISON_STACK) : 1;
    int retrigger = !held_before || x->multi_trigger;
    for (int k = 0; k < stack; k++) {
        group_voice_start(synth, &synth->voices[idx[k]], t, note, vel01, target_pitch, atk1_coef, atk2_coef, k, stack, retrigger);
    }
}

/* Mono / Unison note off: the key leaves the stack; if it was the sounding one, the voices go back to the key now on
 * top (legato), or release when none is left */
static void group_note_off(synth_engine_t *synth, int t, uint8_t note) {
    int c = 0;
    for (int i = 0; i < synth->mono_count[t]; i++) {
        if (synth->mono_keys[t][i] == note) continue;
        synth->mono_keys[t][c] = synth->mono_keys[t][i];
        synth->mono_vel[t][c++] = synth->mono_vel[t][i];
    }
    synth->mono_count[t] = c;
    int idx[NUM_VOICES], n = timbre_voices(synth, t, idx);
    for (int k = 0; k < n; k++) {
        voice_t *v = &synth->voices[idx[k]];
        if (!v->active || !v->gate || v->note != note) continue;
        if (c > 0) {
            uint8_t top = synth->mono_keys[t][c - 1];
            float pitch = (float)top + (float)(synth->octave_transpose * 12);
            v->note = top;
            v->target_pitch = pitch;
            if (synth->timbre_params[t][PARAM_PORTAMENTO] < 0.005f) v->current_pitch = pitch;
        } else {
            v->gate = false;
            adsr_gate_off(&v->filter_env);
            adsr_gate_off(&v->amp_env);
        }
    }
}

/* Layer mode, one timbre: its voice for a note among its own (timbre_voices): the one already playing the note,
 * else an idle one, else the released one with the lowest level, else the oldest */
static int layer_voice_for(const synth_engine_t *synth, int t, uint8_t note) {
    int idx[NUM_VOICES], n = timbre_voices(synth, t, idx);
    int released = -1, oldest = idx[0];
    for (int k = 0; k < n; k++) {
        const voice_t *v = &synth->voices[idx[k]];
        if (v->active && v->note == note) return idx[k];
    }
    for (int k = 0; k < n; k++) {
        const voice_t *v = &synth->voices[idx[k]];
        if (!v->active || v->amp_env.stage == ENV_IDLE) return idx[k];
        if (!v->gate && (released < 0 || v->amp_env.value < synth->voices[released].amp_env.value)) released = idx[k];
        if (v->age < synth->voices[oldest].age) oldest = idx[k];
    }
    return released >= 0 ? released : oldest;
}

/* Starts one timbre's voice in Layer mode (the single-timbre counterpart of the paired start below) */
static void layer_voice_start(synth_engine_t *synth, int t, uint8_t note, float vel01, float target_pitch,
                              float atk1_coef, float atk2_coef) {
    voice_t *v = &synth->voices[layer_voice_for(synth, t, note)];
    bool was_active = v->active && (v->amp_env.stage != ENV_IDLE);
    v->active = true;
    v->gate = true;
    v->note = note;
    v->velocity = vel01;
    v->age = synth->voice_counter;
    v->timbre_index = t;
    v->is_timbre_2 = t;
    v->layer_partner = -1;
    v->unison_cents = v->unison_pan = 0.0f;
    v->unison_gain = 1.0f;
    v->kill = false;
    if (!was_active || synth->timbre_params[t][PARAM_PORTAMENTO] < 0.005f) v->current_pitch = target_pitch;
    v->target_pitch = target_pitch;
    /* A sounding voice (retrigger, steal) keeps its filter state, and its EGs restart as EG Reset says (voice_gate_on),
     * so its sound never jumps (crushing the filter state to 5 % stepped an open filter's output) */
    if (!was_active) voice_fresh_state(synth, v);
    voice_gate_on(&synth->timbre_extra[t], v, was_active, atk1_coef, atk2_coef);
}

/* Note on for the timbres in mask (bit 0 Timbre 1, bit 1 Timbre 2; Single mode plays Timbre 1 whatever the mask).
 * The arpeggiator sends its notes to its target timbre and the keys to the other one. */
static void voice_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity, int mask) {
    if (!synth) synth = default_synth();

    if (velocity == 0) {
        voice_note_off(synth, note, mask);
        return;
    }

    /* Initialize if needed to prevent uninitialized zero state */
    if (synth->params[PARAM_MASTER_VOL] <= 0.01f) {
        synth_init(synth);
    }

    float fs = (float)MOVE_SAMPLE_RATE;
    synth->voice_counter++;

    float target_pitch = (float)note + (float)(synth->octave_transpose * 12);
    float vel01 = (float)velocity / 127.0f;
    if (vel01 <= 0.01f) vel01 = 0.8f;

    /* Compute exponential attack coefficients per timbre */
    float t1_atk1_p = synth->timbre_params[0][PARAM_ATTACK1];
    float t1_atk2_p = synth->timbre_params[0][PARAM_ATTACK2];
    float t1_atk1_coef = attack_time_to_coeff(t1_atk1_p, EG_ATTACK_MIN_S * tinyk_tuning.attack_scale, EG_ATTACK_MAX_S * tinyk_tuning.attack_scale, fs);
    float t1_atk2_coef = attack_time_to_coeff(t1_atk2_p, EG_ATTACK_MIN_S * tinyk_tuning.attack_scale, EG_ATTACK_MAX_S * tinyk_tuning.attack_scale, fs);

    float t2_atk1_p = synth->timbre_params[1][PARAM_ATTACK1];
    float t2_atk2_p = synth->timbre_params[1][PARAM_ATTACK2];
    float t2_atk1_coef = attack_time_to_coeff(t2_atk1_p, EG_ATTACK_MIN_S * tinyk_tuning.attack_scale, EG_ATTACK_MAX_S * tinyk_tuning.attack_scale, fs);
    float t2_atk2_coef = attack_time_to_coeff(t2_atk2_p, EG_ATTACK_MIN_S * tinyk_tuning.attack_scale, EG_ATTACK_MAX_S * tinyk_tuning.attack_scale, fs);

    float *params = synth->params;
    /* Synchronize voice_mode flag and params */
    if (synth->voice_mode == 1 && params[PARAM_VOICE_MODE] <= 0.5f) {
        params[PARAM_VOICE_MODE] = 1.0f;
    } else if (synth->voice_mode == 0 && params[PARAM_VOICE_MODE] > 0.5f) {
        synth->voice_mode = 1;
    }
    int is_layer_mode = (params[PARAM_VOICE_MODE] > 0.5f);

    if (is_layer_mode) {
        /* Layer mode: each timbre in its own half of the voices, as its voice assign says */
        for (int t = 0; t < 2; t++) {
            if (!((mask >> t) & 1)) continue;
            float atk1 = t ? t2_atk1_coef : t1_atk1_coef, atk2 = t ? t2_atk2_coef : t1_atk2_coef;
            if (synth->timbre_extra[t].assign == ASSIGN_POLY) layer_voice_start(synth, t, note, vel01, target_pitch, atk1, atk2);
            else group_note_on(synth, t, note, vel01, target_pitch, atk1, atk2);
        }
    } else if (synth->timbre_extra[0].assign != ASSIGN_POLY) {
        /* Single mode, Mono or Unison: one voice, or UNISON_STACK of them */
        group_note_on(synth, 0, note, vel01, target_pitch, t1_atk1_coef, t1_atk2_coef);
    } else {
        /* =========================================================
         * SINGLE MODE (polyphonic over all the voices)
         * ========================================================= */
        float portamento = synth->timbre_params[0][PARAM_PORTAMENTO];
        int voice_idx = -1;

        /* 1. Check if same note is already sounding on a voice: retrigger */
        for (int i = 0; i < NUM_VOICES; i++) {
            if (synth->voices[i].active && synth->voices[i].note == note) {
                voice_idx = i;
                break;
            }
        }

        /* 2. Find free/idle voice */
        if (voice_idx < 0) {
            for (int i = 0; i < NUM_VOICES; i++) {
                if (!synth->voices[i].active || synth->voices[i].amp_env.stage == ENV_IDLE) {
                    voice_idx = i;
                    break;
                }
            }
        }

        /* 3. Voice Stealing:
         * - Priority 1: steal a released voice (gate == false) with lowest amplitude.
         * - Priority 2: steal the oldest active voice.
         */
        if (voice_idx < 0) {
            float lowest_released_amp = 1e9f;
            int released_idx = -1;
            uint32_t oldest_age = 0xFFFFFFFF;
            int oldest_idx = 0;

            for (int i = 0; i < NUM_VOICES; i++) {
                voice_t *vi = &synth->voices[i];
                if (!vi->gate) {
                    if (vi->amp_env.value < lowest_released_amp) {
                        lowest_released_amp = vi->amp_env.value;
                        released_idx = i;
                    }
                }
                if (vi->age < oldest_age) {
                    oldest_age = vi->age;
                    oldest_idx = i;
                }
            }
            voice_idx = (released_idx >= 0) ? released_idx : oldest_idx;
        }

        if (voice_idx < 0 || voice_idx >= NUM_VOICES) {
            voice_idx = 0;
        }

        voice_t *v = &synth->voices[voice_idx];
        bool was_active = v->active && (v->amp_env.stage != ENV_IDLE);

        v->active = true;
        v->gate = true;
        v->note = note;
        v->velocity = vel01;
        v->age = synth->voice_counter;
        v->timbre_index = 0;
        v->is_timbre_2 = 0;
        v->layer_partner = -1;
        v->unison_cents = v->unison_pan = 0.0f;
        v->unison_gain = 1.0f;
        v->kill = false;

        if (!was_active || portamento < 0.005f) {
            v->current_pitch = target_pitch;
        }
        v->target_pitch = target_pitch;

        /* as in layer_voice_start: a sounding voice keeps its filter state */
        if (!was_active) voice_fresh_state(synth, v);

        voice_gate_on(&synth->timbre_extra[0], v, was_active, t1_atk1_coef, t1_atk2_coef);

    }

    /* Patch LFOs with key sync restart on note-on, at their positive peak. The engine keeps one LFO per timbre, so the
     * hardware's TIMBRE and VOICE sync modes both restart that LFO. */
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    for (int t = 0; t < (is_layer ? 2 : 1); t++) {
        if (is_layer && !((mask >> t) & 1)) continue;
        for (int l = 0; l < 2; l++) {
            if (synth->timbre_extra[t].lfo_keysync[l] != 0) {
                /* start at the positive peak, as the VST does: sine peaks at phase 0.25; triangle, square and
                 * the (falling) saw are +1 at phase 0 */
                int sine = (l == 1 && synth->timbre_extra[t].lfo_wave[l] == 2);
                synth->patch_lfo[t][l].phase = sine ? 0.25f : 0.0f;
            }
        }
    }
}

void synth_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity) {
    voice_note_on(synth, note, velocity, 3);
}

/* Releases the note on the timbres in mask (a layered pair shares its note, so both halves are found) */
static void voice_note_off(synth_engine_t *synth, uint8_t note, int mask) {
    if (!synth) synth = default_synth();
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    int poly_mask = 0;
    for (int t = 0; t < (is_layer ? 2 : 1); t++) {
        if (is_layer && !((mask >> t) & 1)) continue;
        if (synth->timbre_extra[t].assign != ASSIGN_POLY) group_note_off(synth, t, note);
        else poly_mask |= 1 << t;
    }
    if (!is_layer && poly_mask) poly_mask = 3;
    mask = poly_mask;
    if (!mask) return;
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_t *v = &synth->voices[i];
        if (!v->active || v->note != note || !v->gate) continue;
        if (is_layer && !((mask >> (v->is_timbre_2 ? 1 : 0)) & 1)) continue;
        v->gate = false;
        adsr_gate_off(&v->filter_env);
        adsr_gate_off(&v->amp_env);
    }
}

void synth_note_off(synth_engine_t *synth, uint8_t note) {
    voice_note_off(synth, note, 3);
}

/* --- Keys through the arpeggiator --------------------------------------------------------------------------------
 * MIDI notes come here. With the program's arpeggiator off they play directly; on, they go to it, and it plays its
 * target timbre (Layer mode: Timbre 1, Timbre 2 or both; Single mode: the one timbre) while the other timbre plays
 * the keys as usual: A.21 AutoHouse arpeggiates its bass (Timbre 1) under a held drum (Timbre 2). synth_note_on /
 * synth_note_off stay direct, so renders and tests of a single note are unaffected. */
static int arp_voice_mask(const synth_engine_t *synth) {
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    if (!is_layer) return 3;
    return synth->arp.set.target == 1 ? 1 : (synth->arp.set.target == 2 ? 2 : 3);
}

static void arp_emit_voice(void *ctx, int on, uint8_t note, uint8_t vel) {
    synth_engine_t *synth = (synth_engine_t *)ctx;
    if (on) voice_note_on(synth, note, vel, arp_voice_mask(synth));
    else voice_note_off(synth, note, arp_voice_mask(synth));
}

static void key_note_off(synth_engine_t *synth, uint8_t note) {
    if (!synth->arp.set.on) {
        synth_note_off(synth, note);
        return;
    }
    int direct = 3 & ~arp_voice_mask(synth);
    if (direct) voice_note_off(synth, note, direct);
    arp_key_off(&synth->arp, note, arp_emit_voice, synth);
}

static void key_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity) {
    if (velocity == 0) {
        key_note_off(synth, note);
        return;
    }
    if (!synth->arp.set.on) {
        synth_note_on(synth, note, velocity);
        return;
    }
    int direct = 3 & ~arp_voice_mask(synth);
    if (direct) voice_note_on(synth, note, velocity, direct);
    arp_key_on(&synth->arp, note, velocity, arp_emit_voice, synth);
}

/* --- Arp Settings page -------------------------------------------------------------------------------------------
 * The program's arpeggiator settings as knobs (indices for enums, plain integers for gate and swing). A turn takes
 * effect at once on a running arpeggio: type, range and gate from the next step; resolution and swing re-find the
 * next step on the new grid; latch off drops the keys no longer held; a new target moves the arpeggio and the keys
 * to their new timbres without leaving notes on. All of them are saved in the slot state. */
static const char *const ARP_TYPE_NAMES[6] = { "UP", "DOWN", "ALT1", "ALT2", "RANDOM", "TRIGGER" };
static const char *const ARP_TYPE_SHORT[6] = { "UP", "DOWN", "ALT1", "ALT2", "RND", "TRIG" };
static const char *const ARP_RANGE_NAMES[4] = { "1 Oct", "2 Oct", "3 Oct", "4 Oct" };
static const char *const ARP_RANGE_SHORT[4] = { "1OCT", "2OCT", "3OCT", "4OCT" };
static const char *const ARP_RES_NAMES[6] = { "1/24", "1/16", "1/12", "1/8", "1/6", "1/4" };
static const char *const ARP_OFF_ON[2] = { "Off", "On" };
static const char *const ARP_OFF_ON_SHORT[2] = { "OFF", "ON" };
static const char *const ARP_TARGET_NAMES[3] = { "Both", "Layer 1", "Layer 2" };
static const char *const ARP_TARGET_SHORT[3] = { "BOTH", "L1", "L2" };

typedef struct {
    const char *key, *name, *short_name;
    const char *const *options, *const *shorts; /* NULL: an integer from min to max */
    int count, min, max;
    const char *unit;
} arp_param_t;

static const arp_param_t ARP_PARAMS[8] = {
    { "arp_type", "Type", "TYPE", ARP_TYPE_NAMES, ARP_TYPE_SHORT, 6, 0, 5, NULL },
    { "arp_range", "Range", "RANGE", ARP_RANGE_NAMES, ARP_RANGE_SHORT, 4, 0, 3, NULL },
    { "arp_resolution", "Resolution", "RESO", ARP_RES_NAMES, ARP_RES_NAMES, 6, 0, 5, NULL },
    { "arp_gate", "Gate", "GATE", NULL, NULL, 0, 0, 100, "%" },
    { "arp_swing", "Swing", "SWING", NULL, NULL, 0, -100, 100, "%" },
    { "arp_latch", "Latch", "LATCH", ARP_OFF_ON, ARP_OFF_ON_SHORT, 2, 0, 1, NULL },
    { "arp_key_sync", "Key Sync", "KSYNC", ARP_OFF_ON, ARP_OFF_ON_SHORT, 2, 0, 1, NULL },
    { "arp_target", "Target", "TARGT", ARP_TARGET_NAMES, ARP_TARGET_SHORT, 3, 0, 2, NULL },
};
#define ARP_PARAM_COUNT ((int)(sizeof ARP_PARAMS / sizeof ARP_PARAMS[0]))

static int arp_param_index(const char *key) {
    for (int i = 0; i < ARP_PARAM_COUNT; i++) {
        if (strcmp(key, ARP_PARAMS[i].key) == 0) return i;
    }
    return -1;
}

static int arp_param_get(const synth_engine_t *synth, int i) {
    const arp_settings_t *a = &synth->arp.set;
    switch (i) {
        case 0: return a->type;
        case 1: return a->range - 1;
        case 2: return a->resolution;
        case 3: return (int)lroundf(a->gate * 100.0f);
        case 4: return (int)lroundf(a->swing * 100.0f);
        case 5: return a->latch;
        case 6: return a->key_sync;
        default: return a->target;
    }
}

/* Latch off: the arpeggio keeps only the keys still held */
static void arp_unlatch(synth_engine_t *synth) {
    arp_t *a = &synth->arp;
    int n = 0;
    for (int i = 0; i < a->held_count; i++) {
        int down = 0;
        for (int j = 0; j < a->down_count; j++) down |= a->down_note[j] == a->held_note[i];
        if (!down) continue;
        a->held_note[n] = a->held_note[i];
        a->held_vel[n++] = a->held_vel[i];
    }
    a->held_count = n;
    if (n == 0 && a->running) {
        arp_release(a, arp_emit_voice, synth);
        a->running = 0;
    }
}

/* New target: the arpeggio's notes end on the old timbre, the held keys move from the old direct timbre to the new */
static void set_arp_target(synth_engine_t *synth, int target) {
    arp_t *a = &synth->arp;
    if (target == a->set.target) return;
    int old_direct = 3 & ~arp_voice_mask(synth);
    arp_release(a, arp_emit_voice, synth);
    if (a->set.on && old_direct) {
        for (int i = 0; i < a->down_count; i++) voice_note_off(synth, a->down_note[i], old_direct);
    }
    a->set.target = target;
    int new_direct = 3 & ~arp_voice_mask(synth);
    if (a->set.on && new_direct) {
        for (int i = 0; i < a->down_count; i++) {
            uint8_t vel = 100;
            for (int j = 0; j < a->held_count; j++) {
                if (a->held_note[j] == a->down_note[i]) vel = a->held_vel[j];
            }
            voice_note_on(synth, a->down_note[i], vel, new_direct);
        }
    }
}

static void arp_param_set(synth_engine_t *synth, int i, int v) {
    const arp_param_t *p = &ARP_PARAMS[i];
    arp_t *a = &synth->arp;
    v = arp_clampi(v, p->min, p->max);
    switch (i) {
        case 0: a->set.type = v; break;
        case 1: a->set.range = v + 1; break;
        case 2: a->set.resolution = v; if (a->running) arp_resync(a); break;
        case 3: a->set.gate = (float)v / 100.0f; break;
        case 4: a->set.swing = (float)v / 100.0f; if (a->running) arp_resync(a); break;
        case 5: a->set.latch = v; if (!v) arp_unlatch(synth); break;
        case 6: a->set.key_sync = v; break;
        default: set_arp_target(synth, v); break;
    }
}

/* Arp Steps page: step (0..7) plays or rests from its next turn on, live. A step past the pattern's length extends
 * it to that step (the MS2000 banks store length 1), the steps in between playing. */
/* Where the arpeggiator is, for the Arp Steps LEDs (canvas.js). The host reads it about four times a second, so it
 * gives what the widget needs to run the playhead on by itself between reads:
 *   "0,<length>"                                          stopped (no playhead)
 *   "1,<length>,<next>,<ms to next>,<step ms>,<swing>"    running: <next> is the index of the next step (mod 1680,
 *                                                          which keeps its parity and its place in any length 1..8);
 *                                                          the step now lit is the one before it
 * From an even step to the next takes step x (1 + swing / 3), from an odd one step x (1 - swing / 3) (arp_step_time). Read on
 * the host's param thread: the doubles it reads are written by the audio thread, a torn read only misplaces one frame. */
static int format_arp_playhead(const synth_engine_t *synth, char *buf, int buf_len) {
    const arp_t *a = &synth->arp;
    int len = arp_clampi(a->set.length, 1, 8);
    float bpm = synth->tempo_bpm > 1.0f ? synth->tempo_bpm : 120.0f;
    if (!a->set.on || !a->running) return snprintf(buf, buf_len, "0,%d", len);
    double ms_per_beat = 60000.0 / (double)bpm;
    double to_next = (a->next_step - a->pos) * ms_per_beat;
    if (to_next < 0.0) to_next = 0.0;
    return snprintf(buf, buf_len, "1,%d,%d,%.1f,%.2f,%.3f", len, (int)(a->step_k % 1680), to_next,
                    arp_step_beats(a) * ms_per_beat, a->set.swing);
}

static void set_arp_step(synth_engine_t *synth, int step, int play) {
    uint8_t bit = (uint8_t)(1u << step);
    if (step + 1 > synth->arp.set.length) {
        for (int s = synth->arp.set.length; s < step; s++) synth->arp.set.pattern &= (uint8_t)~(1u << s);
        synth->arp.set.length = step + 1;
    }
    if (play) synth->arp.set.pattern &= (uint8_t)~bit;
    else synth->arp.set.pattern |= bit;
}

/* Turns the arpeggiator on or off (the Arp knob): its notes stop, keys start fresh */
static void set_arp_on(synth_engine_t *synth, int on) {
    on = on ? 1 : 0;
    if (on == synth->arp.set.on) return;
    arp_release(&synth->arp, arp_emit_voice, synth);
    arp_reset(&synth->arp);
    synth->arp.set.on = on;
}

void synth_all_notes_off(synth_engine_t *synth) {
    if (!synth) synth = default_synth();
    arp_reset(&synth->arp);
    synth->mono_count[0] = synth->mono_count[1] = 0;
    /* Sounding voices fade out over KILL_RELEASE_S (a cut was a click); the render frees them at the end */
    for (int i = 0; i < NUM_VOICES; i++) {
        voice_t *v = &synth->voices[i];
        v->gate = false;
        if (!v->active || v->amp_env.stage == ENV_IDLE) {
            v->active = false;
            continue;
        }
        v->kill = true;
        adsr_gate_off(&v->filter_env);
        adsr_gate_off(&v->amp_env);
    }
}

/* Output headroom: the last gain stage, after the Mod FX, delay, DC blockers, master volume and the soft-knee limiter, so
 * nothing before it changes. Native Move instruments are staged around -12..-8 dBFS peak, and a TinyK track should sit
 * with them: no program's full-velocity chord may peak past -6 dBFS (test_headroom in tools/test_behavior.c). The voice
 * mix is linear, as the plug-in's is (TINYK_MIX_GAIN: the soft clip and the limiter act on extremes only), so the
 * programs keep their dynamics (median crest about 15 dB) and the peaks are real: with the built-in bank 0.56 puts the
 * loudest of them (its EQ boosts, program 70) at -6.1 dBFS and a typical chord at -27 dBFS RMS (0.68 left it at -4.4). It is a plain gain: a louder build is
 * TINYK_CFLAGS=-DTINYK_OUTPUT_HEADROOM=0.9f ./scripts/build.sh, with peaks to -1 dBFS. The calibration tools build
 * the engine with -DTINYK_OUTPUT_HEADROOM=1.0f to keep comparing against the VST takes at unity. */
#ifndef TINYK_OUTPUT_HEADROOM
#define TINYK_OUTPUT_HEADROOM 0.56f
#endif

/* Soft-knee saturation / tanh master limiter to guarantee no digital wrap-around clipping */
static inline float soft_knee_limiter(float x) {
    if (isnan(x) || isinf(x)) return 0.0f;
    float abs_x = fabsf(x);
    if (abs_x <= 0.65f) {
        return x;
    }
    float sign = (x >= 0.0f) ? 1.0f : -1.0f;
    float over = abs_x - 0.65f;
    return sign * (0.65f + 0.335f * tanhf(over / 0.335f));
}

/* Audio Render Loop (No dynamic allocations!) */
static void render_segment(synth_engine_t *synth, int16_t *out_lr, int frames, double beat);

/* --- Mod FX ---------------------------------------------------------------------------------------------------------
 * Program byte 25 picks the effect, 23 its LFO speed (rate = modfx_rate_lo_hz * modfx_rate_span^speed), 24 its depth
 * (PARAM_CHORUS_MIX; 0 = off, as on the VST). The chorus line (chorus_buf_*) is written every sample.
 *   Chorus/Flanger: one tap per channel on a triangle LFO, left and right in anti-phase, the delay sweeping
 *     chorus_center_ms +- depth * chorus_depth_ms; the wet copy is added to the dry signal (chorus_wet). Fitted on the
 *     VST's A.11 at speed 44 / 80 (ref_a11_modfx_speed44 / 80): a triangle sweep of about 2..8 ms, the wet takes 2-4 dB
 *     above the dry one, L/R correlation 0.19 / 0.07 (dry 0.47).
 *   Ensemble: four taps sweeping 8 .. 12 ms on one sine LFO (measured: see modfx_process).
 *   Phaser: six first-order all-pass stages per channel swept exponentially by a triangle LFO (right channel a
 *     quarter cycle on), with feedback, summed with the dry signal at 0.707 each, PHASER_* (chosen, not measured). */
#define ENS_WET         0.763f   /* 1 / sqrt(1 + 0.75^2 + 0.4^2): the three taps a side sum to the wet level */
#define PHASER_LO_HZ    490.0f
#define PHASER_HI_HZ    3100.0f

/* Rates measured on the VST, every 8 knob steps (straight lines between).
 * LFO 1 / 2 frequency (a triangle LFO -> pan, the level's period): 0.016 Hz at 0, 0.25 at 24, 1.75 at 40, 5.0 at
 * 64, then doubling every 12 steps (33 Hz at 96, 82 at 112; the top two points continue that line, the pan
 * could not follow them). The old 0.05 x 600^knob was 4-5 x too slow from the middle up.
 * Mod FX speed (the phase wobble of a sine through the chorus at full depth): its own, slower curve. */
static const float LFO_RATE_HZ[17] = { 0.0157f, 0.0900f, 0.1696f, 0.2502f, 0.8302f, 1.7502f, 2.7506f, 3.7500f, 4.9978f, 8.1793f,
    12.9755f, 20.5917f, 32.7045f, 51.9048f, 82.4155f, 130.8f, 196.0f };
static const float MODFX_RATE_HZ[17] = { 0.0454f, 0.0454f, 0.0504f, 0.0926f, 0.1927f, 0.4078f, 0.7937f, 1.4297f, 2.3958f, 3.8194f,
    5.7986f, 8.4722f, 11.9792f, 16.4931f, 22.1528f, 27.1528f, 31.5f };
static float rate_table(const float *hz, float knob01) {
    float k = clamp01f(knob01) * 127.0f;
    if (k >= 120.0f) return hz[15] + (hz[16] - hz[15]) * (k - 120.0f) / 7.0f;
    int i = (int)(k / 8.0f);
    return hz[i] + (hz[i + 1] - hz[i]) * (k / 8.0f - (float)i);
}

static inline float modfx_tri(float ph) { /* phase 0..1 -> triangle -1 (0) .. +1 (0.5) */
    ph -= floorf(ph);
    return 1.0f - 4.0f * fabsf(ph - 0.5f);
}

static inline float chorus_tap(const float *buf, uint32_t wpos, float delay) {
    /* 4-point Hermite: a straight line between samples dulled the tap (and, fed back, every pass of it) */
    float rpos = (float)wpos - delay;
    while (rpos < 0.0f) rpos += (float)CHORUS_BUFFER_SIZE;
    int i1 = (int)rpos % CHORUS_BUFFER_SIZE;
    float x = rpos - floorf(rpos);
    float y0 = buf[(i1 + CHORUS_BUFFER_SIZE - 1) % CHORUS_BUFFER_SIZE], y1 = buf[i1];
    float y2 = buf[(i1 + 1) % CHORUS_BUFFER_SIZE], y3 = buf[(i1 + 2) % CHORUS_BUFFER_SIZE];
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + y1;
}

/* One sample of the program's Mod FX on (l, r); wpos is where this sample was written into the chorus line */
static inline void modfx_process(synth_engine_t *s, float *l, float *r, uint32_t wpos, float depth, float fs) {
    float hz = rate_table(MODFX_RATE_HZ, s->modfx_speed);
    s->chorus_lfo_phase += hz / fs;
    if (s->chorus_lfo_phase >= 1.0f) s->chorus_lfo_phase -= 1.0f;
    float ph = s->chorus_lfo_phase, ms = fs * 0.001f, maxd = (float)(CHORUS_BUFFER_SIZE - 4);
    if (s->modfx_type == 2) {
        /* Phaser, measured on the VST (noise through it against the dry spectrum, held at three depths and
         * tracked over the sweep). Its response is two notches with a resonant peak between them over a
         * lowered floor, the same on both sides (the plug-in's is mono), and it is reproduced here as exactly
         * that, three biquads, because no all-pass-chain-with-feedback tried fits it (2-8 stages, either sign:
         * 5-8 dB off; this form: 0.8-1.1 dB at depth 40, 80 and 127):
         *   notches at fc / 2.36 and fc x 2.36, Q 0.37;
         *   a peak at fc: gain 12.5 + 0.035 d + 0.00085 d^2 dB, Q 0.38 + 4e-7 d^3 (+15 dB / 0.41 at 40, +31 / 1.2 at 127);
         *   floor -1.0 - 0.0324 d dB.
         * fc sweeps 490 Hz .. 3.1 kHz whatever the depth, at its own rate, 0.00149 x speed^2 Hz (0.86 Hz at 24,
         * 3.43 at 48, 13.7 at 96). The sweep's shape (a triangle in octaves) is assumed. */
        float sp = s->modfx_speed * 127.0f, d127 = depth * 127.0f;
        s->chorus_lfo_phase += (0.00149f * sp * sp - hz) / fs; /* its own rate, not the chorus's added above */
        if (s->chorus_lfo_phase >= 1.0f) s->chorus_lfo_phase -= 1.0f;
        if (s->chorus_lfo_phase < 0.0f) s->chorus_lfo_phase += 1.0f;
        if (s->phaser_tick-- <= 0) { /* new coefficients every 16 samples */
            s->phaser_tick = 15;
            float sweep = 0.5f + 0.5f * modfx_tri(s->chorus_lfo_phase);
            float fc = PHASER_LO_HZ * powf(PHASER_HI_HZ / PHASER_LO_HZ, sweep);
            float f0[3] = { fc / 2.36f, fc * 2.36f, fc };
            float pq = 0.38f + 4.0e-7f * d127 * d127 * d127;
            float A = powf(10.0f, (12.5f + 0.035f * d127 + 0.00085f * d127 * d127) / 40.0f);
            for (int k = 0; k < 3; k++) {
                float w0 = 2.0f * (float)M_PI * fminf(f0[k], 0.45f * fs) / fs, cw = cosf(w0);
                float al = sinf(w0) / (2.0f * (k < 2 ? 0.37f : pq));
                float b0 = k < 2 ? 1.0f : 1.0f + al * A, b2 = k < 2 ? 1.0f : 1.0f - al * A;
                float a0 = k < 2 ? 1.0f + al : 1.0f + al / A, a2 = k < 2 ? 1.0f - al : 1.0f - al / A;
                float *c = s->phaser_coef[k];
                c[0] = b0 / a0; c[1] = -2.0f * cw / a0; c[2] = b2 / a0; c[3] = -2.0f * cw / a0; c[4] = a2 / a0;
            }
            s->phaser_floor = powf(10.0f, (-1.0f - 0.0324f * d127) / 20.0f);
        }
        float in[2] = { *l, *r };
        for (int c = 0; c < 2; c++) {
            float x = in[c] * s->phaser_floor;
            for (int k = 0; k < 3; k++) { /* transposed direct form II; phaser_ap holds the two states of each */
                const float *q = s->phaser_coef[k];
                float *st = &s->phaser_ap[c][2 * k];
                float y = q[0] * x + st[0];
                st[0] = q[1] * x - q[3] * y + st[1];
                st[1] = q[2] * x - q[4] * y;
                x = y;
            }
            in[c] = isfinite(x) ? x : 0.0f;
        }
        *l = in[0];
        *r = in[1];
        return;
    }
    if (s->modfx_type == 1) {
        /* Ensemble, measured on the VST (a 27.5 Hz saw through it: every copy of its edge is a tap, followed
         * over a whole LFO cycle at speed 24). Four taps on ONE sine LFO, all sweeping 8 .. 12 ms at every
         * depth: 10 + 2 sin(p) and 10 + 2 sin(p + 60 deg), and the mirror image of each, 10 - 2 sin(..). The
         * first pair's taps go one to each side; the second pair's go to both, about 2 : 1, the mirror image
         * favouring the other side. Depth is only the mix: noise through it sits 1.6 / 3.0 / 3.5 / 2.9 dB
         * under dry at depth 32 / 64 / 96 / 127 with L/R correlation 0.98 / 0.90 / 0.75 / 0.62. The tap
         * weights (1, 0.75, 0.4) are read off the edge copies to about 25 %. */
        float d127 = depth * 127.0f;
        /* the mix that gives those levels and correlations with these taps: the wet side rises in a straight
         * line (0.55 at 127), the dry falls to 0.65 at 64 and 0.46 at 127 */
        static const float ENS_D[5] = { 0, 32, 64, 96, 127 }, ENS_DRY[5] = { 1.0f, 0.82f, 0.65f, 0.524f, 0.46f };
        float dry = flt_table(ENS_D, ENS_DRY, 5, d127), wet = 0.00433f * d127 * ENS_WET;
        float sa = 2.0f * ms * sinf(2.0f * (float)M_PI * ph), sb = 2.0f * ms * sinf(2.0f * (float)M_PI * (ph + 1.0f / 6.0f));
        float c = 10.0f * ms;
        float ap = 0.5f * (chorus_tap(s->chorus_buf_l, wpos, c + sa) + chorus_tap(s->chorus_buf_r, wpos, c + sa));
        float am = 0.5f * (chorus_tap(s->chorus_buf_l, wpos, c - sa) + chorus_tap(s->chorus_buf_r, wpos, c - sa));
        float bp = 0.5f * (chorus_tap(s->chorus_buf_l, wpos, c + sb) + chorus_tap(s->chorus_buf_r, wpos, c + sb));
        float bm = 0.5f * (chorus_tap(s->chorus_buf_l, wpos, c - sb) + chorus_tap(s->chorus_buf_r, wpos, c - sb));
        *l = dry * *l + wet * (ap + 0.75f * bp + 0.4f * bm);
        *r = dry * *r + wet * (am + 0.75f * bm + 0.4f * bp);
        return;
    }
    /* Chorus/Flanger, measured on the VST (noise and a sine through it, depth 8..127 at a slow speed). One
     * moving tap per side on a triangle LFO, the sides in anti-phase. Depth (d, 0..127) does three things:
     *   the sweep moves down and narrows: 9.2 - 0.068 d  ..  24.85 - 0.159 d ms (6.5-18.5 ms at 40, 2.4-9.0
     *     at 100, 0.6-4.6 at 127: a chorus that turns into a flanger);
     *   the mix goes from dry to an even blend by 40, holds to 80, then on to nearly all wet
     *     (dry 1 - d / 80, then 0.5, then 0.5 - 0.0073 (d - 80); wet = 1 - dry);
     *   above 40 the tap is fed back, 0.0104 (d - 40): 0.62 at 100, 0.90 at 127.
     * These reproduce the measured level (-3.2 dB at 40, -1.2 at 100, +3.8 at 127) and L/R correlation
     * (0.5, 0.17, 0.01) of noise; the split between wet level and feedback is inferred from those two, and
     * the feedback's sign and everything below depth 30 (too faint to track) are assumed. */
    float d127 = depth * 127.0f;
    float lo_ms = 9.2f - 0.068f * d127, hi_ms = 24.85f - 0.159f * d127;
    float dry = d127 <= 40.0f ? 1.0f - d127 / 80.0f : (d127 <= 80.0f ? 0.5f : 0.5f - 0.0073f * (d127 - 80.0f));
    float fb = d127 > 40.0f ? fminf(0.92f, 0.0104f * (d127 - 40.0f)) : 0.0f;
    float dl = (lo_ms + (hi_ms - lo_ms) * (0.5f + 0.5f * modfx_tri(ph))) * ms;
    float dr = (lo_ms + (hi_ms - lo_ms) * (0.5f + 0.5f * modfx_tri(ph + 0.5f))) * ms;
    float wl = chorus_tap(s->chorus_buf_l, wpos, fmaxf(3.0f, fminf(maxd, dl)));
    float wr = chorus_tap(s->chorus_buf_r, wpos, fmaxf(3.0f, fminf(maxd, dr)));
    s->chorus_buf_l[wpos] += fb * wl;
    s->chorus_buf_r[wpos] += fb * wr;
    *l = dry * *l + (1.0f - dry) * wl;
    *r = dry * *r + (1.0f - dry) * wr;
}

/* One block: the session tempo once, then the audio in segments that end where an arpeggiator step or gate end
 * falls, so its notes start on the sample (one segment per block while it is off or idle). */
void synth_render(synth_engine_t *synth, int16_t *out_lr, int frames) {
    if (!synth) synth = default_synth();
    if (!out_lr || frames <= 0) return;

    if (synth->params[PARAM_MASTER_VOL] <= 0.01f) {
        synth_init(synth);
    }

    const float fs = (float)MOVE_SAMPLE_RATE;

    /* Session tempo and transport position, once per block: tempo-synced LFOs, delays and the arpeggiator follow
     * the Move */
    {
        /* The tempo, smoothed: the Set tempo glides over ~50 ms (a tempo knob step moves synced delays without
         * a jump); a get_bpm measured from MIDI clock jitters by about +-0.3 BPM block to block (seen on the
         * Move), which wobbled synced delay times, so then only real changes (> 2 BPM away) are followed within
         * ~50 ms and small wobble over ~1 s. The first block takes the value as is. */
        int exact;
        float bpm = host_tempo_bpm(&exact);
        if (synth->tempo_bpm <= 0.0f) {
            synth->tempo_bpm = bpm;
        } else {
            float tau = (exact || fabsf(bpm - synth->tempo_bpm) > 2.0f) ? 0.05f : 1.0f;
            synth->tempo_bpm += (bpm - synth->tempo_bpm) * (1.0f - expf(-(float)frames / (tau * fs)));
        }
    }
    const double beat0 = host_beat_position();
    const double beats_per_frame = (double)synth->tempo_bpm / (60.0 * (double)fs);

    arp_t *arp = &synth->arp;
    if (!arp->set.on) {
        render_segment(synth, out_lr, frames, beat0);
        return;
    }
    arp_clock(arp, beat0);
    for (int done = 0; done < frames;) {
        arp_process(arp, arp_emit_voice, synth);
        int n = arp_frames_to_event(arp, synth->tempo_bpm, fs, frames - done);
        render_segment(synth, out_lr + 2 * done, n, beat0 >= 0.0 ? beat0 + (double)done * beats_per_frame : beat0);
        arp_advance(arp, n, synth->tempo_bpm, fs);
        done += n;
    }
}

/* Renders frames at the current settings; beat is the transport position at the first frame (< 0: stopped) */
static void render_segment(synth_engine_t *synth, int16_t *out_lr, int frames, double beat) {
    const float fs = (float)MOVE_SAMPLE_RATE;
    const float lfo_slew_step = 2.0f / (LFO_SLEW_S * fs);
    const float lfo_smooth_k = 1.0f - expf(-1.0f / (LFO_SMOOTH_S * fs));
    const float vel_glide_k = 1.0f - expf(-1.0f / (VEL_GLIDE_S * fs));
    const float kill_coef = 1.0f / (KILL_RELEASE_S * fs); /* a release step: the whole fade in KILL_RELEASE_S */

#ifdef TINYK_TEMPO_LOG
    /* Diagnostic builds only (-DTINYK_TEMPO_LOG): what the host reports, logged at start and on every change.
     * host->log does blocking file I/O when /data/UserData/schwung/debug_log_on exists: never in a release. */
    {
        static int logged = 0, last_running = -2, last_clock = -9;
        static float last_raw = -1.0f;
        static float last_set = -1.0f;
        float raw = (g_host && g_host->get_bpm) ? g_host->get_bpm() : -1.0f;
        move_info_t mi;
        int mi_ok = g_move_info && g_move_info(&mi, sizeof mi) && mi.valid;
        float set_tempo = mi_ok ? mi.tempo : -1.0f;
        int running = beat >= 0.0;
        int clock = (g_host && g_host->get_clock_status) ? g_host->get_clock_status() : -1;
        if (g_host && g_host->log &&
            (!logged || fabsf(raw - last_raw) > 0.5f || fabsf(set_tempo - last_set) > 0.05f ||
             running != last_running || clock != last_clock)) {
            char msg[240];
            snprintf(msg, sizeof msg, "TinyK tempo: api %u get_bpm %s -> %.2f | move_info %s -> %.2f (ext sync %d) | using %.2f"
                     " | beat_position %s -> %.3f | clock status %d",
                     (unsigned)g_host->api_version, g_host->get_bpm ? "set" : "NULL", raw,
                     g_move_info ? (mi_ok ? "valid" : "invalid") : "NULL", set_tempo, mi_ok ? mi.midi_clock_sync : -1,
                     synth->tempo_bpm, g_host->get_beat_position ? "set" : "NULL", beat, clock);
            g_host->log(msg);
            logged = 1; last_raw = raw; last_set = set_tempo; last_running = running; last_clock = clock;
        }
    }
#endif

    /* Extract global parameters */
    float lfo1_rate_p   = synth->params[PARAM_LFO1_RATE];
    float lfo2_rate_p   = synth->params[PARAM_LFO2_RATE];
    float chorus_mix_p  = synth->params[PARAM_CHORUS_MIX];
    float delay_time_p  = synth->params[PARAM_DELAY_TIME];
    float delay_fdbk_p  = synth->params[PARAM_DELAY_FEEDBACK];
    float delay_mix_p   = synth->params[PARAM_DELAY_MIX];
    float master_vol_p  = synth->params[PARAM_MASTER_VOL];
    float pan_p         = synth->params[PARAM_PAN];

    float *params = synth->params;
    /* Synchronize voice_mode flag and params */
    if (synth->voice_mode == 1 && params[PARAM_VOICE_MODE] <= 0.5f) {
        params[PARAM_VOICE_MODE] = 1.0f;
    } else if (synth->voice_mode == 0 && params[PARAM_VOICE_MODE] > 0.5f) {
        synth->voice_mode = 1;
    }
    int is_layer_mode = (params[PARAM_VOICE_MODE] > 0.5f);

    /* Sanity fallback checks to guarantee audible defaults */
    if (master_vol_p <= 0.01f) master_vol_p = 0.8f;

    /* Timbre balance (equal-power-style): at 0.5 both timbres play at full level,
     * moving toward either end fades the other one out. */
    float bal = synth->timbre_balance;
    if (bal < 0.0f) bal = 0.0f;
    if (bal > 1.0f) bal = 1.0f;
    float tA_vol = fminf(1.0f, 2.0f * (1.0f - bal));
    float tB_vol = fminf(1.0f, 2.0f * bal);


    /* Precompute per-timbre render configurations (Zero heap allocation) */
    typedef struct {
        int osc1_wave;
        float pw;
        int osc2_wave;
        float detune_semi;
        int sync_ring_mode;
        int lfo2_rate_modulated; /* a patch aims at LFO2 FREQ */
        int xmod_on;            /* Sine cross-mod pitch envelope (sine_xmod_applies) */
        float osc_mix;
        float pw_knob;                        /* the Pulse Width knob, 0..1 */
        float noise_ratio, noise_k, noise_gain; /* the Noise oscillator's own filter: resonance = Control 2 */
        float gain_osc1, gain_osc2, gain_amp; /* the three level knobs as gains (level_curve, the wave's own level, amp^2) */
        float amp_knob;       /* the amp level knob itself, 0..1: applied once more after the distortion */
        float sub_level;
        float glide_coeff;
        float transpose_semi;
        float noise_level;
        float level;
        float pan;              /* timbre pan, -1..+1 */
        float bend;             /* the pitch bend now, semitones: the wheel over the timbre's bend range */
        float vibrato;          /* the mod wheel's vibrato now: semitones of LFO2 swing, signed */
        const float *wavetable;                 /* Vox */
        const float *const *dwgs;               /* the DWGS wave's tables, one per level */
        int dwgs_periods;                       /* periods of the note in one DWGS table; 1 for other waves */

        float cutoff_pitch;   /* the knob's corner in octaves above FLT_BASE_HZ, its top-end bend included */
        float cutoff_pitch_max; /* the knob's own top: modulation cannot push the cutoff past it */
        float flt_ratio, flt_k, flt_gain, flt_gain2; /* resonance: corner ratio, damping 1 / Q, output gain, 24LPF stage 2 gain */
        float resonance;
        filter_type_t filter_type;
        float keytrack;
        float env_int;
        float drive;
        float mod_int;
        float vel_sens;

        float dcy1_coef;
        float sustain1;
        float rel1_coef;
        float dcy2_coef;
        float sustain2;
        float rel2_coef;

        const timbre_extra_t *extra; /* LFO settings and virtual patch matrix */
        float lfo_dt[2];             /* patch LFO1/LFO2 phase increment per sample */
        float patch_amt[4];          /* patch intensity after the depth curve, -1..1 */
    } timbre_render_cfg_t;

    timbre_render_cfg_t t_cfg[2];

    for (int t = 0; t < 2; t++) {
        const float *tp = is_layer_mode ? synth->timbre_params[t] : synth->params;

        float wave1_p       = tp[PARAM_WAVE1];
        float pw_p          = tp[PARAM_PULSE_WIDTH];
        float wave2_p       = tp[PARAM_WAVE2];
        float detune_p      = tp[PARAM_DETUNE];
        float sync_ring_p   = tp[PARAM_SYNC_RING];
        float osc_mix_p     = tp[PARAM_OSC_MIX];
        float sub_level_p   = tp[PARAM_SUB_LEVEL];
        float portamento_p  = tp[PARAM_PORTAMENTO];

        float cutoff_p      = tp[PARAM_CUTOFF];
        float resonance_p   = tp[PARAM_RESONANCE];
        float filter_type_p = tp[PARAM_FILTER_TYPE];
        float keytrack_p    = tp[PARAM_KEYTRACK];
        float env_int_p     = tp[PARAM_ENV_INT];
        float drive_p       = tp[PARAM_DRIVE];
        float mod_int_p     = tp[PARAM_MOD_INT];
        float vel_sens_p    = tp[PARAM_VEL_SENS];

        float decay1_p      = tp[PARAM_DECAY1];
        float sustain1_p    = tp[PARAM_SUSTAIN1];
        float release1_p    = tp[PARAM_RELEASE1];
        float decay2_p      = tp[PARAM_DECAY2];
        float sustain2_p    = tp[PARAM_SUSTAIN2];
        float release2_p    = tp[PARAM_RELEASE2];

        const timbre_extra_t *extra = &synth->timbre_extra[is_layer_mode ? t : 0];

        t_cfg[t].osc1_wave = (int)(wave1_p * (float)(OSC1_WAVE_COUNT - 1) + 0.5f);
        t_cfg[t].pw = 0.5f + 0.5f * pw_p;
        t_cfg[t].pw_knob = pw_p;
        {
            float c2 = clamp01f(extra->osc1_ctrl[1]) * 127.0f;
            float r127 = 4.5f + c2 * (122.5f / 127.0f);
            t_cfg[t].noise_ratio = flt_table(FLT_RES, FLT_F_RATIO, FLT_POINTS, r127);
            t_cfg[t].noise_k = flt_damp(r127);
            t_cfg[t].noise_gain = powf(10.0f, flt_table(NOISE_RES, NOISE_GAIN_DB, NOISE_POINTS, c2) / 20.0f);
        }
        t_cfg[t].osc2_wave = (int)(wave2_p * (float)(OSC2_WAVE_COUNT - 1) + 0.5f);
        t_cfg[t].detune_semi = (detune_p - 0.5f) * 48.0f;
        t_cfg[t].sync_ring_mode = (int)(sync_ring_p * (float)(SYNC_RING_COUNT - 1) + 0.5f);
        t_cfg[t].transpose_semi = extra->transpose_semi;
        t_cfg[t].noise_level = extra->noise_level;
        t_cfg[t].level = extra->level;
        t_cfg[t].pan = extra->pan;
        t_cfg[t].bend = synth->bend_src * extra->bend_semi;
        {
            /* Vibrato Int, measured on the VST (LFO2 -> pitch at seven intensities and five wheel positions):
             * the swing is the wheel (CC1 / 127) times the velocity / keyboard family's pitch curve, a quarter
             * of a semitone per step to 12 at 48 and on to 24 at 63 (+5, the usual setting: 1.24 semitones
             * each way at full wheel); a negative value turns the LFO over. */
            float vi = fabsf(extra->vibrato_int);
            t_cfg[t].vibrato = synth->modwheel_src * copysignf(vi <= 48.0f ? 0.25f * vi : 12.0f + (vi - 48.0f) * (12.0f / 15.0f), extra->vibrato_int);
        }

        /* Vox wavetable: built on first use */
        t_cfg[t].wavetable = synth->wavetable[t];
        if (t_cfg[t].osc1_wave == OSC1_WAVE_VOX && synth->wavetable_key[t] != OSC1_WAVE_VOX) {
            build_wavetable(synth->wavetable[t]);
            synth->wavetable_key[t] = OSC1_WAVE_VOX;
        }
        t_cfg[t].dwgs = g_dwgs_table[dwgs_index(extra)];
        t_cfg[t].dwgs_periods = (t_cfg[t].osc1_wave == OSC1_WAVE_DWGS) ? DWGS_WAVES[dwgs_index(extra)].periods : 1;
        t_cfg[t].osc_mix = osc_mix_p;
        {
            /* The Osc Mix and Level knobs are the page's view of the three level knobs a program stores (Osc 1,
             * Osc 2, Amp). A turn of either is folded back into them: Mix re-splits the two oscillators under
             * the louder one's level, Level sets the amp knob. */
            timbre_extra_t *lv = &synth->timbre_extra[t];
            float loud = fmaxf(lv->lvl_osc1, lv->lvl_osc2);
            if (isnan(lv->mix_seen)) lv->mix_seen = osc_mix_p;
            if (isnan(lv->level_seen)) lv->level_seen = lv->level;
            if (fabsf(osc_mix_p - lv->mix_seen) > 1e-6f) {
                if (loud < 0.01f) loud = 1.0f;
                lv->lvl_osc1 = loud * fminf(1.0f, 2.0f * (1.0f - osc_mix_p));
                lv->lvl_osc2 = loud * fminf(1.0f, 2.0f * osc_mix_p);
                lv->mix_seen = osc_mix_p;
            }
            if (fabsf(lv->level - lv->level_seen) > 1e-6f) {
                lv->lvl_amp = clamp01f(lv->level / fmaxf(loud, 0.01f));
                lv->level_seen = lv->level;
            }
            t_cfg[t].gain_osc1 = level_curve(lv->lvl_osc1) * osc_wave_gain(t_cfg[t].osc1_wave == OSC1_WAVE_SQUARE, t_cfg[t].osc1_wave == OSC1_WAVE_TRIANGLE);
            t_cfg[t].gain_osc2 = level_curve(lv->lvl_osc2) * osc_wave_gain(t_cfg[t].osc2_wave == OSC2_WAVE_SQUARE, t_cfg[t].osc2_wave == OSC2_WAVE_TRIANGLE);
            t_cfg[t].gain_amp = lv->lvl_amp * lv->lvl_amp;
            t_cfg[t].amp_knob = lv->lvl_amp;
        }
        t_cfg[t].sub_level = sub_level_p;

        t_cfg[t].glide_coeff = 1.0f;
        if (portamento_p > 0.005f) {
            float glide_time = 0.005f * powf(400.0f, portamento_p);
            t_cfg[t].glide_coeff = 1.0f - expf(-1.0f / (glide_time * fs));
        }

        t_cfg[t].resonance = resonance_p;
        t_cfg[t].filter_type = (filter_type_t)(int)(filter_type_p * (float)(FILTER_TYPE_COUNT - 1) + 0.5f);
        /* BPF12's centre follows the knob on its own curve (fitted on VST takes at cutoff 32/64/96, res 63) */
        /* The knob's coefficient (knob_fc: exponential, then a straight line, then flat from 110); modulation
         * then moves that corner in plain octaves, up to the knob's own top (measured: an EG or key track
         * from a high knob setting moves the corner by its full depth, 1.45 octaves for EG -10 from knob 100,
         * where bending the sum gave 0.79). */
        {
            float f0 = 2.0f * (float)M_PI * FLT_BASE_HZ / fs;
            t_cfg[t].cutoff_pitch_max = log2f(knob_fc(127.0f, fs) / f0);
            t_cfg[t].cutoff_pitch = log2f(knob_fc(clamp01f(cutoff_p) * 127.0f, fs) / f0);
        }
        {
            float r127 = clamp01f(resonance_p) * 127.0f;
            t_cfg[t].flt_ratio = flt_table(FLT_RES, FLT_F_RATIO, FLT_POINTS, r127);
            t_cfg[t].flt_k = flt_damp(r127);
            t_cfg[t].flt_gain = powf(10.0f, flt_table(FLT_RES, FLT_GAIN_DB, FLT_POINTS, r127) / 20.0f);
            t_cfg[t].flt_gain2 = powf(10.0f, flt_table(FLT_RES, FLT_STAGE2_DB, FLT_POINTS, r127) / 20.0f);
        }
        t_cfg[t].keytrack = keytrack_p;
        t_cfg[t].env_int = env_int_p;
        t_cfg[t].drive = drive_p;
        t_cfg[t].mod_int = mod_int_p;
        t_cfg[t].vel_sens = vel_sens_p;

        t_cfg[t].dcy1_coef = time_to_coeff(decay1_p, EG_DECAY_MIN_S * tinyk_tuning.decay_scale, EG_DECAY_MAX_S * tinyk_tuning.decay_scale, fs);
        t_cfg[t].sustain1 = sustain1_p;
        t_cfg[t].rel1_coef = time_to_coeff(release1_p, EG_DECAY_MIN_S * tinyk_tuning.release_scale, EG_DECAY_MAX_S * tinyk_tuning.release_scale, fs);
        t_cfg[t].dcy2_coef = time_to_coeff(decay2_p, EG_DECAY_MIN_S * tinyk_tuning.decay_scale, EG_DECAY_MAX_S * tinyk_tuning.decay_scale, fs);
        t_cfg[t].sustain2 = sustain2_p;
        t_cfg[t].rel2_coef = time_to_coeff(release2_p, EG_DECAY_MIN_S * tinyk_tuning.release_scale, EG_DECAY_MAX_S * tinyk_tuning.release_scale, fs);

        t_cfg[t].extra = extra;
        for (int p = 0; p < 4; p++) {
            /* Patch depth against intensity, measured on the VST (2026-10). The curve depends on the SOURCE's
             * family as well as on the destination; each amount below scales the destination's range where it
             * is used (pitch x 24 semitones, cutoff x FLT_PATCH_OCTAVES, pan on -1..+1, Ctrl 1 on the knob,
             * amp as gain = (1 + amount x source)^2 with the bracket held to 0..2). u = |int| / 63.
             *   EG1, EG2, LFO1, LFO2 (a square LFO at seven intensities; the EGs checked on pitch):
             *     pitch, osc 2 tune: 0.04 / 0.20 / 1 / 3 / 5 / 12 / 24 semitones at 4 / 8 / 16 / 24 / 32 / 48 / 63
             *     cutoff: 7.83 octaves x u^2 (24 knob steps at 32);  amp: u^2;  pan: 2 u^2 (hard over from 45)
             *     Ctrl 1, noise: u^2, by analogy, not measured
             *   Velocity, keyboard (and, unmeasured, pitch bend and mod wheel):
             *     pitch, osc 2 tune: straight to 12 semitones at 48, then straight to 24 at 63
             *     cutoff: 2.94 octaves x (0.596 u + 0.404 u^2);  amp: 1.955 x the pitch curve / 24
             *     pan: 1.183 u + 0.801 u^2;  Ctrl 1: 0.596 u + 0.404 u^2;  noise: the pitch curve, roughly
             * LFO2 FREQ: the intensity moves LFO2's rate KNOB, one step per unit at a source of 1 (see the LFO
             * loop in the sample loop). (The old single u^2.4 was close to the EG / LFO family
             * for cutoff, and wrong for pitch: +16 gave 0.9 semitones for either family's 1 or 4.) */
            static const float PITCH_EG_LFO_I[8] = { 0, 4, 8, 16, 24, 32, 48, 63 };
            static const float PITCH_EG_LFO_ST[8] = { 0.0f, 0.043f, 0.199f, 1.0f, 3.0f, 5.0f, 12.0f, 24.0f };
            float a = extra->patch_int[p], u = fabsf(a), i63 = u * 63.0f, amt;
            int slow = extra->patch_src[p] <= PATCH_SRC_LFO2; /* the EG / LFO family */
            float lin = i63 <= 48.0f ? i63 / 96.0f : 0.5f + (i63 - 48.0f) / 30.0f;
            float mix = 0.596f * u + 0.404f * u * u;
            switch (extra->patch_dst[p]) {
                case PATCH_DST_PITCH: case PATCH_DST_OSC2_PITCH:
                    amt = slow ? flt_table(PITCH_EG_LFO_I, PITCH_EG_LFO_ST, 8, i63) / 24.0f : lin;
                    break;
                case PATCH_DST_CUTOFF:
                    amt = slow ? u * u : (2.94f / FLT_PATCH_OCTAVES) * mix;
                    break;
                case PATCH_DST_AMP:
                    amt = slow ? u * u : 1.955f * lin;
                    break;
                case PATCH_DST_PAN:
                    amt = slow ? 2.0f * u * u : 1.183f * u + 0.801f * u * u;
                    break;
                case PATCH_DST_OSC1_CTRL1:
                    amt = slow ? u * u : mix;
                    break;
                case PATCH_DST_NOISE:
                    amt = slow ? u * u : lin;
                    break;
                default:
                    amt = powf(u, tinyk_tuning.patch_int_curve);
                    break;
            }
            t_cfg[t].patch_amt[p] = copysignf(amt, a);
        }
        int lfo2_rate_modulated = 0;
        for (int p = 0; p < 4; p++) {
            if (extra->patch_dst[p] == PATCH_DST_LFO2_FREQ && extra->patch_int[p] != 0.0f) lfo2_rate_modulated = 1;
        }
        t_cfg[t].lfo2_rate_modulated = lfo2_rate_modulated;
        for (int l = 0; l < 2; l++) {
            float hz;
            if (extra->lfo_sync_note[l] >= 0) {
                /* tempo sync: period = note length (fraction of a whole note) at the session tempo */
                float note = LFO_SYNC_NOTES[extra->lfo_sync_note[l]];
                hz = synth->tempo_bpm / (240.0f * note);
                /* Free-running (no key sync) synced LFOs also lock their phase to the transport while it runs,
                 * cycle start on beat 0; key-synced ones restart on each note, as on the hardware. A patch into
                 * LFO2 FREQ does nothing to a synced LFO2 (measured on the VST: the same rate at every
                 * intensity, four time bases), so it locks like any other. */
                if (beat >= 0.0 && extra->lfo_keysync[l] == 0) {
                    double cycles = beat / (4.0 * (double)note);
                    synth->patch_lfo[t][l].phase = (float)(cycles - floor(cycles));
                }
            } else {
                hz = rate_table(LFO_RATE_HZ, extra->lfo_rate[l]);
            }
            t_cfg[t].lfo_dt[l] = hz / fs;
        }
    }

    /* LFO frequencies: the measured knob curve (LFO_RATE_HZ) */
    float lfo1_freq = rate_table(LFO_RATE_HZ, lfo1_rate_p);
    float lfo2_freq = rate_table(LFO_RATE_HZ, lfo2_rate_p);
    float lfo1_dt = lfo1_freq / fs;
    float lfo2_dt = lfo2_freq / fs;

    /* Delay config (delay_time_s, delay_gain: measured on the VST). The hardware has one delay depth that sets
     * both the repeats and their level; depth 0 means the delay is off. */
    float target_delay_samples;
    if (synth->delay_sync_note >= 0) {
        /* tempo-synced: the Delay Time knob steps the time base (1/32 .. 1/1) at the session tempo */
        int base = (int)lroundf(clamp01f(delay_time_p) * 14.0f);
        target_delay_samples = 240.0f / synth->tempo_bpm * DELAY_SYNC_NOTES[base] * fs;
    } else {
        target_delay_samples = delay_time_s(delay_time_p) * fs;
    }
    /* a whole number of samples: reading between samples would low-pass every repeat a little more */
    target_delay_samples = floorf(fmaxf(100.0f, fminf((float)(DELAY_BUFFER_SIZE - 2), target_delay_samples)) + 0.5f);
    float delay_feedback = delay_gain(delay_fdbk_p, target_delay_samples / fs);
    float delay_send = delay_gain(delay_mix_p, target_delay_samples / fs);
    eq_update(synth, fs);
    int delay_on = (delay_mix_p > 0.005f);

    /* The low-pass types' fixed one-pole low-pass: y += a (x - y) */
    float lp_stage_w = 2.0f * (float)M_PI * FLT_LP_STAGE_HZ / fs;
    /* Brightness tilt coefficients: bilinear first-order high shelf H(s) = (G s + 1) / (s + 1), prewarped at
     * tilt_hz. Unity at DC, G toward Nyquist. */
    float tilt_b0, tilt_b1, tilt_a1;
    {
        float G = powf(10.0f, tinyk_tuning.tilt_db / 20.0f);
        float K = tanf((float)M_PI * fminf(tinyk_tuning.tilt_hz, 0.49f * fs) / fs);
        tilt_b0 = (G + K) / (1.0f + K);
        tilt_b1 = (K - G) / (1.0f + K);
        tilt_a1 = (K - 1.0f) / (1.0f + K);
    }
    /* The audible noise's pre-shaping: the inverse of the tilt, then the plug-in's own roll-off (see heard_noise) */
    float noise_b0, noise_b1, noise_a1;
    float noise_lp_a = 1.0f - expf(-2.0f * (float)M_PI * NOISE_LP_HZ / fs);
    {
        float G = powf(10.0f, tinyk_tuning.tilt_db / 20.0f);
        float K = tanf((float)M_PI * fminf(tinyk_tuning.tilt_hz, 0.49f * fs) / fs);
        noise_b0 = (G + K) / (1.0f + K);
        noise_b1 = (K - G) / (1.0f + K);
        noise_a1 = (K - 1.0f) / (1.0f + K);
    }

    /* Pan: Left / Right gains */
    float pan_l = cosf(pan_p * (float)(M_PI * 0.5)) * 1.4142f;
    float pan_r = sinf(pan_p * (float)(M_PI * 0.5)) * 1.4142f;

    for (int s = 0; s < frames; s++) {
        /* 1. Update LFO 1 */
        synth->lfo1.phase += lfo1_dt;
        if (synth->lfo1.phase >= 1.0f) {
            synth->lfo1.phase -= 1.0f;
        }
        float lfo1_val = 2.0f * fabsf(2.0f * synth->lfo1.phase - 1.0f) - 1.0f;

        /* 2. Update LFO 2 */
        synth->lfo2.phase += lfo2_dt;
        if (synth->lfo2.phase >= 1.0f) {
            synth->lfo2.phase -= 1.0f;
        }
        float lfo2_val = sinf(2.0f * (float)M_PI * synth->lfo2.phase);

        /* White noise source shared by all voices (xorshift32, no heap, no libc rand) */
        uint32_t noise_state = synth->noise_state;
        noise_state ^= noise_state << 13;
        noise_state ^= noise_state >> 17;
        noise_state ^= noise_state << 5;
        synth->noise_state = noise_state;
        float white_noise = (float)(int32_t)noise_state * (1.0f / 2147483648.0f);
        /* The noise you hear. The brightness tilt further down models the plug-in's oscillators and its noise has
         * none of it, so the noise first goes through the tilt's inverse, (1 + a1 z^-1) / (b0 + b1 z^-1) (stable:
         * the shelf's zero is inside the unit circle). The plug-in's noise is not white either: flat to 3 kHz,
         * then -1, -2, -6, -9 dB at 4.8, 6.8, 9.6, 13.6 kHz, which a one-pole low-pass at NOISE_LP_HZ follows
         * within 2 dB. S&H keeps the raw source. */
        float flat_noise = (white_noise + noise_a1 * synth->noise_x1) / noise_b0 - (noise_b1 / noise_b0) * synth->noise_y1;
        synth->noise_x1 = white_noise;
        synth->noise_y1 = flat_noise;
        synth->noise_lp += noise_lp_a * (flat_noise - synth->noise_lp);
        float heard_noise = synth->noise_lp;

        /* Patch-matrix LFOs, per timbre. LFO2's rate can itself be a patch destination. Measured on the VST
         * (a constant source of 1 into LFO2 FREQ, the vibrato rate read off a sine, 17 settings from three knob
         * positions): the patch moves the rate KNOB by intensity x source, one knob step per unit, to 1 %. The
         * LFOs here are one per timbre, so a per-voice source (an EG, velocity, the keyboard) is taken from the
         * timbre's newest voice. A tempo-synced LFO2 keeps its synced rate whatever the patch says (measured). */
        float plfo[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        for (int t = 0; t < (is_layer_mode ? 2 : 1); t++) {
            const timbre_extra_t *ex = t_cfg[t].extra;
            float lfo2_dt = t_cfg[t].lfo_dt[1];
            if (t_cfg[t].lfo2_rate_modulated && ex->lfo_sync_note[1] < 0) {
                const voice_t *lead = NULL;
                for (int vi = 0; vi < NUM_VOICES; vi++) {
                    const voice_t *c = &synth->voices[vi];
                    if (c->active && c->timbre_index == t && (!lead || c->age >= lead->age)) lead = c;
                }
                float knob_shift = 0.0f;
                for (int p = 0; p < 4; p++) {
                    if (ex->patch_dst[p] != PATCH_DST_LFO2_FREQ || ex->patch_int[p] == 0.0f) continue;
                    float sv = 0.0f;
                    switch (ex->patch_src[p]) {
                        case PATCH_SRC_EG1:        sv = lead ? lead->filter_env.value : 0.0f; break;
                        case PATCH_SRC_EG2:        sv = lead ? lead->amp_env.value : 0.0f; break;
                        case PATCH_SRC_LFO1:       sv = synth->patch_lfo[t][0].out; break;
                        case PATCH_SRC_VELOCITY:   sv = lead ? lead->velocity : 0.0f; break;
                        case PATCH_SRC_KBD:
                            sv = lead ? ((float)lead->note + (float)(synth->octave_transpose * 12) + t_cfg[t].transpose_semi - 60.0f) / 43.0f : 0.0f;
                            break;
                        case PATCH_SRC_PITCH_BEND: sv = synth->bend_src; break;
                        case PATCH_SRC_MOD_WHEEL:  sv = synth->modwheel_src; break;
                        default: break;
                    }
                    knob_shift += ex->patch_int[p] * (63.0f / 127.0f) * sv;
                }
                lfo2_dt = rate_table(LFO_RATE_HZ, ex->lfo_rate[1] + knob_shift) / fs;
            }
            for (int l = 0; l < 2; l++) {
                lfo_t *lf = &synth->patch_lfo[t][l];
                lf->phase += l == 1 ? lfo2_dt : t_cfg[t].lfo_dt[0];
                if (lf->phase >= 1.0f) {
                    lf->phase -= floorf(lf->phase);
                    lf->sh_value = white_noise; /* sample & hold from the deterministic noise source */
                }
                /* Slew-limited: a full -1 -> +1 swing takes at least LFO_SLEW_S, so the saw's wrap, square
                 * edges, S&H steps and key-sync restarts are short ramps instead of one-sample jumps (an
                 * LFO -> amp / pan route turned each into a click: A.21's gate, A.31's S&H pan). Continuous
                 * shapes are far slower than the limit and pass unchanged. */
                float d = patch_lfo_value(lf, l, ex->lfo_wave[l]) - lf->lim;
                lf->lim += fmaxf(-lfo_slew_step, fminf(lfo_slew_step, d));
                lf->out += (lf->lim - lf->out) * lfo_smooth_k;
                plfo[t][l] = lf->out;
            }
        }

        /* 3. Render Voices */
        float voice_sum_l = 0.0f;
        float voice_sum_r = 0.0f;

        for (int v_idx = 0; v_idx < NUM_VOICES; v_idx++) {
            voice_t *v = &synth->voices[v_idx];
            if (!v->active) continue;

            int t_idx = is_layer_mode ? v->is_timbre_2 : 0;
            const timbre_render_cfg_t *cfg = &t_cfg[t_idx];

            /* Virtual patch matrix: per-destination sum of intensity x source, except amp: each amp route is
             * its own gain stage, 1 + intensity x source (floored at 0), and their product is squared, then capped
             * at full level (1 / the timbre's level). That is A.21's off-beat hat on the VST (two LFO1 saw -> amp
             * routes, +48 and +63): it decays as the squared product (3.9 dB rms over the hat, the sum was
             * 11.5: it went silent at 2/3 of the cycle), reaches silence exactly at the cycle's end, and the
             * kick half stays level (ref_a21_timbre2_drum_c3). EG sources use the envelope value from the
             * previous sample (the envelopes are advanced further down). */
            float pmod[PATCH_DST_COUNT] = { 0.0f };
            float amp_gain = 1.0f;
            /* The note every keyboard follower sees (the KBD patch source, filter key track, the Noise
             * oscillator's cutoff): the played key moved by the timbre's transpose. Measured on the VST: with
             * transpose +12 the tracked filter and the Noise oscillator sit an octave up, and KBD -> pitch -48
             * holds C4 at every transpose. */
            float kbd_note = (float)v->note + (float)(synth->octave_transpose * 12) + cfg->transpose_semi;
            for (int p = 0; p < 4; p++) {
                float amt = cfg->patch_amt[p];
                if (amt == 0.0f) continue;
                float sv;
                switch (cfg->extra->patch_src[p]) {
                    case PATCH_SRC_EG1:      sv = v->filter_env.value; break;
                    case PATCH_SRC_EG2:      sv = v->amp_env.value; break;
                    case PATCH_SRC_LFO1:     sv = plfo[t_idx][0]; break;
                    case PATCH_SRC_LFO2:     sv = plfo[t_idx][1]; break;
                    /* Velocity, measured: 0..1 for pitch, pan and Ctrl 1, but for cutoff and amp it is zero at
                     * velocity 80 and runs to +1 at 127 (-0.34 at 64, -1.68 at 1). Keyboard: one unit per octave
                     * from C4 on a pitch route (+32 gives exactly 8 semitones per octave), a quarter of that on
                     * a cutoff route; other destinations are assumed like cutoff. */
                    case PATCH_SRC_VELOCITY: {
                        int d = cfg->extra->patch_dst[p];
                        sv = (d == PATCH_DST_CUTOFF || d == PATCH_DST_AMP) ? (v->velocity * 127.0f - 80.0f) / 47.0f : v->velocity;
                        break;
                    }
                    case PATCH_SRC_KBD: {
                        int d = cfg->extra->patch_dst[p];
                        sv = (kbd_note - 60.0f) / ((d == PATCH_DST_PITCH || d == PATCH_DST_OSC2_PITCH) ? 12.0f : 43.0f);
                        break;
                    }
                    case PATCH_SRC_PITCH_BEND: sv = synth->bend_src; break;
                    case PATCH_SRC_MOD_WHEEL:  sv = synth->modwheel_src; break;
                    default:                 sv = 0.0f; break;
                }
                if (cfg->extra->patch_dst[p] == PATCH_DST_AMP) amp_gain *= fmaxf(0.0f, 1.0f + amt * sv);
                else pmod[cfg->extra->patch_dst[p]] += amt * sv;
            }
            amp_gain = fminf(amp_gain, 2.0f); /* the plug-in's ceiling: x 4 after squaring */
            amp_gain *= amp_gain;

            /* Portamento Pitch Glide */
            v->current_pitch += (v->target_pitch - v->current_pitch) * cfg->glide_coeff;

            /* Pitch bend over the timbre's own range (measured on the VST: straight, range x bend, a negative
             * range bends the other way), the mod wheel's vibrato (LFO2, see vibrato above), patch -> pitch */
            float pitch_mod = lfo1_val * (cfg->mod_int * 0.5f) + cfg->bend + cfg->vibrato * plfo[t_idx][1]
                            + pmod[PATCH_DST_PITCH] * tinyk_tuning.patch_pitch_scale + v->unison_cents * 0.01f;

            float final_note1 = v->current_pitch + cfg->transpose_semi + pitch_mod;
            float freq1 = note_to_freq(final_note1);
            float dt1 = freq1 / fs;
            if (dt1 > 0.45f) dt1 = 0.45f;

            float final_note2 = v->current_pitch + cfg->transpose_semi + cfg->detune_semi + pitch_mod
                              + pmod[PATCH_DST_OSC2_PITCH] * tinyk_tuning.patch_pitch_scale;
            float freq2 = note_to_freq(final_note2);
            float dt2 = freq2 / fs;
            if (dt2 > 0.45f) dt2 = 0.45f;

            float dt_sub = 0.5f * dt1;

            /* Advance Phase & Handle Hard Sync */
            bool sync_triggered = false;
            /* Osc 1 Sine, Control 1 / 2 = cross modulation: Osc 2's waveform modulates the sine's FREQUENCY,
             * linearly and through zero, by up to XMOD_MAX_HZ * depth^2 (in Hz, whatever the note), depth =
             * Control 1 + Control 2 * LFO1 + patches -> Ctrl 1 on 0..1. Measured on the VST: sidebands of a
             * 110 Hz sine against a modulator a fifth up fit to 0.1-0.5 dB for all three Osc 2 waves and the
             * deviation is 95 Hz at Control 1 = 8 at 110, 220, 440 and 880 Hz alike (24 kHz at 127). Osc 2's
             * level does not matter, only its wave and pitch. */
            float fm_dt = 0.0f;
            if (cfg->osc1_wave == OSC1_WAVE_SINE) {
                float d = cfg->extra->osc1_ctrl[0] + cfg->extra->osc1_ctrl[1] * plfo[t_idx][0] + pmod[PATCH_DST_OSC1_CTRL1];
                if (d > 0.0f) {
                    if (d > 1.0f) d = 1.0f;
                    float m = cfg->osc2_wave == OSC2_WAVE_SAW ? 1.0f - 2.0f * v->osc2_phase
                            : cfg->osc2_wave == OSC2_WAVE_SQUARE ? (v->osc2_phase < 0.5f ? 1.0f : -1.0f)
                            : 2.0f * fabsf(2.0f * v->osc2_phase - 1.0f) - 1.0f;
                    fm_dt = (XMOD_MAX_HZ / fs) * d * d * m;
                }
            }
            v->osc1_phase += dt1 + fm_dt;
            if (v->osc1_phase >= 1.0f) {
                v->osc1_phase -= floorf(v->osc1_phase);
                sync_triggered = true;
                if (++v->osc1_cycle >= cfg->dwgs_periods) v->osc1_cycle = 0;
            } else if (v->osc1_phase < 0.0f) {
                v->osc1_phase -= floorf(v->osc1_phase); /* through zero: the sine runs backwards */
            }

            v->osc2_phase += dt2;
            if ((cfg->sync_ring_mode == SYNC_RING_SYNC || cfg->sync_ring_mode == SYNC_RING_BOTH) && sync_triggered) {
                v->osc2_phase = v->osc1_phase * (dt2 / dt1);
            }
            if (v->osc2_phase >= 1.0f) {
                v->osc2_phase -= 1.0f;
            }

            v->sub_phase += dt_sub;
            if (v->sub_phase >= 1.0f) {
                v->sub_phase -= 1.0f;
            }

            /* --- Oscillator 1 Signal Generation --- */
            float osc1_out = 0.0f;
            /* Control 1 as the wave sees it: the knob (Pulse Width on the page for the pulse), Control 2 x LFO1, patches */
            float ctrl = (cfg->osc1_wave == OSC1_WAVE_SQUARE ? cfg->pw_knob : cfg->extra->osc1_ctrl[0])
                       + cfg->extra->osc1_ctrl[1] * plfo[t_idx][0] + pmod[PATCH_DST_OSC1_CTRL1];
            ctrl = ctrl < 0.0f ? 0.0f : (ctrl > 1.0f ? 1.0f : ctrl);
            switch (cfg->osc1_wave) {
                /* Control 1 (+ Control 2 x LFO1 + patches -> Ctrl 1) on Saw, Pulse and Triangle, measured on the VST
                 * through the harmonics of a 55 Hz note at five settings each (ctrl below is that sum, 0..1). */
                case OSC1_WAVE_SAW: {
                    /* Two saws, the second moved by ctrl / 2 of a cycle and each at half level: every harmonic
                     * from the second up follows 0.637 |cos(pi k ctrl / 2)| / k to 0.01-0.04 (a saw an octave
                     * up at full). Osc 1's saw alone also carries an extra fundamental of 18.4 / f Hz of a
                     * full-level sine (0.335 at 55 Hz, 0.173 at 110), the same at every Control 1. */
                    float ph2 = fmod_pos(v->osc1_phase + 0.5f * ctrl);
                    osc1_out = 0.5f * ((2.0f * v->osc1_phase - 1.0f) - poly_blep(v->osc1_phase, dt1)
                                     + (2.0f * ph2 - 1.0f) - poly_blep(ph2, dt1))
                             - fminf(0.5f, SAW1_FUND_HZ / fmaxf(freq1, 20.0f)) * sinf(2.0f * (float)M_PI * v->osc1_phase);
                    break;
                }
                case OSC1_WAVE_SQUARE: {
                    /* duty 0.5 + 0.5 ctrl: 0.563 at 16, 0.753 at 64, 0.942 at 112, silent at 127 */
                    float pw = 0.5f + 0.5f * ctrl;
                    if (pw > 0.999f) pw = 0.999f;
                    /* half a period on from the phase wrap: at the wrap, where sync restarts Osc 2 (upward
                     * from zero), the plug-in's pulse is in its low half (a synced Osc 2 square cancels it) */
                    float sq = fmod_pos(v->osc1_phase + 0.5f);
                    float raw = (sq < pw) ? 1.0f : -1.0f;
                    osc1_out = raw + poly_blep(sq, dt1) - poly_blep(fmod_pos(sq - pw), dt1);
                    osc1_out -= 2.0f * pw - 1.0f; /* remove the duty-cycle DC offset */
                    break;
                }
                case OSC1_WAVE_TRIANGLE: {
                    /* the triangle driven 1 + 2 ctrl times too hard and folded back on itself: at full it is a
                     * triangle three harmonics up (the plug-in: harmonics 1, 3, 9 = 0, 1.105, 0.132) */
                    float tri = 2.0f * fabsf(2.0f * v->osc1_phase - 1.0f) - 1.0f;
                    float w = fmod_pos(((1.0f + 2.0f * ctrl) * tri + 1.0f) * 0.25f);
                    osc1_out = 1.0f - 4.0f * fabsf(w - 0.5f);
                    break;
                }
                case OSC1_WAVE_SINE:
                    /* at its positive peak at the phase wrap (measured against a synced Osc 2 square: their
                     * ring product is a half-cosine twice per period) */
                    osc1_out = cosf(2.0f * (float)M_PI * v->osc1_phase);
                    break;
                case OSC1_WAVE_DWGS: {
                    /* the richest level whose top harmonic is under Nyquist at this pitch */
                    int level = 0;
                    float top = dt1 * (float)(2 * DWGS_HARMONICS);
                    while (top > 1.0f && level < DWGS_LEVELS - 1) { top *= 0.5f; level++; }
                    int cycle = v->osc1_cycle < cfg->dwgs_periods ? v->osc1_cycle : 0;
                    float pos = ((float)cycle + v->osc1_phase) * (float)dwgs_level_size(level);
                    int i0 = (int)pos, last = dwgs_level_size(level) * cfg->dwgs_periods - 1;
                    if (i0 > last) i0 = last;
                    const float *tab = cfg->dwgs[level] + i0;
                    float x = pos - (float)i0;
                    float c1 = 0.5f * (tab[1] - tab[-1]);
                    float c2 = tab[-1] - 2.5f * tab[0] + 2.0f * tab[1] - 0.5f * tab[2];
                    float c3 = 0.5f * (tab[2] - tab[-1]) + 1.5f * (tab[0] - tab[1]);
                    osc1_out = ((c3 * x + c2) * x + c1) * x + tab[0];
                    break;
                }
                case OSC1_WAVE_VOX: {
                    /* The Vox wave, measured on the VST (tools/capture_vox.py): one fixed pulse per period of
                     * the note, the same at every pitch, so a higher note only brings the pulses closer (they
                     * overlap and add) and the formant stays where it is. Control 1 shortens the pulse (about
                     * an octave of formant per 32 steps, 300 Hz to 4.5 kHz) and reshapes it a little: nine
                     * captured pulses, each on its own time scale d, read at the scale the knob gives and
                     * cross-faded. The pulse train's mean is taken off (the plug-in's output has none). */
                    float c127 = (ctrl < 0.0f ? 0.0f : (ctrl > 1.0f ? 1.0f : ctrl)) * 127.0f;
                    int seg = 0;
                    while (seg < VOX_POINTS - 2 && c127 > VOX_C1[seg + 1]) seg++;
                    float w = (c127 - VOX_C1[seg]) / (VOX_C1[seg + 1] - VOX_C1[seg]);
                    float d = VOX_D_S[seg] + (VOX_D_S[seg + 1] - VOX_D_S[seg]) * w;
                    float period = (float)VOX_PER_D / (fmaxf(freq1, 8.0f) * d);     /* in table samples */
                    const short *pa = VOX_PULSE[seg], *pb = VOX_PULSE[seg + 1];
                    float x = v->osc1_phase * period, sum = 0.0f;
                    /* At most VOX_MAX_PULSES sound at once. Where a long pulse on a high note would need more
                     * (Control 1 under 32 from about E5 up), its tail is faded out over the last eight instead
                     * of being cut: the cut put a step in every period, 15-20 dB of hash above 12 kHz. */
                    int whole = period * (float)VOX_MAX_PULSES >= (float)(VOX_LEN - 1) ? VOX_MAX_PULSES : VOX_MAX_PULSES - 8;
                    int k = 0;
                    for (; k < whole && x < (float)(VOX_LEN - 1); k++, x += period) {
                        int i = (int)x;
                        float f = x - (float)i;
                        float a = (float)pa[i] + ((float)pa[i + 1] - (float)pa[i]) * f;
                        float b2 = (float)pb[i] + ((float)pb[i + 1] - (float)pb[i]) * f;
                        sum += a + (b2 - a) * w;
                    }
                    for (; k < VOX_MAX_PULSES && x < (float)(VOX_LEN - 1); k++, x += period) {
                        int i = (int)x;
                        float f = x - (float)i;
                        float a = (float)pa[i] + ((float)pa[i + 1] - (float)pa[i]) * f;
                        float b2 = (float)pb[i] + ((float)pb[i + 1] - (float)pb[i]) * f;
                        float u = ((float)(VOX_MAX_PULSES - k) - v->osc1_phase) * 0.125f; /* 1 .. 0 over the last eight periods */
                        sum += (a + (b2 - a) * w) * u * u * (3.0f - 2.0f * u);
                    }
                    osc1_out = sum * VOX_SCALE
                             - (VOX_AREA[seg] + (VOX_AREA[seg + 1] - VOX_AREA[seg]) * w) * (float)VOX_PER_D / period;
                    break;
                }
                case OSC1_WAVE_NOISE: {
                    /* The Noise oscillator, measured on the VST: noise through its own resonant 12 dB low-pass,
                     * a core like the main filter's. Control 1 is its cutoff on the main filter's knob scale,
                     * following the keyboard one octave per octave from C4 (peak 657 / 1303 / 2589 / 4953 Hz
                     * at notes 36 / 48 / 60 / 72); Control 2 is its resonance (see NOISE_GAIN_DB). Control 2
                     * is not an LFO depth on this wave. */
                    float nk = cfg->extra->osc1_ctrl[0] + pmod[PATCH_DST_OSC1_CTRL1];
                    nk = nk < 0.0f ? 0.0f : (nk > 1.0f ? 1.0f : nk);
                    float nfn = (2.0f * (float)M_PI * FLT_BASE_HZ / fs)
                              * exp2f(fminf(FLT_KNOB_OCTAVES + 2.0f, nk * FLT_KNOB_OCTAVES + (kbd_note - 60.0f) / 12.0f));
                    float nfc = nfn / sqrtf(1.0f + (nfn / FLT_F_BEND) * (nfn / FLT_F_BEND));
                    float nf = fminf(FLT_F_MAX, nfc * (1.0f + (cfg->noise_ratio - 1.0f) * (1.0f + 0.12f * nfc * nfc)));
                    float nhp, nbp, nlp;
                    svf_core(&v->noise_svf, NOISE_OSC_GAIN * heard_noise, nf, cfg->noise_k, &nhp, &nbp, &nlp);
                    osc1_out = cfg->noise_gain * nlp;
                    break;
                }
            }

            /* --- Oscillator 2 Signal Generation --- */
            /* Every Osc 2 wave is at its rising zero crossing at phase 0, which is where sync restarts it.
             * Measured on the VST (one period of Osc 1 saw + synced Osc 2, both waves in view): the synced saw
             * wraps half a period after Osc 1's, the square goes high at Osc 1's wrap, the triangle starts
             * upward from zero. Restarting the saw at its wrap and the triangle at its peak, as before, put
             * the mix and the ring product of a synced pair 6-13 dB off. */
            float osc2_out = 0.0f;
            switch (cfg->osc2_wave) {
                case OSC2_WAVE_SAW: {
                    float ph = fmod_pos(v->osc2_phase + 0.5f);
                    osc2_out = (2.0f * ph - 1.0f) - poly_blep(ph, dt2);
                    break;
                }
                case OSC2_WAVE_SQUARE: {
                    float raw = (v->osc2_phase < 0.5f) ? 1.0f : -1.0f;
                    osc2_out = raw + poly_blep(v->osc2_phase, dt2) - poly_blep(fmod_pos(v->osc2_phase - 0.5f), dt2);
                    break;
                }
                case OSC2_WAVE_TRIANGLE:
                    osc2_out = 2.0f * fabsf(2.0f * fmod_pos(v->osc2_phase + 0.75f) - 1.0f) - 1.0f;
                    break;
            }

            /* the top-octave roll-off (osc_rolloff): every wave with harmonics; the sine keeps its level on the
             * plug-in, and the Noise oscillator has no pitch */
            if (dt1 >= OSC_ROLLOFF_MIN_DT || dt2 >= OSC_ROLLOFF_MIN_DT) {
                if (!v->osc_lp_on) {
                    v->osc_lp[0] = osc1_out;
                    v->osc_lp[1] = osc2_out;
                    v->osc_lp_on = 1;
                }
                if (cfg->osc1_wave != OSC1_WAVE_SINE && cfg->osc1_wave != OSC1_WAVE_NOISE) {
                    osc1_out = osc_rolloff(&v->osc_lp[0], osc1_out, dt1);
                }
                osc2_out = osc_rolloff(&v->osc_lp[1], osc2_out, dt2);
            } else if (v->osc_lp_on) {
                v->osc_lp_on = 0;
            }

            /* Ring Modulation: Osc 1 x Osc 2 in Osc 2's place. Its level against the plain Osc 2 depends on
             * Osc 1's wave (pairs at inharmonic intervals on the VST: saw and square 1.11, sine 2.27, triangle 1.5,
             * each within 0.3 dB over two or three Osc 2 waves; the other
             * waves are not measured) */
            float osc2_final = osc2_out;
            if (cfg->sync_ring_mode == SYNC_RING_RING || cfg->sync_ring_mode == SYNC_RING_BOTH) {
                float k = cfg->osc1_wave == OSC1_WAVE_SAW || cfg->osc1_wave == OSC1_WAVE_SQUARE ? 1.11f
                        : cfg->osc1_wave == OSC1_WAVE_SINE ? 2.27f : cfg->osc1_wave == OSC1_WAVE_TRIANGLE ? 1.5f : 1.2f;
                osc2_final = osc1_out * osc2_out * k;
            }

            /* Sub-oscillator: square wave 1 octave down */
            float sub_out = (v->sub_phase < 0.5f) ? 1.0f : -1.0f;

            /* Mixer, as the plug-in's: each source at its own level knob (level_curve), simply added. A level
             * of 1.0 is a full-level sine's amplitude: the filter, and above all the distortion's clip point,
             * are fixed against that. The amp level comes after the distortion. */
            float osc_sum = cfg->gain_osc1 * osc1_out + cfg->gain_osc2 * osc2_final
                          + cfg->sub_level * sub_out
                          + level_curve(fmaxf(0.0f, fminf(1.0f, cfg->noise_level + pmod[PATCH_DST_NOISE]))) * NOISE_MIX_GAIN * heard_noise;

            /* --- Envelopes (Exponential Curves) --- */
            float f_env = adsr_process(&v->filter_env, cfg->dcy1_coef, cfg->sustain1, cfg->rel1_coef);
            float a_env = adsr_process(&v->amp_env, cfg->dcy2_coef, cfg->sustain2, v->kill ? kill_coef : cfg->rel2_coef);

            /* Clean voice deactivation when envelope finishes or drops to zero while released */
            if (v->amp_env.stage == ENV_IDLE || (!v->gate && a_env <= 0.0005f)) {
                v->active = false;
                v->gate = false;
                v->kill = false;
                v->amp_env.stage = ENV_IDLE;
                v->amp_env.value = 0.0f;
                v->filter_env.stage = ENV_IDLE;
                v->filter_env.value = 0.0f;
                v->filter_svf[0].s1 = 0.0f;
                v->filter_svf[0].s2 = 0.0f;
                v->filter_svf[1].s1 = 0.0f;
                v->filter_svf[1].s2 = 0.0f;
                continue;
            }

            /* --- Filter Processing --- */
            /* All modulation is summed as control pitch (octaves above cutoff_base_hz), then converted
             * to Hz once. Key tracking is bipolar around 0.5 (0.5 = none, 1.0 = KEYTRACK_SLOPE octaves per octave about KEYTRACK_PIVOT). */
            /* Key tracking, measured on the VST: octaves of cutoff per octave played = track / 48 up to +-48
             * (0.33 at 16, 0.67 at 32), then on to 2 at +-63 (4.00 octaves over two at 63). The pivot is C4 (the corner does not move there). */
            float kt63 = (cfg->keytrack * 2.0f - 1.0f) * 63.0f, kta = fabsf(kt63);
            float keytrack_mod = (kbd_note - KEYTRACK_PIVOT) / 12.0f
                               * copysignf(kta <= 48.0f ? kta / 48.0f : 1.0f + (kta - 48.0f) / 15.0f, kt63);
            float lfo2_mod = cfg->mod_int * lfo2_val * 2.0f;

            /* Bipolar filter envelope: env_int 0.5 = off, > 0.5 opens, < 0.5 closes on note strike */
            float bipolar_env = (cfg->env_int - 0.5f) * 2.0f;
            bipolar_env *= (1.0f - cfg->vel_sens + cfg->vel_sens * v->velocity);
            float eg_mod = bipolar_env * f_env * FLT_EG_OCTAVES;

            float cutoff_pitch = cfg->cutoff_pitch + keytrack_mod + lfo2_mod + eg_mod
                               + pmod[PATCH_DST_CUTOFF] * FLT_PATCH_OCTAVES;
            /* The knob's corner (already bent, see cutoff_pitch) moved by its modulations, stopping at the
             * knob's own top; then resonance scales it (flt_ratio). See the filter notes above svf_core. */
            float flt_oct = fmaxf(-2.0f, fminf(cfg->cutoff_pitch_max, cutoff_pitch));
            float flt_fc = (2.0f * (float)M_PI * FLT_BASE_HZ / fs) * exp2f(flt_oct);
            /* resonance's share grows a little with the coefficient itself (x 1.06 at 0.72, x 1.10 at 0.89) */
            float flt_f = flt_fc * (1.0f + (cfg->flt_ratio - 1.0f) * (1.0f + 0.12f * flt_fc * flt_fc));
            if (flt_f > FLT_F_MAX) flt_f = FLT_F_MAX;

            float filtered, hp, bp, lp;
            /* The plug-in kicks the filter at every note-on, whatever feeds it (measured with every source at 0:
             * a thump that scales with the amp level, rings at the filter's own frequency and is the same size
             * through every type once their output gains are taken off, so it enters the core's state). Its ring
             * is about 0.3 x Q of a saw's peak at the low-pass output up to Q 20, a random size and sign from
             * note to note (+-6 dB), and falls with the corner as exp(-f / 477 Hz) (9 cutoffs at resonance 127:
             * within 3 dB). Inaudible on most sounds; at resonance 100+ and a low cutoff it is a pinged resonator
             * (A.78 reznotes is that ping and little else). The resonance gain sits before the core, as in
             * the plug-in, so the kick is the same size at every resonance.
             * The ring starts at a random PHASE, not just either way up: the kick is shared between the band
             * and the low state (in quadrature, equal in size at these low corners). With a sign alone, the
             * rings of a unison stack were in step or exactly opposed, and through the distortion's clip
             * (which evens out their sizes) two against two cancelled outright: A.78's notes came out at
             * -8, -14 or -33 .. -48 dB by the draw, half of them the last. The plug-in's twelve takes spread
             * -29 .. -16 dB, as four rings at unrelated phases do. */
            if (v->flt_kick != 0.0f) {
                float hz = flt_f * fs * (1.0f / (2.0f * (float)M_PI));
                float kick = v->flt_kick * FLT_KICK * fminf(1.0f / fmaxf(cfg->flt_k, 0.05f), 20.0f) * expf(-hz / FLT_KICK_HZ);
                float kc = kick * cosf(v->flt_kick_phase), ks = kick * sinf(v->flt_kick_phase);
                v->filter_svf[0].s1 += (double)kc;
                v->filter_svf[0].s2 += (double)ks;
                /* the 24LPF rings as loud as the 12LPF (measured), so its second core takes the same kick, sized
                 * at the output: this engine's second stage carries its resonance gain after the core */
                if (cfg->filter_type == FILTER_LP_24) {
                    float g2 = 2.5f / fmaxf(cfg->flt_gain2, 1e-4f);
                    v->filter_svf[1].s1 += (double)(kc * g2);
                    v->filter_svf[1].s2 += (double)(ks * g2);
                }
                v->flt_kick = 0.0f;
            }
            svf_core(&v->filter_svf[0], cfg->flt_gain * osc_sum, flt_f, cfg->flt_k, &hp, &bp, &lp);
            /* `driven` is what the distortion clips: not the clean output for three of the four types */
            float driven;
            if (cfg->filter_type == FILTER_HP_12) {
                filtered = driven = hp;
            } else if (cfg->filter_type == FILTER_BP_12) {
                filtered = FLT_BP_GAIN * bp;
                driven = DIST_BP_GAIN * filtered;
            } else {
                /* the low-pass types' extra stage (FLT_LP_A0): the 12LPF takes it as it is, the 24LPF with
                 * its low-frequency gain held at 1, before the second core */
                float lp_a = FLT_LP_A0 + FLT_LP_AF * flt_f;
                if (cfg->filter_type == FILTER_LP_24) {
                    float lp2;
                    /* here the stage's corner follows the knob's coefficient alone, not resonance's share of it,
                     * times 0.9 (sines to 10.5 kHz at cutoff 100 and 127, six resonances: 0.2-0.5 dB rms) */
                    v->flt_shelf_y1 += fminf(0.9f, 0.9f * lp_stage_w * (FLT_LP_A0 + FLT_LP_AF * flt_fc)) * (lp - v->flt_shelf_y1);
                    if (fabsf(v->flt_shelf_y1) < 1e-15f) v->flt_shelf_y1 = 0.0f;
                    svf_core(&v->filter_svf[1], FLT_LP_GAIN * v->flt_shelf_y1, flt_f, cfg->flt_k, &hp, &bp, &lp2);
                    filtered = cfg->flt_gain2 * lp2;
                    driven = (DIST_LP24_GAIN / lp_a) * filtered;
                } else {
                    /* 1 - exp(-u) by its series (u stays under 0.7): the step-invariant pole */
                    float u = fminf(0.9f, lp_stage_w * lp_a);
                    v->flt_shelf_y1 += u * (1.0f - u * (0.5f - u * (1.0f / 6.0f - u * (1.0f / 24.0f)))) * (lp / lp_a - v->flt_shelf_y1);
                    if (fabsf(v->flt_shelf_y1) < 1e-15f) v->flt_shelf_y1 = 0.0f;
                    filtered = FLT_LP12_GAIN * v->flt_shelf_y1;
                    driven = FLT_LP12_GAIN * lp;
                }
            }
            if (!isfinite(filtered)) filtered = 0.0f;

            if (isnan(filtered) || isinf(filtered)) filtered = 0.0f;

            /* Key velocity does not set the level: on the microKORG (measured on the VST: the same level at every
             * velocity) it acts only where a virtual patch routes it (PATCH_SRC_VELOCITY) */
            v->vel_gain += (1.0f - v->vel_gain) * vel_glide_k;

            /* De-click: a fresh voice fades in (raised cosine, zero slope at the start). A sounding voice whose
             * amp EG was reset (voice_gate_on) fades its old level out by the same curve reversed while the new
             * attack fades in, inside the amp section. */
            float declick = 1.0f, amp_contour = a_env * a_env;
            if (v->declick_pos < DECLICK_SAMPLES) {
                float c = cosf((float)M_PI * (float)v->declick_pos / (float)DECLICK_SAMPLES);
                declick = 0.5f - 0.5f * c;
                declick *= declick;
                if (v->reset_from > 0.0f) {
                    float out = 0.5f + 0.5f * c;
                    amp_contour = amp_contour * declick + v->reset_from * out * out;
                    declick = 1.0f;
                }
                if (++v->declick_pos >= DECLICK_SAMPLES) v->reset_from = 0.0f;
            }
            v->amp_heard = amp_contour * declick;

            /* Amp Envelope & Velocity Scaling */
            /* the amp follows the square of its envelope (see the envelope notes) */
            /* The amp section: the level knob squared, the EG squared and the amp patches. */
            float amp_section = amp_contour * cfg->gain_amp * amp_gain;
            float voice_audio;
            if (cfg->drive > 0.0f) {
                /* Distortion (timbre byte 27) clips the amp section's OUTPUT, so the amp level and the amp EG
                 * are its drive: measured on the VST, a sine comes out clean below amp level 90 and a decaying
                 * note stops clipping as it fades (level contour and harmonics, clean against distorted). It is
                 * a gain of 2.69 (+8.6 dB) into a HARD clip at 1.55 times a full-level sine, exactly linear
                 * below it (harmonics under -80 dB); clipped right down, every filter type comes out at the
                 * same level. The level knob then acts once more, after the clip: with distortion on it is a
                 * cube (amp 71 = -15.2 dB, six settings), and a half-clipped sine at amp 110 has the
                 * fundamental that split gives (3.7 dB; the knob all before the clip would give 4.5). */
                float d = isfinite(driven) ? driven * amp_section * DIST_GAIN : 0.0f;
                voice_audio = fmaxf(-DIST_CLIP, fminf(DIST_CLIP, d)) * cfg->amp_knob * v->vel_gain * declick * v->unison_gain;
            } else {
                voice_audio = filtered * amp_section * v->vel_gain * declick * v->unison_gain;
            }
            DIAG_CHECK(voice_audio);
            if (isnan(voice_audio) || isinf(voice_audio)) voice_audio = 0.0f;

            /* Stereo pan spread & gain scaling. Layer mode: Voice A (Timbre 1) panned left and
             * scaled by its balance share, Voice B (Timbre 2) panned right and scaled likewise. */
            float v_gain_l = 1.0f;
            float v_gain_r = 1.0f;

            /* Level and place: the timbre balance (Layer mode), then the timbre's pan (byte +26) plus the voice's
             * place in a unison stack (the soft clip below handles overs) */
            {
                float vol = is_layer_mode ? (v->is_timbre_2 ? tB_vol : tA_vol) : 1.0f;
                /* a linear balance, as measured on the VST: unity on both sides at the centre, the far side
                 * falling to nothing and the near side rising to 2 (+6 dB) at a hard pan; patch -> pan moves
                 * the same position */
                float place = fmaxf(-1.0f, fminf(1.0f, cfg->pan + v->unison_pan + pmod[PATCH_DST_PAN]));
                /* the position moves at most PAN_SLEW_PER_S: a square LFO -> pan +63 crosses from one side to
                 * the other in 2 ms on the VST (10-90 % in 1.61 ms), not in a step */
                float pan_step = PAN_SLEW_PER_S / fs;
                if (v->pan_fresh) { v->pan_pos = place; v->pan_fresh = 0; }
                else if (place > v->pan_pos + pan_step) v->pan_pos += pan_step;
                else if (place < v->pan_pos - pan_step) v->pan_pos -= pan_step;
                else v->pan_pos = place;
                v_gain_l = vol * (1.0f - v->pan_pos);
                v_gain_r = vol * (1.0f + v->pan_pos);
            }

            voice_sum_l += voice_audio * v_gain_l;
            voice_sum_r += voice_audio * v_gain_r;
        }

        /* Brightness tilt: the VST's oscillators are brighter than an ideal 1/n saw. A first-order high shelf of
         * tilt_db 12.43 with its corner at tilt_hz 12.7 kHz (+1.7 dB at 3 kHz, +6 at 8 kHz, +11 at 16 kHz) follows
         * the plug-in's saw and square within 0.5 dB from 500 Hz to 19 kHz (tools/measure/tilt_fit.py: every
         * harmonic of two low notes each, through the open high-pass). The first fit (26.7 dB at 20 kHz, on one
         * take through the open low-pass) kept rising where the plug-in levels off: 2 dB too bright at 11 kHz,
         * 4.5 at 15, 8 at 19, through every filter type. It sits after the filters, where it is linear and
         * commutes with them, so it leaves the pre-filter drive stage (and its measured behaviour) untouched.
         * Measured alternative: per voice before the filter scored worse (total 106.8 vs 104.1; the boosted
         * edges clip in the drive stage). */
        {
            float yl = tilt_b0 * voice_sum_l + tilt_b1 * synth->tilt_x1[0] - tilt_a1 * synth->tilt_y1[0];
            float yr = tilt_b0 * voice_sum_r + tilt_b1 * synth->tilt_x1[1] - tilt_a1 * synth->tilt_y1[1];
            synth->tilt_x1[0] = voice_sum_l;
            synth->tilt_x1[1] = voice_sum_r;
            if (fabsf(yl) < 1e-15f) yl = 0.0f;
            if (fabsf(yr) < 1e-15f) yr = 0.0f;
            synth->tilt_y1[0] = yl;
            synth->tilt_y1[1] = yr;
            voice_sum_l = yl;
            voice_sum_r = yr;
        }

        /* DC blocker (one-pole high-pass, ~3.5 Hz): y[n] = x[n] - x[n-1] + R * y[n-1] */
        {
            const float dc_r = 0.9995f;
            float yl = voice_sum_l - synth->dc_x[0] + dc_r * synth->dc_y[0];
            float yr = voice_sum_r - synth->dc_x[1] + dc_r * synth->dc_y[1];
            synth->dc_x[0] = voice_sum_l;
            synth->dc_x[1] = voice_sum_r;
            if (fabsf(yl) < 1e-15f) yl = 0.0f;
            if (fabsf(yr) < 1e-15f) yr = 0.0f;
            synth->dc_y[0] = yl;
            synth->dc_y[1] = yr;
            voice_sum_l = yl;
            voice_sum_r = yr;
        }

        /* Soft-clip the voice mix so stacked voices and layered timbres saturate smoothly */
        /* Level structure. The plug-in's mix is linear: a hard-panned voice is twice a centred one, four unison
         * voices are four times one, a resonant peak stands 17 dB proud. With a full voice at 1.0 running
         * straight into this clip and the output limiter, all of that was squashed (a +20 pan route measured
         * 4.2 dB side to side where the plug-in gives 8.6). So the mix is taken down by TINYK_MIX_GAIN (9 dB)
         * first: the clip and the limiter now sit 9 dB above a single voice and act on extremes only.
         * TINYK_OUTPUT_HEADROOM gives most of it back after the limiter. */
#ifdef TINYK_LEVEL_PROBE
        tinyk_probe_mix = fmaxf(tinyk_probe_mix, fmaxf(fabsf(voice_sum_l), fabsf(voice_sum_r)) * TINYK_MIX_GAIN);
#endif
        voice_sum_l = soft_clip(voice_sum_l * TINYK_MIX_GAIN);
        voice_sum_r = soft_clip(voice_sum_r * TINYK_MIX_GAIN);

        /* 4. Mod FX (Chorus/Flanger, Ensemble or Phaser: modfx_process) */
        float chorus_l = voice_sum_l;
        float chorus_r = voice_sum_r;

        /* The chorus line is written always, so turning the chorus up never replays stale audio */
        uint32_t wpos = synth->chorus_write_pos;
        synth->chorus_buf_l[wpos] = voice_sum_l;
        synth->chorus_buf_r[wpos] = voice_sum_r;
        synth->chorus_write_pos = (wpos + 1) % CHORUS_BUFFER_SIZE;

        if (chorus_mix_p > 0.01f) modfx_process(synth, &chorus_l, &chorus_r, wpos, chorus_mix_p, fs);

        /* 5. Digital Delay with Feedback */
        float out_l = chorus_l;
        float out_r = chorus_r;

        if (!delay_on) {
            /* Bypassed: 100% dry. Flush the line once so stale repeats cannot reappear later. */
            if (synth->delay_active) {
                memset(synth->delay_buf_l, 0, sizeof(synth->delay_buf_l));
                memset(synth->delay_buf_r, 0, sizeof(synth->delay_buf_r));
                synth->delay_active = 0;
            }
        } else {
            synth->delay_active = 1;
            uint32_t dwpos = synth->delay_write_pos;

            /* both lines are the same length; the type only changes what feeds them and where they are heard */
            uint32_t didx = (dwpos + DELAY_BUFFER_SIZE - (uint32_t)target_delay_samples) % DELAY_BUFFER_SIZE;
            float dtap_l = synth->delay_buf_l[didx];
            float dtap_r = synth->delay_buf_r[didx];
            if (!isfinite(dtap_l)) dtap_l = 0.0f;
            if (!isfinite(dtap_r)) dtap_r = 0.0f;

            /* Dry stays at unity. Every repeat is the one before times the depth's gain, with no damping (the
             * VST's repeats keep their spectrum). Stereo: each side repeats on its own side. Cross: each repeat
             * swaps sides, the first one already. L/R: the two sides are summed to mono and the repeats
             * alternate left, right, left. */
            float wl, wr;
            switch (synth->delay_type) {
                case DELAY_TYPE_CROSS:
                    wl = chorus_l + delay_feedback * dtap_r;
                    wr = chorus_r + delay_feedback * dtap_l;
                    out_l = chorus_l + dtap_r * delay_send;
                    out_r = chorus_r + dtap_l * delay_send;
                    break;
                case DELAY_TYPE_LR:
                    wl = 0.5f * (chorus_l + chorus_r) + delay_feedback * dtap_r;
                    wr = delay_feedback * dtap_l;
                    out_l = chorus_l + dtap_l * delay_send;
                    out_r = chorus_r + dtap_r * delay_send;
                    break;
                default:
                    wl = chorus_l + delay_feedback * dtap_l;
                    wr = chorus_r + delay_feedback * dtap_r;
                    out_l = chorus_l + dtap_l * delay_send;
                    out_r = chorus_r + dtap_r * delay_send;
                    break;
            }
            if (fabsf(wl) < 1e-15f) wl = 0.0f;
            if (fabsf(wr) < 1e-15f) wr = 0.0f;
            synth->delay_buf_l[dwpos] = wl;
            synth->delay_buf_r[dwpos] = wr;
            synth->delay_write_pos = (dwpos + 1) % DELAY_BUFFER_SIZE;
        }

        /* EQ: the program's Low / Hi shelves (eq_update); a band at 0 dB is skipped */
        if (synth->eq_gain[0] != 0) {
            out_l = eq_band(synth->eq_coef[0], synth->eq_z[0][0], out_l);
            out_r = eq_band(synth->eq_coef[0], synth->eq_z[0][1], out_r);
        }
        if (synth->eq_gain[1] != 0) {
            out_l = eq_band(synth->eq_coef[1], synth->eq_z[1][0], out_l);
            out_r = eq_band(synth->eq_coef[1], synth->eq_z[1][1], out_r);
        }

        /* 6. Master Volume, Pan, Soft-Knee Limiter & Convert to int16 */
        if (master_vol_p <= 0.01f) master_vol_p = 0.8f;

        /* Output DC blocker (~3.5 Hz, as on the voice mix): subsonic content the Mod FX and delay lift (the Phaser's
         * feedback passes DC above unity) never reaches the output as an offset (B.35 SlowBend's 0.07) */
        {
            const float dc_r = 0.9995f;
            float yl = out_l - synth->out_dc_x[0] + dc_r * synth->out_dc_y[0];
            float yr = out_r - synth->out_dc_x[1] + dc_r * synth->out_dc_y[1];
            synth->out_dc_x[0] = out_l;
            synth->out_dc_x[1] = out_r;
            if (fabsf(yl) < 1e-15f) yl = 0.0f;
            if (fabsf(yr) < 1e-15f) yr = 0.0f;
            synth->out_dc_y[0] = yl;
            synth->out_dc_y[1] = yr;
            out_l = yl;
            out_r = yr;
        }

        {
            float lvl = fmaxf(fabsf(out_l), fabsf(out_r));
            synth->out_level = (lvl > synth->out_level) ? lvl : synth->out_level * 0.9999f;
        }
        if (synth->flush_stage) { /* program-change flush, see flush_for_program */
            float step = 1.0f / (FLUSH_FADE_S * fs);
            if (synth->flush_stage == 1) {
                synth->flush_gain -= step;
                if (synth->flush_gain <= 0.0f) {
                    synth->flush_gain = 0.0f;
                    flush_effects(synth);
                    synth->out_level = 0.0f;
                    synth->flush_stage = 2;
                }
            } else {
                synth->flush_gain += step;
                if (synth->flush_gain >= 1.0f) {
                    synth->flush_gain = 1.0f;
                    synth->flush_stage = 0;
                }
            }
            out_l *= synth->flush_gain;
            out_r *= synth->flush_gain;
        }

        DIAG_CHECK(out_l);
        DIAG_CHECK(out_r);
        float pre_lim_l = out_l * master_vol_p * pan_l * TINYK_OUTPUT_MAKEUP;
        float pre_lim_r = out_r * master_vol_p * pan_r * TINYK_OUTPUT_MAKEUP;

#ifdef TINYK_LEVEL_PROBE
        tinyk_probe_out = fmaxf(tinyk_probe_out, fmaxf(fabsf(pre_lim_l), fabsf(pre_lim_r)));
#endif
        float lim_l = soft_knee_limiter(pre_lim_l) * TINYK_OUTPUT_HEADROOM;
        float lim_r = soft_knee_limiter(pre_lim_r) * TINYK_OUTPUT_HEADROOM;

        int32_t sample_l = (int32_t)(lim_l * 32767.0f);
        int32_t sample_r = (int32_t)(lim_r * 32767.0f);

        if (sample_l > 32767)  sample_l = 32767;
        if (sample_l < -32768) sample_l = -32768;
        if (sample_r > 32767)  sample_r = 32767;
        if (sample_r < -32768) sample_r = -32768;

        out_lr[s * 2]     = (int16_t)sample_l;
        out_lr[s * 2 + 1] = (int16_t)sample_r;
    }
}

/* =====================================================================
 * Move Plugin API v2 Implementation
 * ===================================================================== */

static void apply_state(synth_engine_t *synth, const char *json);

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    /* Decode any .syx banks once, here, so bank switching later never touches the file system */
    if (module_dir && g_syx_bank_count == 0) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/banks", module_dir);
        syx_scan_dir(dir);
    }
    tinyk_instance_t *inst = (tinyk_instance_t*)calloc(1, sizeof *inst);
    if (!inst) return NULL;
    synth_engine_t *synth = &inst->synth;
    synth_init(synth);
    if (json_defaults) {
        const char *vm = strstr(json_defaults, "\"voice_mode\"");
        if (vm) {
            const char *colon = strchr(vm, ':');
            if (colon) {
                while (*colon == ' ' || *colon == '\t' || *colon == ':') colon++;
                if (*colon == '1' || strncmp(colon, "\"Layer", 6) == 0) {
                    synth->voice_mode = 1;
                    synth->params[PARAM_VOICE_MODE] = 1.0f;
                }
            }
        }
    }
    apply_state(synth, json_defaults); /* a slot restored with its saved "state" in the defaults */
    synth->labels_reported = label_context(synth);
    return inst;
}

static void v2_destroy_instance(void *instance) {
    free(instance);
}

static void parse_midi_buffer(synth_engine_t *synth, const uint8_t *msg, int len) {
    if (!synth) synth = default_synth();
    if (!msg || len <= 0) return;

    /* Ensure synth is initialized if parameters are empty */
    if (synth->params[PARAM_MASTER_VOL] <= 0.01f) {
        synth_init(synth);
    }

    int i = 0;
    uint8_t running_status = 0;

    while (i < len) {
        uint8_t b = msg[i];

        /* Real-time single byte messages (0xF8 - 0xFF) */
        if (b >= 0xF8) {
            i++;
            continue;
        }

        /* Status byte (0x80 - 0xFF) */
        if (b >= 0x80) {
            if (b < 0xF0) {
                running_status = b;
                i++;
            } else {
                /* System common messages */
                running_status = 0;
                if (b == 0xF0) { /* SysEx */
                    while (i < len && msg[i] != 0xF7) i++;
                    if (i < len) i++;
                } else if (b == 0xF1 || b == 0xF3) {
                    i += 2;
                } else if (b == 0xF2) {
                    i += 3;
                } else {
                    i++;
                }
                continue;
            }
        }

        /* Channel voice message handling via running_status */
        if (running_status >= 0x80 && running_status < 0xF0) {
            uint8_t cmd = running_status & 0xF0;

            if (cmd == 0x90) { /* Note On */
                if (i + 1 < len) {
                    uint8_t note = msg[i] & 0x7F;
                    uint8_t vel = msg[i + 1] & 0x7F;
                    i += 2;
                    if (vel > 0) {
                        key_note_on(synth, note, vel);
                    } else {
                        key_note_off(synth, note);
                    }
                } else {
                    break;
                }
            } else if (cmd == 0x80) { /* Note Off */
                if (i + 1 < len) {
                    uint8_t note = msg[i] & 0x7F;
                    i += 2;
                    key_note_off(synth, note);
                } else {
                    break;
                }
            } else if (cmd == 0xB0) { /* Control Change */
                if (i + 1 < len) {
                    uint8_t cc = msg[i] & 0x7F;
                    uint8_t val = msg[i + 1] & 0x7F;
                    i += 2;
                    float val01 = (float)val / 127.0f;
                    if (cc == 120 || cc == 123) {
                        synth_all_notes_off(synth);
                    } else if (cc == 1) {
                        synth->modwheel_src = val01; /* virtual patch source Mod Wheel: 0 at rest, up to +1 */
                    } else if (cc == 74) {
                        synth_set_param(synth, "cutoff", val01);
                    } else if (cc == 71) {
                        synth_set_param(synth, "resonance", val01);
                    } else if (cc == 5) {
                        synth_set_param(synth, "portamento", val01);
                    } else if (cc == 7) {
                        synth_set_param(synth, "master_vol", val01);
                    } else if (cc == 10) {
                        synth_set_param(synth, "pan", val01);
                    }
                } else {
                    break;
                }
            } else if (cmd == 0xC0) { /* Program Change */
                if (i < len) {
                    int prog = msg[i] & 0x7F;
                    synth_load_preset(synth, prog % 128);
                    i++;
                } else {
                    break;
                }
            } else if (cmd == 0xD0) { /* Channel Pressure */
                if (i < len) {
                    i++;
                } else {
                    break;
                }
            } else if (cmd == 0xE0) { /* Pitch Bend */
                if (i + 1 < len) {
                    int lsb = msg[i] & 0x7F;
                    int msb = msg[i + 1] & 0x7F;
                    int bend = (msb << 7) | lsb;
                    synth->bend_src = (float)(bend - 8192) / 8192.0f; /* virtual patch source Pitch Bend, -1..+1 */
                    i += 2;
                } else {
                    break;
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : default_synth();
    parse_midi_buffer(synth, msg, len);
}

/* --- Slot state for the host's autosave / boot restore -----------------------------------------------------
 * Schwung saves get_param("state") into the Set's slot file every few seconds and hands it back through
 * set_param("state") when the slot is restored (boot, Set change). A read the host cannot complete makes it keep
 * the old file, so TinyK always answers. The state is the bank (by name: the file list can change), the program
 * and everything a knob can have changed since it loaded: params, both timbres' parameter sets and extras, voice
 * mode, timbre edit / balance, octave and the delay time base. */
#define STATE_VERSION 1
#define STATE_EXTRA_COUNT 37     /* older states have the first 24 (before Osc 1 Ctrl 1 / 2), 26 (before assign / pan), 30 (before the three level knobs) or 33 (before EG reset, bend range, vibrato int) */
#define STATE_EXTRA_MIN 24

static void extra_to_floats(const timbre_extra_t *x, float *f) {
    int n = 0;
    f[n++] = x->transpose_semi; f[n++] = x->noise_level; f[n++] = x->level; f[n++] = x->dwgs;
    for (int i = 0; i < 2; i++) {
        f[n++] = (float)x->lfo_wave[i]; f[n++] = (float)x->lfo_keysync[i];
        f[n++] = x->lfo_rate[i]; f[n++] = (float)x->lfo_sync_note[i];
    }
    for (int i = 0; i < 4; i++) {
        f[n++] = (float)x->patch_src[i]; f[n++] = (float)x->patch_dst[i]; f[n++] = x->patch_int[i];
    }
    f[n++] = x->osc1_ctrl[0]; f[n++] = x->osc1_ctrl[1];
    f[n++] = (float)x->assign; f[n++] = x->unison_cents; f[n++] = x->pan; f[n++] = (float)x->multi_trigger;
    f[n++] = x->lvl_osc1; f[n++] = x->lvl_osc2; f[n++] = x->lvl_amp;
    f[n++] = (float)x->eg_reset[0]; f[n++] = (float)x->eg_reset[1]; f[n++] = x->bend_semi; f[n++] = x->vibrato_int;
}


/* f holds count values (STATE_EXTRA_MIN..STATE_EXTRA_COUNT); fields past the end keep the program's value */
static void extra_from_floats(timbre_extra_t *x, const float *f, int count) {
    int n = 0;
    x->transpose_semi = fmaxf(-24.0f, fminf(24.0f, f[n++]));
    x->noise_level = clamp01f(f[n++]); x->level = clamp01f(f[n++]); x->dwgs = clamp01f(f[n++]);
    for (int i = 0; i < 2; i++) {
        x->lfo_wave[i] = clampi((int)lroundf(f[n++]), 0, 3);
        x->lfo_keysync[i] = clampi((int)lroundf(f[n++]), 0, 2);
        x->lfo_rate[i] = clamp01f(f[n++]);
        x->lfo_sync_note[i] = clampi((int)lroundf(f[n++]), -1, 14);
    }
    for (int i = 0; i < 4; i++) {
        x->patch_src[i] = clampi((int)lroundf(f[n++]), 0, PATCH_SRC_COUNT - 1);
        x->patch_dst[i] = clampi((int)lroundf(f[n++]), 0, PATCH_DST_COUNT - 1);
        x->patch_int[i] = fmaxf(-1.0f, fminf(1.0f, f[n++]));
    }
    for (int i = 0; i < 2 && n < count; i++) x->osc1_ctrl[i] = clamp01f(f[n++]);
    if (n < count) x->assign = clampi((int)lroundf(f[n++]), 0, 2);
    if (n < count) x->unison_cents = fmaxf(0.0f, fminf(127.0f, f[n++]));
    if (n < count) x->pan = fmaxf(-1.0f, fminf(1.0f, f[n++]));
    if (n < count) x->multi_trigger = f[n++] >= 0.5f;
    if (n + 2 < count) {
        x->lvl_osc1 = clamp01f(f[n++]); x->lvl_osc2 = clamp01f(f[n++]); x->lvl_amp = clamp01f(f[n++]);
    }
    if (n + 3 < count) {
        x->eg_reset[0] = f[n++] >= 0.5f; x->eg_reset[1] = f[n++] >= 0.5f;
        x->bend_semi = fmaxf(-12.0f, fminf(12.0f, f[n++])); x->vibrato_int = fmaxf(-63.0f, fminf(63.0f, f[n++]));
    }
    x->mix_seen = x->level_seen = NAN; /* the restored knobs are what these levels already stand for */
}

/* The value after "key": in a JSON object, or NULL */
static const char *state_find(const char *json, const char *key) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int state_number(const char *json, const char *key, float *out) {
    const char *p = state_find(json, key);
    char *end;
    if (!p) return 0;
    float v = strtof(p, &end);
    if (end == p || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* Reads the array at key: min_n..max_n numbers. Returns how many; 0 (and out untouched) on any mismatch. */
static int state_array_n(const char *json, const char *key, float *out, int min_n, int max_n) {
    float tmp[NUM_PARAMS > STATE_EXTRA_COUNT ? NUM_PARAMS : STATE_EXTRA_COUNT];
    const char *p = state_find(json, key);
    int n = 0;
    if (!p || *p != '[' || max_n > (int)(sizeof tmp / sizeof tmp[0])) return 0;
    p++;
    for (;;) {
        char *end;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p == ']') break;
        if (n > 0) {
            if (*p != ',') return 0;
            p++;
        }
        if (n == max_n) return 0;
        tmp[n] = strtof(p, &end);
        if (end == p || !isfinite(tmp[n])) return 0;
        p = end;
        n++;
    }
    if (n < min_n) return 0;
    memcpy(out, tmp, (size_t)n * sizeof(float));
    return n;
}

/* Exactly n numbers */
static int state_array(const char *json, const char *key, float *out, int n) {
    return state_array_n(json, key, out, n, n) == n;
}

/* The JSON string at key, unescaped into out; 0 when absent */
static int state_string(const char *json, const char *key, char *out, int out_len) {
    const char *p = state_find(json, key);
    int n = 0;
    if (!p || *p != '"' || out_len <= 0) return 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (n < out_len - 1) out[n++] = *p;
    }
    out[n] = '\0';
    return *p == '"';
}

/* Restores a get_param("state") document. Anything missing or malformed keeps the program's loaded value. */
static void apply_state(synth_engine_t *synth, const char *json) {
    char bank[SYX_BANK_NAME_LEN + 8];
    float f, arr[NUM_PARAMS], ex[STATE_EXTRA_COUNT];
    if (!json || !strstr(json, "\"tinyk_state\"")) return;

    int b = 0;
    if (state_string(json, "bank", bank, sizeof bank)) {
        for (int i = 0; i < g_syx_bank_count; i++) {
            if (strcmp(bank, g_syx_banks[i].name) == 0) b = i + 1;
        }
    }
    synth->bank_file = b; /* a bank no longer on the Move falls back to Built-in; the saved values below still apply */
    rebuild_playlist(synth);
    int preset = state_number(json, "preset", &f) ? clampi((int)lroundf(f), 0, NUM_PRESETS - 1) : 0;
    synth_all_notes_off(synth);
    load_preset(synth, preset);

    if (state_array(json, "params", arr, NUM_PARAMS)) {
        for (int i = 0; i < NUM_PARAMS; i++) synth->params[i] = clamp01f(arr[i]);
    }
    if (state_array(json, "timbre1", arr, NUM_PARAMS)) {
        for (int i = 0; i < NUM_PARAMS; i++) synth->timbre_params[0][i] = clamp01f(arr[i]);
    }
    if (state_array(json, "timbre2", arr, NUM_PARAMS)) {
        for (int i = 0; i < NUM_PARAMS; i++) synth->timbre_params[1][i] = clamp01f(arr[i]);
    }
    int nx;
    if ((nx = state_array_n(json, "extra1", ex, STATE_EXTRA_MIN, STATE_EXTRA_COUNT)) > 0) extra_from_floats(&synth->timbre_extra[0], ex, nx);
    if ((nx = state_array_n(json, "extra2", ex, STATE_EXTRA_MIN, STATE_EXTRA_COUNT)) > 0) extra_from_floats(&synth->timbre_extra[1], ex, nx);
    if (state_number(json, "voice_mode", &f)) synth->voice_mode = f >= 0.5f ? 1 : 0;
    if (state_number(json, "timbre_edit", &f)) synth->timbre_edit = f >= 0.5f ? 1 : 0;
    if (state_number(json, "timbre_balance", &f)) synth->timbre_balance = clamp01f(f);
    if (state_number(json, "octave", &f)) synth->octave_transpose = clampi((int)lroundf(f), -4, 4);
    if (state_number(json, "delay_sync", &f)) synth->delay_sync_note = clampi((int)lroundf(f), -1, 14);
    if (state_number(json, "delay_type", &f)) synth->delay_type = clampi((int)lroundf(f), 0, 2);
    if (state_number(json, "modfx_type", &f)) synth->modfx_type = clampi((int)lroundf(f), 0, 2);
    if (state_number(json, "modfx_speed", &f)) synth->modfx_speed = clamp01f(f);
    if (state_number(json, "eq_low_freq", &f)) synth->eq_freq[0] = clampi((int)lroundf(f), 0, 29);
    if (state_number(json, "eq_low_gain", &f)) synth->eq_gain[0] = clampi((int)lroundf(f), -12, 12);
    if (state_number(json, "eq_hi_freq", &f)) synth->eq_freq[1] = clampi((int)lroundf(f), 0, 29);
    if (state_number(json, "eq_hi_gain", &f)) synth->eq_gain[1] = clampi((int)lroundf(f), -12, 12);
    if (state_number(json, "arp_on", &f)) set_arp_on(synth, f >= 0.5f);
    for (int i = 0; i < ARP_PARAM_COUNT; i++) {
        if (state_number(json, ARP_PARAMS[i].key, &f)) arp_param_set(synth, i, (int)lroundf(f));
    }
    if (state_number(json, "arp_length", &f)) synth->arp.set.length = clampi((int)lroundf(f), 1, 8);
    if (state_number(json, "arp_pattern", &f)) synth->arp.set.pattern = (uint8_t)clampi((int)lroundf(f), 0, 255);
    synth->params[PARAM_VOICE_MODE] = synth->voice_mode ? 1.0f : 0.0f;
    synth->params[PARAM_TIMBRE_EDIT] = synth->timbre_edit ? 1.0f : 0.0f;
    synth->params[PARAM_TIMBRE_BALANCE] = synth->timbre_balance;
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    synth_engine_t *synth = (synth_engine_t*)instance;
    if (!synth || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        apply_state(synth, val);
        return;
    }

    if (strcmp(key, "arp_on") == 0) { /* option name or index */
        synth_set_param(synth, key, (strcmp(val, "On") == 0 || strcmp(val, "ON") == 0) ? 1.0f : (float)atof(val));
        return;
    }
    {
        int ai = arp_param_index(key);
        if (ai >= 0) { /* an option name ("ALT1", "1/8", "Timbre 2"), or the index / integer */
            const arp_param_t *p = &ARP_PARAMS[ai];
            int v = p->options ? name_index(val, p->options, p->count) : -1;
            if (v < 0 && p->shorts) v = name_index(val, p->shorts, p->count);
            synth_set_param(synth, key, v >= 0 ? (float)v : (float)atof(val));
            return;
        }
    }
    if (strncmp(key, "arp_step", 8) == 0) { /* "Play" / "Rest" or 1 / 0 */
        int play = (strcmp(val, "Play") == 0 || strcmp(val, "PLAY") == 0) ? 1
                 : (strcmp(val, "Rest") == 0 || strcmp(val, "REST") == 0) ? 0 : atof(val) >= 0.5;
        synth_set_param(synth, key, (float)play);
        return;
    }

    if (strcmp(key, "all_notes_off") == 0) {
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "octave_transpose") == 0) {
        synth->octave_transpose = atoi(val);
        return;
    }

    if (strcmp(key, "genre_category") == 0) {
        int genre = -1;
        const char *genres[8] = {
            "Trance", "Techno", "Electronica", "DnB",
            "Hiphop", "Retro", "SE", "Vocoder"
        };
        for (int i = 0; i < 8; i++) {
            if (strstr(val, genres[i])) {
                genre = i;
                break;
            }
        }
        if (genre < 0) {
            if (val[0] >= '1' && val[0] <= '8' && val[1] == '.') {
                genre = val[0] - '1';
            } else {
                float f = (float)atof(val);
                if (f >= 0.0f && f <= 7.0f && strchr(val, '.') == NULL) {
                    genre = (int)roundf(f);
                } else if (f >= 0.0f && f <= 1.0f) {
                    genre = (int)roundf(f * 7.0f);
                }
            }
        }
        if (genre < 0) genre = 0;
        select_category_pos(synth, genre, current_patch(synth));
        return;
    }

    if (strcmp(key, "program_num") == 0) {
        float f = (float)atof(val);
        int p = 1;
        if (f >= 1.0f && f <= 16.0f) {
            p = (int)roundf(f);
        } else if (f >= 0.0f && f < 1.0f) {
            p = 1 + (int)roundf(f * 15.0f);
        }
        if (p < 1) p = 1;
        if (p > 16) p = 16;
        select_category_pos(synth, synth->genre_category, p - 1);
        return;
    }

    if (strcmp(key, "bank_side") == 0) {
        int side = 0;
        if (strstr(val, "Side B") || strstr(val, "side b") || strcmp(val, "1") == 0 || strcmp(val, "1.0") == 0) {
            side = 1;
        } else if (strstr(val, "Side A") || strstr(val, "side a") || strcmp(val, "0") == 0 || strcmp(val, "0.0") == 0) {
            side = 0;
        } else {
            float f = (float)atof(val);
            side = (f >= 0.5f) ? 1 : 0;
        }
        select_side(synth, side);
        return;
    }

    if (strcmp(key, "bank_file") == 0) {
        /* bank number, or a bank name ("Built-in" or a file name without .syx) */
        int b = -1;
        if (strcmp(val, "Built-in") == 0) b = 0;
        for (int i = 0; i < g_syx_bank_count && b < 0; i++) {
            if (strcmp(val, g_syx_banks[i].name) == 0) b = i + 1;
        }
        if (b < 0) b = (int)roundf((float)atof(val));
        select_bank_file(synth, b);
        return;
    }

    if (strcmp(key, "category") == 0 || strcmp(key, "patch") == 0) {
        /* option name ("Retro", "B3", "B.12 ARPEJMATR") or index */
        int byname = (strcmp(key, "category") == 0) ? category_index_by_name(synth, val) : patch_coord_index(synth, val);
        if (byname < 0 && strcmp(key, "patch") == 0) byname = patch_label_index(synth, val);
        synth_set_param(synth, key, byname >= 0 ? (float)byname : (float)atof(val));
        return;
    }

    if (strcmp(key, "dwgs_wave") == 0 || strcmp(key, "dwgs_pick") == 0) {
        /* wave name ("Organ3"), list label ("42 Organ3") or index 0..63 */
        const char *name = val;
        while (*name >= '0' && *name <= '9') name++;
        int i = (*name == ' ') ? name_index(name + 1, DWGS_NAMES, DWGS_WAVE_COUNT) : name_index(val, DWGS_NAMES, DWGS_WAVE_COUNT);
        synth_set_param(synth, key, i >= 0 ? (float)i : (float)atof(val));
        return;
    }

    const selector_t *sel = find_selector(key);
    if (sel) {
        /* option name ("Square", "SQR", "BPF12") or index; a value with a decimal point is the normalized 0..1 */
        int n = sel->count;
        int i = name_index(val, sel->options, n);
        if (i < 0) i = name_index(val, sel->shorts, n);
        if (i < 0 && strchr(val, '.') != NULL) {
            synth_set_param(synth, key, clamp01f((float)atof(val)));
            return;
        }
        if (i < 0) i = atoi(val);
        if (i < 0) i = 0;
        if (i > n - 1) i = n - 1;
        int mode = sel->to_engine ? sel->to_engine[i] : i;
        synth_set_param(synth, key, (float)mode / (float)(n - 1));
        return;
    }

    if (strcmp(key, "program") == 0) {
        /* "11".."88", or with the bank: "A.11", "B27", "b 34" */
        const char *p = val;
        while (*p == ' ') p++;
        if (*p == 'A' || *p == 'a' || *p == 'B' || *p == 'b') {
            synth->bank_side = (*p == 'B' || *p == 'b') ? 1 : 0;
            p++;
            while (*p == '.' || *p == ' ' || *p == '-') p++;
        }
        float f = (float)atof(p);
        if (f >= 0.0f && f < 1.0f && strchr(p, '.') != NULL) {
            int idx = (int)roundf(f * 63.0f);
            select_program(synth, synth->bank_side, idx / 8, idx % 8);
        } else {
            set_program_number(synth, (int)roundf(f));
        }
        return;
    }

    if (strcmp(key, "preset") == 0) {
        synth_set_param(synth, "preset", (float)atof(val));
        return;
    }

    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        int mode = 0;
        if (strstr(val, "Layer") || strstr(val, "layer") || strstr(val, "2-Voice") || strstr(val, "2-voice") ||
            strcmp(val, "1") == 0 || strcmp(val, "1.0") == 0 || strcmp(val, "1.000000") == 0) {
            mode = 1;
        } else if (strstr(val, "Single") || strstr(val, "single") || strstr(val, "4-Voice") || strstr(val, "4-voice") ||
                   strcmp(val, "0") == 0 || strcmp(val, "0.0") == 0 || strcmp(val, "0.000000") == 0) {
            mode = 0;
        } else {
            float f = (float)atof(val);
            mode = (f >= 0.5f) ? 1 : 0;
        }
        synth->voice_mode = mode;
        synth->params[PARAM_VOICE_MODE] = (mode == 1) ? 1.0f : 0.0f;
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 || strcmp(key, "Layer") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        int t = 0;
        if (strstr(val, "2") || strcmp(val, "1") == 0 || strcmp(val, "1.0") == 0) {
            t = 1;
        } else {
            float f = (float)atof(val);
            t = (f >= 0.5f) ? 1 : 0;
        }
        synth->timbre_edit = t;
        synth->params[PARAM_TIMBRE_EDIT] = (t == 1) ? 1.0f : 0.0f;
        return;
    }

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 || strcmp(key, "Layer Bal") == 0 ||
        strcmp(key, "Balance") == 0 || strcmp(key, "4") == 0 || strcmp(key, "param_4") == 0) {
        float f = (float)atof(val);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        synth->timbre_balance = f;
        synth->params[PARAM_TIMBRE_BALANCE] = f;
        return;
    }

    float float_val = (float)atof(val);
    synth_set_param(synth, key, float_val);
}

/* --- Live parameter metadata for the host --------------------------------------------------------
 * The Schwung host reads get_param("chain_params") and lets it override the inline ui_hierarchy
 * metadata; enums display their option names. Serving it from the engine is what lets Bank show the
 * .syx file names found at start-up. Built on request into the instance's scratch buffer. */
typedef struct { char *buf; size_t cap, len; } json_out_t;

static void json_put(json_out_t *o, const char *text) {
    size_t n = strlen(text);
    if (o->len + n + 1 > o->cap) n = o->cap - o->len - 1;
    memcpy(o->buf + o->len, text, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void json_put_string(json_out_t *o, const char *text) {
    char esc[8];
    json_put(o, "\"");
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\') { esc[0] = '\\'; esc[1] = (char)*c; esc[2] = '\0'; }
        else if (*c < 0x20) snprintf(esc, sizeof esc, "\\u%04x", *c);
        else { esc[0] = (char)*c; esc[1] = '\0'; }
        json_put(o, esc);
    }
    json_put(o, "\"");
}

/* Labels for the per-timbre controls in Layer mode, so the grid says which layer a turn changes: { key, cell
 * label, full label (the page's own label with "T2 ") }, with the 2 read as 1 while Timbre 1 is edited. Single
 * mode keeps the plain labels from ui_hierarchy. The cell labels fit the host's 5-character cell (checked with
 * its labelVerbatim). */
static const char *const T2_LABELS[][3] = {
    { "cutoff", "L2.CUT", "L2 Cutoff" }, { "resonance", "L2.RES", "L2 Resonance" },
    { "attack2", "L2.ATK", "L2 Amp Atk" }, { "release2", "L2.REL", "L2 Amp Rel" },
    { "drive", "L2.DIST", "L2 Distortion" }, { "level", "L2.LVL", "L2 Level" },
    { "wave1", "L2.WV1", "L2 Wave 1" }, { "pulse_width", "L2.PW", "L2 Pulse Width" },
    { "osc1_ctrl1", "L2.CT1", "L2 Control 1" }, { "osc1_ctrl2", "L2.CT2", "L2 Control 2" },
    { "dwgs_wave", "L2.DWGS", "L2 DWGS Wave" },
    { "wave2", "L2.WV2", "L2 Wave 2" }, { "osc2_semi", "L2.SEMI", "L2 Semi" }, { "osc2_tune", "L2.TUNE", "L2 Tune" },
    { "attack1", "L2.FATK", "L2 Filter Atk" }, { "decay1", "L2.FDCY", "L2 Filter Dcy" },
    { "sustain1", "L2.FSU", "L2 Filter Sus" }, { "release1", "L2.FRL", "L2 Filter Rel" },
    { "decay2", "L2.ADCY", "L2 Amp Dcy" }, { "sustain2", "L2.ASU", "L2 Amp Sus" },
    { "keytrack", "L2.KTRK", "L2 Key Track" }, { "env_int", "L2.EGIN", "L2 EG Int" },
    { "osc_mix", "L2.MIX", "L2 Osc Mix" }, { "noise_level", "L2.NOIS", "L2 Noise" },
    { "sync_ring", "L2.SYNC", "L2 Sync / Ring" }, { "filter_type", "L2.FTYP", "L2 Filter Type" },
    { "voice_assign", "L2.VOIC", "L2 Voice" }, { "portamento", "L2.PORT", "L2 Portamento" }
};

/* Opens a chain_params entry: key, name and, for a per-timbre control in Layer mode (badge 1 or 2, see
 * timbre_badge), its T1 / T2 labels */
static void json_put_head(json_out_t *o, const char *key, const char *name, int badge) {
    char label[32], cell[16];
    json_put(o, "{\"key\":");
    json_put_string(o, key);
    json_put(o, ",\"name\":");
    json_put_string(o, name);
    for (size_t i = 0; badge && i < sizeof T2_LABELS / sizeof T2_LABELS[0]; i++) {
        if (strcmp(key, T2_LABELS[i][0]) != 0) continue;
        snprintf(label, sizeof label, "%s", T2_LABELS[i][2]);
        snprintf(cell, sizeof cell, "%s", T2_LABELS[i][1]);
        label[1] = cell[1] = (char)('0' + badge);
        json_put(o, ",\"label\":");
        json_put_string(o, label);
        json_put(o, ",\"short_name\":");
        json_put_string(o, cell);
        break;
    }
}

/* The DWGS Waves list, numbered as on the hardware (1..64). The host shows `label` and writes `index` */
static const char *build_dwgs_list_json(char *buf, size_t cap) {
    json_out_t o = { buf, cap, 0 };
    char item[64];
    buf[0] = '\0';
    for (int i = 0; i < DWGS_WAVE_COUNT; i++) {
        snprintf(item, sizeof item, "%s{\"index\":%d,\"label\":\"%d %s\"}", i ? "," : "[", i, i + 1, DWGS_NAMES[i]);
        json_put(&o, item);
    }
    json_put(&o, "]");
    return buf;
}

/* An enum entry; shorts (the 3-character square's texts) may be NULL */
static void json_put_enum(json_out_t *o, const char *key, const char *name, const char *const *opts,
                          const char *const *shorts, int n, int badge) {
    json_put_head(o, key, name, badge);
    json_put(o, ",\"type\":\"enum\",\"options\":[");
    for (int i = 0; i < n; i++) {
        if (i) json_put(o, ",");
        json_put_string(o, opts[i]);
    }
    if (shorts) {
        json_put(o, "],\"short_options\":[");
        for (int i = 0; i < n; i++) {
            if (i) json_put(o, ",");
            json_put_string(o, shorts[i]);
        }
    }
    json_put(o, "],\"default\":0},");
}

/* Program: options are the current category's patch names ("B.12 ARPEJMATR"), which the host shows when
 * the knob is touched or turned; short_options are the codes its 3-character enum square has room for. */
/* The Category names on offer (synth->play_ncat of them): the matrix row each stands for; row 8 holds ordinary
 * programs in a bank that does not keep vocoders there: "Other" */
static void category_names(const synth_engine_t *synth, const char *names[8]) {
    for (int c = 0; c < 8; c++) {
        int row = c < synth->play_ncat ? synth->cat_row[c] : 7;
        names[c] = (row == 7) ? "Other" : CATEGORY_NAMES[row];
    }
}

static void json_put_patch_enum(json_out_t *o, const synth_engine_t *synth) {
    char label[48];
    int count = category_patch_count(synth, synth->genre_category);
    json_put(o, "{\"key\":\"patch\",\"name\":\"Program\",\"type\":\"enum\",\"options\":[");
    for (int i = 0; i < count; i++) {
        if (i) json_put(o, ",");
        format_preset_name(synth, program_raw(synth, synth->genre_category, i), label, sizeof label);
        json_put_string(o, label);
    }
    json_put(o, "],\"short_options\":[");
    for (int i = 0; i < count; i++) {
        char coord[8];
        if (i) json_put(o, ",");
        slot_coord(program_raw(synth, synth->genre_category, i), coord, sizeof coord);
        json_put_string(o, coord);
    }
    json_put(o, "],\"default\":0},");
}

static const char *build_bank_list_json(char *buf, size_t cap) {
    json_out_t o = { buf, cap, 0 };
    char item[32];
    buf[0] = '\0';
    json_put(&o, "[{\"index\":0,\"label\":\"Built-in\"}");
    for (int i = 0; i < g_syx_bank_count; i++) {
        snprintf(item, sizeof item, ",{\"index\":%d,\"label\":", i + 1);
        json_put(&o, item);
        json_put_string(&o, g_syx_banks[i].name);
        json_put(&o, "}");
    }
    json_put(&o, "]");
    return buf;
}

static const char *build_chain_params_json(const synth_engine_t *synth, char *buf, size_t cap) {
    json_out_t o = { buf, cap, 0 };
    const char *bank_names[SYX_MAX_BANKS + 1];
    static const char *const VOICE_MODES[2] = { "Single", "Layer" };
    static const char *const VOICE_MODES_SHORT[2] = { "SNGL", "LAYR" }; /* "Single" folds to SIN/GLE */
    static const char *const TIMBRES[2] = { "Layer 1", "Layer 2" };
    static const char *const TIMBRES_SHORT[2] = { "L1", "L2" };
    /* Single mode has one layer: both options say so (two of them, so the stored choice stays a valid index
     * and comes back with Layer) */
    static const char *const TIMBRES_SINGLE[2] = { "N/A (Single)", "N/A (Single)" };
    static const char *const MODFX_TYPES[3] = { "Chorus/Flanger", "Ensemble", "Phaser" };
    static const char *const MODFX_TYPES_SHORT[3] = { "CHO", "ENS", "PHS" };
    static const char *const DELAY_TYPES[3] = { "Stereo", "Cross", "L/R" };
    static const char *const DELAY_TYPES_SHORT[3] = { "ST", "CRS", "L/R" };
    static const char *const TIMBRES_SINGLE_SHORT[2] = { "N/A", "N/A" };
    char num[160];
    int badge = timbre_badge(synth);
    buf[0] = '\0';
    bank_names[0] = "Built-in";
    for (int i = 0; i < g_syx_bank_count; i++) bank_names[i + 1] = g_syx_banks[i].name;

    json_put(&o, "[");
    const char *cat_names[8];
    category_names(synth, cat_names);
    json_put_enum(&o, "category", "Category", cat_names, NULL, synth->play_ncat, 0);
    json_put_patch_enum(&o, synth);
    json_put_enum(&o, "bank_file", "Bank", bank_names, NULL, g_syx_bank_count + 1, 0);
    json_put_enum(&o, "voice_mode", "Mode", VOICE_MODES, VOICE_MODES_SHORT, 2, 0);
    json_put_enum(&o, "timbre_edit", "Layer", badge ? TIMBRES : TIMBRES_SINGLE,
                  badge ? TIMBRES_SHORT : TIMBRES_SINGLE_SHORT, 2, 0);
    for (size_t i = 0; i < sizeof SELECTORS / sizeof SELECTORS[0]; i++) {
        const selector_t *s = &SELECTORS[i];
        json_put_enum(&o, s->key, s->name, s->options, s->shorts, s->count, badge);
    }
    /* DWGS: the names for the overlay, the hardware's wave numbers for the cell's option square */
    json_put_head(&o, "dwgs_wave", "DWGS Wave", badge);
    json_put(&o, ",\"type\":\"enum\",\"options\":[");
    for (int i = 0; i < DWGS_WAVE_COUNT; i++) {
        snprintf(num, sizeof num, "%s\"%d %s\"", i ? "," : "", i + 1, DWGS_NAMES[i]);
        json_put(&o, num);
    }
    json_put(&o, "],\"short_options\":[");
    for (int i = 0; i < DWGS_WAVE_COUNT; i++) {
        snprintf(num, sizeof num, "%s\"%d\"", i ? "," : "", i + 1);
        json_put(&o, num);
    }
    json_put(&o, "],\"default\":0},");
    /* Control 1 is the pulse's width on the Pulse wave: label and cell text follow Wave 1 (the layer's prefix
     * as for every per-layer control) */
    {
        const int pw = pulse_knob_shown(synth);
        char label[32], cell[16];
        if (badge) {
            snprintf(label, sizeof label, "L%d %s", badge, pw ? "Pulse Width" : "Control 1");
            snprintf(cell, sizeof cell, "L%d.%s", badge, pw ? "PW" : "CT1");
        } else {
            snprintf(label, sizeof label, "%s", pw ? "Pulse Width" : "Control 1");
            snprintf(cell, sizeof cell, "%s", pw ? "PW" : "CTL1");
        }
        json_put(&o, "{\"key\":\"osc1_ctrl1\",\"name\":");
        json_put_string(&o, pw ? "Pulse Width" : "Control 1");
        json_put(&o, ",\"label\":");
        json_put_string(&o, label);
        json_put(&o, ",\"short_name\":");
        json_put_string(&o, cell);
        json_put(&o, ",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0},");
    }
    json_put_head(&o, "osc1_ctrl2", "Control 2", badge);
    json_put(&o, ",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0},");
    json_put_enum(&o, "modfx_type", "Mod FX Type", MODFX_TYPES, MODFX_TYPES_SHORT, 3, 0);
    json_put_enum(&o, "delay_type", "Delay Type", DELAY_TYPES, DELAY_TYPES_SHORT, 3, 0);
    json_put(&o, "{\"key\":\"modfx_speed\",\"name\":\"Mod FX Speed\",\"short_name\":\"SPEED\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.3},");
    json_put_head(&o, "osc2_semi", "Semi", badge);
    json_put(&o, ",\"type\":\"int\",\"min\":-24,\"max\":24,\"default\":0,\"unit\":\"st\"},");
    json_put_head(&o, "osc2_tune", "Tune", badge);
    json_put(&o, ",\"type\":\"int\",\"min\":-50,\"max\":50,\"default\":0,\"unit\":\"ct\"},");
    json_put_head(&o, "level", "Level", badge);
    json_put(&o, ",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.9},");
    json_put_head(&o, "noise_level", "Noise", badge);
    json_put(&o, ",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0},");
    json_put(&o, "{\"key\":\"arp_on\",\"name\":\"Arp\",\"short_name\":\"ARP\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],"
                 "\"short_options\":[\"OFF\",\"ON\"],\"default\":0},");
    for (int i = 0; i < ARP_PARAM_COUNT; i++) {
        const arp_param_t *p = &ARP_PARAMS[i];
        if (p->options) {
            json_put_enum(&o, p->key, p->name, p->options, p->shorts, p->count, 0);
        } else {
            json_put_head(&o, p->key, p->name, 0);
            snprintf(num, sizeof num, ",\"short_name\":\"%s\",\"type\":\"int\",\"min\":%d,\"max\":%d,\"default\":0,\"unit\":\"%s\"},",
                     p->short_name, p->min, p->max, p->unit);
            json_put(&o, num);
        }
    }
    /* The steps draw as the module's LED boxes (canvas.js, custom:tinyk_step), which read arp_playhead too */
    for (int i = 1; i <= 8; i++) {
        snprintf(num, sizeof num, "{\"key\":\"arp_step%d\",\"name\":\"Step %d\",\"short_name\":\"ST%d\",\"type\":\"enum\","
                 "\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,", i, i, i);
        json_put(&o, num);
        json_put(&o, "\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},");
    }
    json_put(&o, "{\"key\":\"arp_playhead\",\"name\":\"Arp Playhead\",\"type\":\"string\",\"access\":\"read\"},");
    json_put(&o, "{\"key\":\"mod_wheel\",\"name\":\"Mod Wheel\",\"short_name\":\"MOD\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0},");
    json_put(&o, "{\"key\":\"timbre_balance\",\"name\":\"Layer Bal\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5}");
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (i == PARAM_VOICE_MODE || i == PARAM_TIMBRE_EDIT || i == PARAM_TIMBRE_BALANCE ||
            find_selector(PARAM_METAS[i].id)) continue;
        json_put(&o, ",");
        json_put_head(&o, PARAM_METAS[i].id, PARAM_METAS[i].name, badge);
        snprintf(num, sizeof num, ",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":%.3f}", PARAM_METAS[i].def_val);
        json_put(&o, num);
    }
    json_put(&o, "]");
    return buf;
}

static void json_put_floats(json_out_t *o, const char *key, const float *v, int n) {
    char num[32];
    json_put(o, ",\"");
    json_put(o, key);
    json_put(o, "\":[");
    for (int i = 0; i < n; i++) {
        snprintf(num, sizeof num, i ? ",%.6g" : "%.6g", v[i]);
        json_put(o, num);
    }
    json_put(o, "]");
}

/* get_param("state"), see apply_state. The length it would need, like snprintf. */
static int build_state_json(const synth_engine_t *synth, char *tmp, size_t cap, char *buf, int buf_len) {
    json_out_t o = { tmp, cap, 0 };
    char num[200];
    float ex[STATE_EXTRA_COUNT];
    tmp[0] = '\0';
    snprintf(num, sizeof num, "{\"tinyk_state\":%d,\"bank\":", STATE_VERSION);
    json_put(&o, num);
    json_put_string(&o, active_bank_name(synth));
    snprintf(num, sizeof num, ",\"preset\":%d,\"voice_mode\":%d,\"timbre_edit\":%d,\"timbre_balance\":%.6g,"
             "\"octave\":%d,\"delay_sync\":%d,\"arp_on\":%d,\"arp_pattern\":%d,\"arp_length\":%d", synth->current_preset,
             synth->voice_mode, synth->timbre_edit, synth->timbre_balance, synth->octave_transpose, synth->delay_sync_note,
             synth->arp.set.on, synth->arp.set.pattern, synth->arp.set.length);
    json_put(&o, num);
    snprintf(num, sizeof num, ",\"eq_low_freq\":%d,\"eq_low_gain\":%d,\"eq_hi_freq\":%d,\"eq_hi_gain\":%d,\"delay_type\":%d"
             ",\"modfx_type\":%d,\"modfx_speed\":%.6g",
             synth->eq_freq[0], synth->eq_gain[0], synth->eq_freq[1], synth->eq_gain[1], synth->delay_type,
             synth->modfx_type, synth->modfx_speed);
    json_put(&o, num);
    for (int i = 0; i < ARP_PARAM_COUNT; i++) {
        snprintf(num, sizeof num, ",\"%s\":%d", ARP_PARAMS[i].key, arp_param_get(synth, i));
        json_put(&o, num);
    }
    json_put_floats(&o, "params", synth->params, NUM_PARAMS);
    json_put_floats(&o, "timbre1", synth->timbre_params[0], NUM_PARAMS);
    json_put_floats(&o, "timbre2", synth->timbre_params[1], NUM_PARAMS);
    extra_to_floats(&synth->timbre_extra[0], ex);
    json_put_floats(&o, "extra1", ex, STATE_EXTRA_COUNT);
    extra_to_floats(&synth->timbre_extra[1], ex);
    json_put_floats(&o, "extra2", ex, STATE_EXTRA_COUNT);
    json_put(&o, "}");
    return snprintf(buf, buf_len, "%s", tmp);
}

/* UI hierarchy served to the host: GENERATED from src/module.json's ui_hierarchy (keep them identical) */
static const char MK_UI_HIERARCHY[] =
    "{\"levels\":{\"root\":{\"name\":\"TinyK\",\"label\":\"TinyK\",\"list_param\":\"preset\",\"count_param\":\"preset_count\",\"name_pa"
    "ram\":\"preset_name\",\"params\":[{\"level\":\"perf\",\"label\":\"Perf\"},{\"level\":\"osc\",\"label\":\"Osc\"},{\"level\":\"filte"
    "r\",\"label\":\"Filter\"},{\"level\":\"amp\",\"label\":\"Amp\"},{\"level\":\"mod\",\"label\":\"Mod\"},{\"level\":\"fx\",\"label\":"
    "\"Effects\"},{\"level\":\"arpset\",\"label\":\"Arp Settings\"},{\"level\":\"steps\",\"label\":\"Arp Steps\"},{\"level\":\"bank\","
    "\"label\":\"Bank\"}],\"knobs\":[]},\"perf\":{\"name\":\"Perf\",\"label\":\"Perf\",\"params\":[{\"key\":\"category\",\"label\":\"Ca"
    "tegory\",\"type\":\"enum\",\"options\":[\"Trance\",\"Techno/House\",\"Electronica\",\"DnB/Breaks\",\"Hiphop/Vintage\",\"Retro\",\""
    "SE/Hit\"],\"default\":0,\"short_name\":\"CAT\"},{\"key\":\"patch\",\"label\":\"Program\",\"short_name\":\"PROG\",\"type\":\"enum\""
    ",\"options\":[\"A1\",\"A2\",\"A3\",\"A4\",\"A5\",\"A6\",\"A7\",\"A8\",\"B1\",\"B2\",\"B3\",\"B4\",\"B5\",\"B6\",\"B7\",\"B8\"],\"d"
    "efault\":0},{\"key\":\"arp_on\",\"label\":\"Arp\",\"short_name\":\"ARP\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"short_op"
    "tions\":[\"OFF\",\"ON\"],\"default\":0},{\"key\":\"voice_mode\",\"label\":\"Mode\",\"type\":\"enum\",\"options\":[\"Single\",\"Lay"
    "er\"],\"short_options\":[\"SNGL\",\"LAYR\"],\"default\":0,\"short_name\":\"MODE\"},{\"key\":\"cutoff\",\"label\":\"Cutoff\",\"type"
    "\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.7,\"step\":0.01,\"short_name\":\"CUT\"},{\"key\":\"resonance\",\"label\":\"Reso"
    "nance\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01,\"short_name\":\"RES\"},{\"key\":\"release2\",\"l"
    "abel\":\"Amp Rel\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01,\"short_name\":\"REL\"},{\"key\":\"tim"
    "bre_edit\",\"label\":\"Layer\",\"type\":\"enum\",\"options\":[\"Layer 1\",\"Layer 2\"],\"short_options\":[\"L1\",\"L2\"],\"default"
    "\":0,\"short_name\":\"LAYER\"}],\"knobs\":[\"category\",\"patch\",\"arp_on\",\"voice_mode\",\"cutoff\",\"resonance\",\"release2\","
    "\"timbre_edit\"]},\"osc\":{\"name\":\"Osc [L1]\",\"label\":\"Osc\",\"params\":[{\"key\":\"wave1\",\"label\":\"Wave 1\",\"type\":\""
    "enum\",\"options\":[\"Saw\",\"Square\",\"Triangle\",\"Sine\",\"Vox\",\"DWGS\",\"Noise\"],\"short_options\":[\"SAW\",\"SQR\",\"TRI"
    "\",\"SIN\",\"VOX\",\"DWG\",\"NZ\"],\"default\":0},{\"key\":\"osc1_ctrl1\",\"label\":\"Control 1\",\"short_name\":\"CTL1\",\"type\""
    ":\"int\",\"min\":0,\"max\":127,\"default\":0},{\"key\":\"dwgs_wave\",\"label\":\"DWGS Wave\",\"short_name\":\"DWGS\",\"type\":\"en"
    "um\",\"options\":[\"1 SynSine1\",\"2 SynSine2\",\"3 SynSine3\",\"4 SynSine4\",\"5 SynSine5\",\"6 SynSine6\",\"7 SynSine7\",\"8 Syn"
    "Bass1\",\"9 SynBass2\",\"10 SynBass3\",\"11 SynBass4\",\"12 SynBass5\",\"13 SynBass6\",\"14 SynBass7\",\"15 SynWave1\",\"16 SynWav"
    "e2\",\"17 SynWave3\",\"18 SynWave4\",\"19 SynWave5\",\"20 SynWave6\",\"21 SynWave7\",\"22 SynWave8\",\"23 SynWave9\",\"24 5thWave1"
    "\",\"25 5thWave2\",\"26 5thWave3\",\"27 Digi1\",\"28 Digi2\",\"29 Digi3\",\"30 Digi4\",\"31 Digi5\",\"32 Digi6\",\"33 Digi7\",\"34"
    " Digi8\",\"35 Endless\",\"36 E.Piano1\",\"37 E.Piano2\",\"38 E.Piano3\",\"39 E.Piano4\",\"40 Organ1\",\"41 Organ2\",\"42 Organ3\","
    "\"43 Organ4\",\"44 Organ5\",\"45 Organ6\",\"46 Organ7\",\"47 Clav1\",\"48 Clav2\",\"49 Guitar1\",\"50 Guitar2\",\"51 Guitar3\",\"5"
    "2 Bass1\",\"53 Bass2\",\"54 Bass3\",\"55 Bass4\",\"56 Bass5\",\"57 Bell1\",\"58 Bell2\",\"59 Bell3\",\"60 Bell4\",\"61 Voice1\",\""
    "62 Voice2\",\"63 Voice3\",\"64 Voice4\"],\"short_options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\",\"10\",\"11\",\""
    "12\",\"13\",\"14\",\"15\",\"16\",\"17\",\"18\",\"19\",\"20\",\"21\",\"22\",\"23\",\"24\",\"25\",\"26\",\"27\",\"28\",\"29\",\"30\""
    ",\"31\",\"32\",\"33\",\"34\",\"35\",\"36\",\"37\",\"38\",\"39\",\"40\",\"41\",\"42\",\"43\",\"44\",\"45\",\"46\",\"47\",\"48\",\"4"
    "9\",\"50\",\"51\",\"52\",\"53\",\"54\",\"55\",\"56\",\"57\",\"58\",\"59\",\"60\",\"61\",\"62\",\"63\",\"64\"],\"default\":0},{\"ke"
    "y\":\"osc1_ctrl2\",\"label\":\"Control 2\",\"short_name\":\"CTL2\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0},{\"key\":"
    "\"wave2\",\"label\":\"Wave 2\",\"type\":\"enum\",\"options\":[\"Saw\",\"Square\",\"Triangle\"],\"short_options\":[\"SAW\",\"SQR\","
    "\"TRI\"],\"default\":0},{\"key\":\"sync_ring\",\"label\":\"Sync / Ring\",\"type\":\"enum\",\"options\":[\"Off\",\"Ring\",\"Sync\","
    "\"Ring Sync\"],\"short_options\":[\"OFF\",\"RING\",\"SYNC\",\"R.SNC\"],\"default\":0},{\"key\":\"osc2_semi\",\"label\":\"Semi\",\""
    "type\":\"int\",\"min\":-24,\"max\":24,\"default\":0,\"unit\":\"st\"},{\"key\":\"osc2_tune\",\"label\":\"Tune\",\"type\":\"int\",\""
    "min\":-50,\"max\":50,\"default\":0,\"unit\":\"ct\"},{\"key\":\"osc_mix\",\"label\":\"Osc Mix\",\"type\":\"float\",\"min\":0.0,\"ma"
    "x\":1.0,\"default\":0.5,\"step\":0.01}],\"knobs\":[\"wave1\",\"osc1_ctrl1\",\"osc1_ctrl2\",\"wave2\",\"sync_ring\",\"osc2_semi\","
    "\"osc2_tune\",\"osc_mix\"]},\"filter\":{\"name\":\"Filter [L1]\",\"label\":\"Filter\",\"params\":[{\"key\":\"filter_type\",\"label"
    "\":\"Filter Type\",\"type\":\"enum\",\"options\":[\"LPF24\",\"LPF12\",\"BPF12\",\"HPF12\"],\"short_options\":[\"LPF24\",\"LPF12\","
    "\"BPF12\",\"HPF12\"],\"default\":0},{\"key\":\"cutoff\",\"label\":\"Cutoff\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default"
    "\":0.7,\"step\":0.01,\"short_name\":\"CUT\"},{\"key\":\"resonance\",\"label\":\"Resonance\",\"type\":\"float\",\"min\":0.0,\"max\""
    ":1.0,\"default\":0.2,\"step\":0.01,\"short_name\":\"RES\"},{\"key\":\"env_int\",\"label\":\"EG Int\",\"type\":\"float\",\"min\":0."
    "0,\"max\":1.0,\"default\":0.5,\"step\":0.01},{\"key\":\"attack1\",\"label\":\"Filter Atk\",\"type\":\"float\",\"min\":0.0,\"max\":"
    "1.0,\"default\":0.01,\"step\":0.01},{\"key\":\"decay1\",\"label\":\"Filter Dcy\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"defa"
    "ult\":0.4,\"step\":0.01},{\"key\":\"sustain1\",\"label\":\"Filter Sus\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5"
    ",\"step\":0.01},{\"key\":\"release1\",\"label\":\"Filter Rel\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\""
    ":0.01}],\"knobs\":[\"filter_type\",\"cutoff\",\"resonance\",\"env_int\",\"attack1\",\"decay1\",\"sustain1\",\"release1\"]},\"amp\""
    ":{\"name\":\"Amp [L1]\",\"label\":\"Amp\",\"params\":[{\"key\":\"noise_level\",\"label\":\"Noise\",\"type\":\"float\",\"min\":0.0,"
    "\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"level\",\"label\":\"Level\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"def"
    "ault\":0.9,\"step\":0.01},{\"key\":\"drive\",\"label\":\"Distortion\",\"short_name\":\"DIST\",\"type\":\"enum\",\"options\":[\"Off"
    "\",\"On\"],\"short_options\":[\"OFF\",\"ON\"],\"default\":0},{\"key\":\"pan\",\"label\":\"Pan\",\"type\":\"float\",\"min\":0.0,\"m"
    "ax\":1.0,\"default\":0.5,\"step\":0.01},{\"key\":\"attack2\",\"label\":\"Amp Attack\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,"
    "\"default\":0.01,\"step\":0.01},{\"key\":\"decay2\",\"label\":\"Amp Dcy\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0"
    ".5,\"step\":0.01},{\"key\":\"sustain2\",\"label\":\"Amp Sus\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.8,\"step\":"
    "0.01},{\"key\":\"release2\",\"label\":\"Amp Rel\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01,\"short"
    "_name\":\"REL\"}],\"knobs\":[\"noise_level\",\"level\",\"drive\",\"pan\",\"attack2\",\"decay2\",\"sustain2\",\"release2\"]},\"mod"
    "\":{\"name\":\"Mod\",\"label\":\"Mod\",\"params\":[{\"key\":\"lfo1_rate\",\"label\":\"LFO1 Rate\",\"type\":\"float\",\"min\":0.0,"
    "\"max\":1.0,\"default\":0.3,\"step\":0.01,\"short_name\":\"LFO1\"},{\"key\":\"lfo2_rate\",\"label\":\"LFO2 Rate\",\"type\":\"float"
    "\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01,\"short_name\":\"LFO2\"},{\"key\":\"mod_wheel\",\"label\":\"Mod Wheel\","
    "\"short_name\":\"MOD\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0},{\"key\":\"keytrack\",\"label\":\"Filter Key Trk\",\""
    "type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},{\"key\":\"voice_assign\",\"label\":\"Voice\",\"short_name"
    "\":\"VOICE\",\"type\":\"enum\",\"options\":[\"Mono\",\"Poly\",\"Unison\"],\"short_options\":[\"MONO\",\"POLY\",\"UNIS\"],\"default"
    "\":1},{\"key\":\"portamento\",\"label\":\"Portamento\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{"
    "\"key\":\"timbre_balance\",\"label\":\"Layer Bal\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01,\"shor"
    "t_name\":\"BAL\"}],\"knobs\":[\"lfo1_rate\",\"lfo2_rate\",\"mod_wheel\",\"keytrack\",\"voice_assign\",\"portamento\",\"timbre_bala"
    "nce\"]},\"fx\":{\"name\":\"Effects\",\"label\":\"Effects\",\"params\":[{\"key\":\"modfx_type\",\"label\":\"Mod FX Type\",\"short_n"
    "ame\":\"FX\",\"type\":\"enum\",\"options\":[\"Chorus/Flanger\",\"Ensemble\",\"Phaser\"],\"short_options\":[\"CHO\",\"ENS\",\"PHS\""
    "],\"default\":0},{\"key\":\"modfx_speed\",\"label\":\"Mod FX Speed\",\"short_name\":\"SPEED\",\"type\":\"float\",\"min\":0.0,\"max"
    "\":1.0,\"default\":0.3,\"step\":0.01},{\"key\":\"chorus_mix\",\"label\":\"Mod FX Depth\",\"type\":\"float\",\"min\":0.0,\"max\":1."
    "0,\"default\":0.0,\"step\":0.01},{\"key\":\"delay_type\",\"label\":\"Delay Type\",\"short_name\":\"D.TYP\",\"type\":\"enum\",\"opt"
    "ions\":[\"Stereo\",\"Cross\",\"L/R\"],\"short_options\":[\"ST\",\"CRS\",\"L/R\"],\"default\":0},{\"key\":\"delay_time\",\"label\":"
    "\"Delay Time\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01},{\"key\":\"delay_feedback\",\"label\":\"D"
    "elay Fdbk\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01},{\"key\":\"delay_mix\",\"label\":\"Delay Mix"
    "\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"master_vol\",\"label\":\"Master Vol\",\"ty"
    "pe\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.8,\"step\":0.01,\"short_name\":\"VOL\"}],\"knobs\":[\"modfx_type\",\"modfx_sp"
    "eed\",\"chorus_mix\",\"delay_type\",\"delay_time\",\"delay_feedback\",\"delay_mix\",\"master_vol\"]},\"arpset\":{\"name\":\"Arp Se"
    "ttings\",\"label\":\"Arp Settings\",\"params\":[{\"key\":\"arp_type\",\"label\":\"Type\",\"short_name\":\"TYPE\",\"type\":\"enum\""
    ",\"options\":[\"UP\",\"DOWN\",\"ALT1\",\"ALT2\",\"RANDOM\",\"TRIGGER\"],\"short_options\":[\"UP\",\"DOWN\",\"ALT1\",\"ALT2\",\"RND"
    "\",\"TRIG\"],\"default\":0},{\"key\":\"arp_range\",\"label\":\"Range\",\"short_name\":\"RANGE\",\"type\":\"enum\",\"options\":[\"1"
    " Oct\",\"2 Oct\",\"3 Oct\",\"4 Oct\"],\"short_options\":[\"1OCT\",\"2OCT\",\"3OCT\",\"4OCT\"],\"default\":0},{\"key\":\"arp_resolu"
    "tion\",\"label\":\"Resolution\",\"short_name\":\"RESO\",\"type\":\"enum\",\"options\":[\"1/24\",\"1/16\",\"1/12\",\"1/8\",\"1/6\","
    "\"1/4\"],\"short_options\":[\"1/24\",\"1/16\",\"1/12\",\"1/8\",\"1/6\",\"1/4\"],\"default\":1},{\"key\":\"arp_gate\",\"label\":\"G"
    "ate\",\"short_name\":\"GATE\",\"type\":\"int\",\"min\":0,\"max\":100,\"default\":80,\"unit\":\"%\"},{\"key\":\"arp_swing\",\"label"
    "\":\"Swing\",\"short_name\":\"SWING\",\"type\":\"int\",\"min\":-100,\"max\":100,\"default\":0,\"unit\":\"%\"},{\"key\":\"arp_latch"
    "\",\"label\":\"Latch\",\"short_name\":\"LATCH\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"short_options\":[\"OFF\",\"ON\"],"
    "\"default\":0},{\"key\":\"arp_key_sync\",\"label\":\"Key Sync\",\"short_name\":\"KSYNC\",\"type\":\"enum\",\"options\":[\"Off\",\""
    "On\"],\"short_options\":[\"OFF\",\"ON\"],\"default\":0},{\"key\":\"arp_target\",\"label\":\"Target\",\"short_name\":\"TARGT\",\"ty"
    "pe\":\"enum\",\"options\":[\"Both\",\"Layer 1\",\"Layer 2\"],\"short_options\":[\"BOTH\",\"L1\",\"L2\"],\"default\":0}],\"knobs\":"
    "[\"arp_type\",\"arp_range\",\"arp_resolution\",\"arp_gate\",\"arp_swing\",\"arp_latch\",\"arp_key_sync\",\"arp_target\"]},\"steps"
    "\":{\"name\":\"Arp Steps\",\"label\":\"Arp Steps\",\"params\":[{\"key\":\"arp_step1\",\"label\":\"Step 1\",\"short_name\":\"ST1\","
    "\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:t"
    "inyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step2\",\"label\":\"Step 2\",\"short_name\":\"ST2\",\"type\":\"enum"
    "\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"e"
    "xtra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step3\",\"label\":\"Step 3\",\"short_name\":\"ST3\",\"type\":\"enum\",\"options\":"
    "[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\""
    "arp_playhead\"]}},{\"key\":\"arp_step4\",\"label\":\"Step 4\",\"short_name\":\"ST4\",\"type\":\"enum\",\"options\":[\"Rest\",\"Pla"
    "y\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]"
    "}},{\"key\":\"arp_step5\",\"label\":\"Step 5\",\"short_name\":\"ST5\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_op"
    "tions\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"a"
    "rp_step6\",\"label\":\"Step 6\",\"short_name\":\"ST6\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST"
    "\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step7\",\"la"
    "bel\":\"Step 7\",\"short_name\":\"ST7\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\""
    "default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step8\",\"label\":\"Step 8"
    "\",\"short_name\":\"ST8\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\""
    "viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}}],\"knobs\":[\"arp_step1\",\"arp_step2\",\"arp_step3\",\""
    "arp_step4\",\"arp_step5\",\"arp_step6\",\"arp_step7\",\"arp_step8\"]},\"bank\":{\"name\":\"Bank\",\"label\":\"Bank\",\"params\":[{"
    "\"key\":\"bank_file\",\"label\":\"Bank\",\"type\":\"enum\",\"options\":[\"Built-in\"],\"default\":0},{\"level\":\"bank_list\",\"la"
    "bel\":\"Browse banks\"}],\"knobs\":[\"bank_file\"]},\"bank_list\":{\"name\":\"Banks\",\"label\":\"Select Bank\",\"items_param\":\""
    "bank_list\",\"select_param\":\"bank_file\",\"navigate_to\":\"root\"}}}";

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    tinyk_instance_t *inst = (tinyk_instance_t*)instance;
    if (!inst || !key || !buf || buf_len <= 0) return -1;
    synth_engine_t *synth = &inst->synth;

    if (strcmp(key, "ui_hierarchy") == 0) {
        /* The per-layer pages carry the edited layer in their name ("Osc [L1]"): "[L2]" while Layer 2 is
         * edited. A rename keeps the page on screen: the host re-anchors by level. */
        int n = snprintf(buf, buf_len, "%s", MK_UI_HIERARCHY);
        if (edit_timbre(synth) == 1 && n < buf_len) {
            for (char *p = strstr(buf, "[L1]"); p; p = strstr(p + 4, "[L1]")) p[2] = '2';
        }
        /* Osc, second knob: Control 1, or the DWGS selector while Wave 1 is DWGS (Control 1 does nothing on a
         * DWGS wave, as on the hardware). The manifest lists both entries; one of the two is cut from what is
         * served, so the page never shows both. There is no separate list page: the knob shows number and
         * name (the dwgs_pick / dwgs_list params remain for hosts and tools).
         * The host maps the knob afresh when it re-reads the hierarchy (is_loading, see label_context). */
        if (n < buf_len) {
            int dw = dwgs_knob_shown(synth);
            if (dw) json_swap(buf, "\"knobs\":[\"wave1\",\"osc1_ctrl1\"", "\"knobs\":[\"wave1\",\"dwgs_wave\"");
            json_cut_object(buf, dw ? "{\"key\":\"osc1_ctrl1\"" : "{\"key\":\"dwgs_wave\"");
            n = (int)strlen(buf);
        }
        return n;
    }

    if (strcmp(key, "state") == 0) {
        return build_state_json(synth, inst->json, sizeof inst->json, buf, buf_len);
    }

    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "TinyK");
    }

    if (strcmp(key, "abbrev") == 0) {
        return snprintf(buf, buf_len, "TINYK");
    }

    if (strcmp(key, "bank_side") == 0) {
        return snprintf(buf, buf_len, "%d", (synth->program_num > 8) ? 1 : 0);
    }

    if (strcmp(key, "genre_category") == 0) {
        return snprintf(buf, buf_len, "%d", synth->genre_category);
    }

    if (strcmp(key, "program_num") == 0) {
        return snprintf(buf, buf_len, "%d", synth->program_num);
    }

    if (strcmp(key, "program") == 0) {
        return snprintf(buf, buf_len, "%d", program_number(synth));
    }

    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        return snprintf(buf, buf_len, "%d", synth->voice_mode);
    }

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 || strcmp(key, "Layer") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        return snprintf(buf, buf_len, "%d", synth->timbre_edit);
    }

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 || strcmp(key, "Layer Bal") == 0 ||
        strcmp(key, "Balance") == 0 || strcmp(key, "4") == 0 || strcmp(key, "param_4") == 0) {
        return snprintf(buf, buf_len, "%.4f", synth->timbre_balance);
    }

    if (strcmp(key, "preset") == 0) { /* the jog wheel's index: the program's ordinal among the playable ones */
        return snprintf(buf, buf_len, "%d", synth->play_ord[synth->current_preset]);
    }

    if (strcmp(key, "preset_count") == 0) { /* playable programs: vocoder programs are not offered */
        return snprintf(buf, buf_len, "%d", synth->play_n);
    }

    if (strcmp(key, "patch_count") == 0) { /* programs in the current category */
        return snprintf(buf, buf_len, "%d", category_patch_count(synth, synth->genre_category));
    }

    if (strcmp(key, "category_count") == 0) {
        return snprintf(buf, buf_len, "%d", synth->play_ncat);
    }

    if (strcmp(key, "category_names") == 0) { /* for the module's own screen: ["Trance", ...] */
        const char *names[8];
        category_names(synth, names);
        int n = snprintf(buf, buf_len, "[");
        for (int c = 0; c < synth->play_ncat && n < buf_len; c++) n += snprintf(buf + n, buf_len - n, "%s\"%s\"", c ? "," : "", names[c]);
        return n < buf_len ? n + snprintf(buf + n, buf_len - n, "]") : n;
    }

    if (strcmp(key, "preset_name") == 0 || strcmp(key, "patch_in_bank") == 0 || strcmp(key, "program_name") == 0) {
        return format_preset_name(synth, synth->current_preset, buf, buf_len);
    }

    if (strcmp(key, "bank_file") == 0) {
        return snprintf(buf, buf_len, "%d", synth->bank_file);
    }

    if (strcmp(key, "category") == 0) {
        return snprintf(buf, buf_len, "%d", synth->genre_category);
    }

    if (strcmp(key, "patch") == 0) {
        return snprintf(buf, buf_len, "%d", current_patch(synth));
    }

    if (strcmp(key, "bank_list") == 0) { /* items for the Banks page: [{"index":0,"label":"Built-in"},...] */
        return snprintf(buf, buf_len, "%s", build_bank_list_json(inst->json, sizeof inst->json));
    }

    if (strcmp(key, "chain_params") == 0) {
        synth->labels_reported = label_context(synth); /* the host now has these labels */
        return snprintf(buf, buf_len, "%s", build_chain_params_json(synth, inst->json, sizeof inst->json));
    }

    if (strcmp(key, "is_loading") == 0) { /* "1" once per unreported Program label change, see synth->labels_reported */
        int ctx = label_context(synth);
        int changed = (ctx != synth->labels_reported);
        synth->labels_reported = ctx;
        return snprintf(buf, buf_len, "%s", changed ? "1" : "0");
    }

    if (strcmp(key, "bank_file_name") == 0) {
        return snprintf(buf, buf_len, "%s", active_bank_name(synth));
    }

    if (strcmp(key, "bank_file_count") == 0) { /* including the built-in bank */
        return snprintf(buf, buf_len, "%d", g_syx_bank_count + 1);
    }

    if (strncmp(key, "bank_file_name:", 15) == 0) {
        int b = atoi(key + 15);
        if (b == 0) return snprintf(buf, buf_len, "Built-in");
        if (b > 0 && b <= g_syx_bank_count) return snprintf(buf, buf_len, "%s", g_syx_banks[b - 1].name);
        return -1;
    }

    if (strncmp(key, "preset_name:", 12) == 0) {
        int idx = atoi(key + 12); /* an ordinal, like preset */
        if (idx >= 0 && idx < synth->play_n) {
            return format_preset_name(synth, synth->play_map[idx], buf, buf_len);
        }
        return -1;
    }

    if (strcmp(key, "bank_name") == 0) {
        return snprintf(buf, buf_len, "%s", (synth->program_num > 8) ? "Side B" : "Side A");
    }

    if (strcmp(key, "bank_count") == 0) {
        return snprintf(buf, buf_len, "2");
    }

    if (strcmp(key, "bank_index") == 0) {
        return snprintf(buf, buf_len, "%d", (synth->program_num > 8) ? 1 : 0);
    }

    if (strcmp(key, "octave_transpose") == 0) {
        return snprintf(buf, buf_len, "%d", synth->octave_transpose);
    }

    const selector_t *sel = find_selector(key);
    if (sel) { /* option index */
        int n = sel->count;
        int mode = (int)lroundf(synth_get_param(synth, key) * (float)(n - 1));
        if (mode < 0) mode = 0;
        if (mode > n - 1) mode = n - 1;
        return snprintf(buf, buf_len, "%d", sel->to_engine ? sel->to_engine[mode] : mode);
    }

    if (strcmp(key, "osc2_semi") == 0 || strcmp(key, "osc2_tune") == 0) {
        return snprintf(buf, buf_len, "%d", (int)synth_get_param(synth, key));
    }

    if (strcmp(key, "mod_wheel") == 0) {
        return snprintf(buf, buf_len, "%d", (int)lroundf(synth_get_param(synth, key)));
    }

    if (strcmp(key, "arp_on") == 0) {
        return snprintf(buf, buf_len, "%d", synth->arp.set.on);
    }

    if (strncmp(key, "arp_step", 8) == 0 && key[8] >= '1' && key[8] <= '8' && key[9] == '\0') {
        return snprintf(buf, buf_len, "%d", (int)synth_get_param(synth, key));
    }

    if (strcmp(key, "arp_playhead") == 0) { /* for the Arp Steps LEDs, see format_arp_playhead */
        return format_arp_playhead(synth, buf, buf_len);
    }

    if (arp_param_index(key) >= 0) {
        return snprintf(buf, buf_len, "%d", arp_param_get(synth, arp_param_index(key)));
    }

    if (strcmp(key, "level") == 0 || strcmp(key, "noise_level") == 0) {
        return snprintf(buf, buf_len, "%.4f", synth_get_param(synth, key));
    }

    if (strcmp(key, "dwgs_wave") == 0 || strcmp(key, "dwgs_pick") == 0) {
        return snprintf(buf, buf_len, "%d", (int)synth_get_param(synth, key));
    }

    if (strcmp(key, "delay_type") == 0) return snprintf(buf, buf_len, "%d", synth->delay_type);
    if (strcmp(key, "modfx_type") == 0) return snprintf(buf, buf_len, "%d", synth->modfx_type);
    if (strcmp(key, "osc1_ctrl1") == 0 || strcmp(key, "osc1_ctrl2") == 0) {
        return snprintf(buf, buf_len, "%d", (int)synth_get_param(synth, key));
    }
    if (strncmp(key, "eq_", 3) == 0 && (strcmp(key + 3, "low_freq") == 0 || strcmp(key + 3, "low_gain") == 0 ||
                                        strcmp(key + 3, "hi_freq") == 0 || strcmp(key + 3, "hi_gain") == 0)) {
        return snprintf(buf, buf_len, "%d", (int)synth_get_param(synth, key));
    }

    if (strcmp(key, "dwgs_list") == 0) { /* items for the DWGS Waves page: [{"index":0,"label":"1 SynSine1"},...] */
        return snprintf(buf, buf_len, "%s", build_dwgs_list_json(inst->json, sizeof inst->json));
    }

    /* Check parameter values */
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(PARAM_METAS[i].id, key) == 0) {
            float val = synth->params[i];
            int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
            if (is_layer && i < PARAM_LFO1_RATE) {
                val = synth->timbre_params[synth->timbre_edit][i];
            }
            return snprintf(buf, buf_len, "%.4f", val);
        }
    }

    return -1;
}

static int v2_get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    (void)buf;
    (void)buf_len;
    return 0;
}

static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : default_synth();
    if (!out_interleaved_lr || frames <= 0) return;
    synth_render(synth, out_interleaved_lr, frames);
}

static void v2_audio_fx_process_block(void *instance, const int16_t *in_interleaved_lr, int16_t *out_interleaved_lr, int frames) {
    (void)in_interleaved_lr;
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : default_synth();
    if (!out_interleaved_lr || frames <= 0) return;
    synth_render(synth, out_interleaved_lr, frames);
}

static plugin_api_v2_t g_plugin_api_v2;

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    find_move_info();
    memset(&g_plugin_api_v2, 0, sizeof(g_plugin_api_v2));
    g_plugin_api_v2.api_version = MOVE_PLUGIN_API_VERSION_2;
    g_plugin_api_v2.create_instance = v2_create_instance;
    g_plugin_api_v2.destroy_instance = v2_destroy_instance;
    g_plugin_api_v2.on_midi = v2_on_midi;
    g_plugin_api_v2.set_param = v2_set_param;
    g_plugin_api_v2.get_param = v2_get_param;
    g_plugin_api_v2.get_error = v2_get_error;
    g_plugin_api_v2.render_block = v2_render_block;
    return &g_plugin_api_v2;
}

static audio_fx_api_v2_t g_audio_fx_api_v2;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    find_move_info();
    memset(&g_audio_fx_api_v2, 0, sizeof(g_audio_fx_api_v2));
    g_audio_fx_api_v2.api_version = 2;
    g_audio_fx_api_v2.create_instance = v2_create_instance;
    g_audio_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_audio_fx_api_v2.process_block = v2_audio_fx_process_block;
    g_audio_fx_api_v2.on_midi = v2_on_midi;
    g_audio_fx_api_v2.set_param = v2_set_param;
    g_audio_fx_api_v2.get_param = v2_get_param;
    g_audio_fx_api_v2.get_error = v2_get_error;
    return &g_audio_fx_api_v2;
}

/* Direct exported functions for host wrappers and raw callbacks */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    if ((uintptr_t)msg <= 1024) {
        int actual_len = (int)(uintptr_t)msg;
        const uint8_t *actual_msg = (const uint8_t*)instance;
        parse_midi_buffer(default_synth(), actual_msg, actual_len);
        return;
    }
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : default_synth();
    parse_midi_buffer(synth, msg, len);
}

void move_plugin_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    move_audio_fx_on_midi(instance, msg, len, source);
}

void move_audio_fx_process(void *instance, int16_t *out_interleaved_lr, int frames) {
    if ((uintptr_t)out_interleaved_lr <= 65536) {
        int actual_frames = (int)(uintptr_t)out_interleaved_lr;
        int16_t *actual_out = (int16_t*)instance;
        if (actual_out && actual_frames > 0) {
            synth_render(default_synth(), actual_out, actual_frames);
        }
        return;
    }
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : default_synth();
    if (out_interleaved_lr && frames > 0) {
        synth_render(synth, out_interleaved_lr, frames);
    }
}

#ifdef TINYK_TUNING
/* Test hooks: drive the module through the same v2 entry points the Schwung host calls */
static void *g_test_inst[2];
/* (Re)creates test instance n (0 or 1; two show that slots do not share state) */
int tinyk_dsp_v2_create_n(int n, const char *module_dir) {
    if (n < 0 || n > 1) return 0;
    v2_destroy_instance(g_test_inst[n]);
    g_test_inst[n] = v2_create_instance(module_dir, NULL);
    return g_test_inst[n] != NULL;
}
void tinyk_dsp_v2_set_n(int n, const char *key, const char *val) { if (n >= 0 && n <= 1 && g_test_inst[n]) v2_set_param(g_test_inst[n], key, val); }
int tinyk_dsp_v2_get_n(int n, const char *key, char *buf, int buf_len) {
    return (n >= 0 && n <= 1 && g_test_inst[n]) ? v2_get_param(g_test_inst[n], key, buf, buf_len) : -1;
}
void tinyk_dsp_v2_midi_n(int n, const uint8_t *msg, int len) { if (n >= 0 && n <= 1 && g_test_inst[n]) v2_on_midi(g_test_inst[n], msg, len, 0); }
void tinyk_dsp_v2_render_n(int n, int16_t *out, int frames) { if (n >= 0 && n <= 1 && g_test_inst[n]) v2_render_block(g_test_inst[n], out, frames); }
int tinyk_dsp_v2_create(const char *module_dir) { return tinyk_dsp_v2_create_n(0, module_dir); }
void tinyk_dsp_v2_set(const char *key, const char *val) { tinyk_dsp_v2_set_n(0, key, val); }
int tinyk_dsp_v2_get(const char *key, char *buf, int buf_len) { return tinyk_dsp_v2_get_n(0, key, buf, buf_len); }
#endif
