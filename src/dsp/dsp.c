#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* RTLD_DEFAULT */
#endif
#include "dsp.h"
#include "presets.h"
#include "syx_bank.h"

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

/* "A.11 Name" for preset idx of the active bank ("A.11" alone when the bank has no name for it) */
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

#define TINYK_TUNING_DEFAULTS { 37.46f, 10.61f, 20.0f, 19000.0f, 2.13f, 8.88f, 1.92f, 0.7f, 2.0f, 1.0f, 1.0f, 1.0f, 0.7f, 0.4f, 10.61f, 24.0f, 120.0f, 2.4f, 26.7f, 20000.0f, 0.6f, 10.36f, 0.56f, 2.0f, 18.0f, 360.0f, -24.5f, 15000.0f, 0.7f, 1.0f, 0.05f, 600.0f, 5.0f, 3.7f, 1.0f }
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
    { "vel_sens",     "Vel Sensitivity","VelSn",  2, 0.3f },

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
    { "chorus_mix",   "Chorus Mix",     "Chor",   4, 0.0f },
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

/* Note-on de-click: a voice starting from silence fades in over this many samples (raised cosine, 1 ms at 44.1 kHz),
 * on top of its amp EG. The EG's fastest attack (1.5 ms) starts at its steepest slope, and with random start phases
 * the waveform is rarely at zero there: that corner, lifted by the brightness tilt, was a click on every fast-attack
 * note. */
#ifndef DECLICK_SAMPLES
#define DECLICK_SAMPLES 44
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
        case 1: return (l->phase < 0.5f) ? 1.0f : -1.0f;
        case 2: return which == 0 ? 2.0f * fabsf(2.0f * l->phase - 1.0f) - 1.0f
                                  : sinf(2.0f * (float)M_PI * l->phase);
        default: return l->sh_value;
    }
}

/* --- Sine cross-modulation (Osc 1 Control 1 / 2 on the Sine wave, Osc 2 synced) ------------------------------------
 * On the microKORG the Sine wave's Control 1 is the cross-modulation depth and Control 2 how far LFO1 moves it.
 * With Osc 2 synced, the factory's drum kicks (A.21 AutoHouse T2 and the MS2000's Auto Disco / Zoop Mania, Ctrl 1/2 =
 * 1 / 12-13) use it as a pitch envelope: the LFO1 saw sweeps the sine 130 -> 33 Hz in ~180 ms (VST takes, C3 = note
 * 60, the sine's note pitch 63.6 Hz):
 *   ref_a21_timbre2_drum_c3            Ctrl 1 / 2 = 1 / 13: the sweep
 *   ref_a21_take1_ctrl2_zero           Ctrl 2 = 0: a steady 17.1 Hz (depth 1 alone), after the first note
 *   ref_a21_take2_osc2_semi_zero       Osc 2 semitone -24 -> 0: the same sweep, so Osc 2's wave is not the modulator
 * Measured, the sine's pitch is straight in octaves against the depth d = 127 * (ctrl1 + ctrl2 * lfo1): about +0.22
 * octave per step, the note's own pitch near d = 9.6 (xmod_semitones per unit of depth, xmod_offset_semitones at
 * depth 0; fitted by renders on the take traces, see tools/a21_kick_trace.py). That only holds where it was measured,
 * so it applies to synced Sine timbres whose depth stays within the measured span (ctrl1 + ctrl2 <= XMOD_MAX_DEPTH,
 * Ctrl 1 or 2 set); other cross-mod settings (audio-rate FM with Osc 2 free: A.13 X-ModBass, A.23 X-ModPerc; deep
 * synced settings) are not modelled and play the plain sine. */
#define XMOD_MAX_DEPTH (14.0f / 127.0f)

static inline int sine_xmod_applies(const timbre_extra_t *x, int osc1_wave, int sync_ring_mode) {
    float c1 = x->osc1_ctrl[0], c2 = x->osc1_ctrl[1];
    return osc1_wave == OSC1_WAVE_SINE && (sync_ring_mode == SYNC_RING_SYNC || sync_ring_mode == SYNC_RING_BOTH) &&
           (c1 > 0.0f || c2 > 0.0f) && c1 + c2 <= XMOD_MAX_DEPTH + 1e-6f && tinyk_tuning.xmod_semitones != 0.0f;
}

/* The cross-mod depth: Control 1 plus Control 2 x LFO1 (the timbre's patch LFO1, -1..+1) */
static inline float sine_xmod_depth(const timbre_extra_t *x, float lfo1) {
    return x->osc1_ctrl[0] + x->osc1_ctrl[1] * lfo1;
}

/* Osc 1 frequency multiplier for a cross-mod depth: an offset in semitones, linear in the depth (clamped to 4 octaves
 * down / 2 up, beyond anything measured) */
static inline float sine_xmod_ratio(float depth) {
    float st = tinyk_tuning.xmod_semitones * depth + tinyk_tuning.xmod_offset_semitones;
    return exp2f(fmaxf(-48.0f, fminf(24.0f, st)) * (1.0f / 12.0f));
}

/* White-noise generator state (xorshift32); reset by synth_init so renders are repeatable */
#define NOISE_SEED 0x1234ABCDu

/* Fast polynomial tanh approximation for saturation in feedback loops */
static inline float fast_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

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

/* ADSR calculation helpers */
static inline float time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    /* Exponential curve for decay/release time control */
    float sec = min_sec * powf(max_sec / min_sec, clamp01f(time_val_01));
    float coeff = expf(-4.60517f / (sec * fs));
    if (coeff < 0.0f) coeff = 0.0f;
    if (coeff > 0.999999f) coeff = 0.999999f;
    return coeff;
}

static inline float attack_time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    /* Analog RC exponential curve for attack time control targeting 1.35 overshoot for punch */
    float sec = min_sec * powf(max_sec / min_sec, clamp01f(time_val_01));
    float coeff = expf(-1.35f / (sec * fs));
    if (coeff < 0.0f) coeff = 0.0f;
    if (coeff > 0.999999f) coeff = 0.999999f;
    return coeff;
}

static inline void adsr_gate_on(adsr_t *env, float attack_coeff) {
    env->stage = ENV_ATTACK;
    env->target = 1.35f;
    env->rate = attack_coeff; /* Stored attack coefficient */
}

static inline void adsr_gate_off(adsr_t *env) {
    env->stage = ENV_RELEASE;
    env->target = 0.0f;
}

static inline float adsr_process(adsr_t *env, float decay_coeff, float sustain_level, float release_coeff) {
    /* Denormal flushing on entry */
    if (fabsf(env->value) < 1e-15f) {
        env->value = 0.0f;
        if (env->stage == ENV_RELEASE) {
            env->stage = ENV_IDLE;
        }
    }

    switch (env->stage) {
        case ENV_IDLE:
            env->value = 0.0f;
            break;
        case ENV_ATTACK:
            /* Analog RC curve targeting 1.35 for snappy punch and immediate bite */
            env->value = env->value * env->rate + (1.0f - env->rate) * 1.35f;
            if (env->value >= 1.0f) {
                env->value = 1.0f;
                env->stage = ENV_DECAY;
                env->target = sustain_level;
            }
            break;
        case ENV_DECAY:
            /* True analog exponential decay curve */
            env->value = env->value * decay_coeff + (1.0f - decay_coeff) * sustain_level;
            if (fabsf(env->value) < 1e-15f) env->value = 0.0f;
            if (fabsf(env->value - sustain_level) < 0.0005f) {
                env->value = sustain_level;
                env->stage = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            env->value = sustain_level;
            if (fabsf(env->value) < 1e-15f) env->value = 0.0f;
            break;
        case ENV_RELEASE:
            /* True analog exponential release curve */
            env->value = env->value * release_coeff;
            if (env->value <= 0.0005f || fabsf(env->value) < 1e-15f) {
                env->value = 0.0f;
                env->stage = ENV_IDLE;
            }
            break;
    }

    if (isnan(env->value) || isinf(env->value)) env->value = 0.0f;
    return fmaxf(0.0f, fminf(1.0f, env->value));
}

/* Safety bound on the SVF integrator states. It must stay well above their normal range: the input is
 * tanh-limited to +-1 but a TPT integrator state reaches ~2x the output, and the band-pass output peaks
 * at 1/k (up to ~16 at full resonance). The old +-2 clamp chopped normal signals near the cutoff ceiling. */
#define SVF_STATE_LIMIT 64.0f

/* Process single 2-pole TPT State Variable Filter with soft non-linear saturation
 * in the resonance feedback path and pre-filter drive stage, preserving low-end weight */
static inline float svf_process_2pole(svf_t *svf, float in, float fc, float res, filter_type_t type, float fs) {
    /* 1. NaN/Infinity sanitization on existing filter states */
    if (isnan(svf->s1) || isinf(svf->s1)) svf->s1 = 0.0f;
    if (isnan(svf->s2) || isinf(svf->s2)) svf->s2 = 0.0f;

    /* 2. Denormal flushing on existing filter states */
    if (fabsf(svf->s1) < 1e-15f) svf->s1 = 0.0f;
    if (fabsf(svf->s2) < 1e-15f) svf->s2 = 0.0f;

    /* 3. State clamping before feedback calculations */
    svf->s1 = fmaxf(-SVF_STATE_LIMIT, fminf(SVF_STATE_LIMIT, svf->s1));
    svf->s2 = fmaxf(-SVF_STATE_LIMIT, fminf(SVF_STATE_LIMIT, svf->s2));

    /* Clamp cutoff frequency to Nyquist safe range */
    if (isnan(fc) || isinf(fc)) fc = 1000.0f;
    if (fc < 20.0f) fc = 20.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f; /* the TPT SVF is stable up to Nyquist; keep tan() well conditioned */

    /* g = tan(pi * fc / fs) */
    float g = tanf((float)M_PI * fc / fs);
    /* Resonance mapping: res in [0, 1] -> damping k from k0 down to k0 * 0.04. Non-finite or out-of-range
     * values are sanitized so the feedback can never become negatively damped and blow up. */
    if (!(res >= 0.0f)) res = 0.0f;
    if (res > 1.0f) res = 1.0f;
    /* Each LPF24 stage starts Butterworth (k = sqrt 2, Q 0.707, as measured on the microKORG at res 0);
     * the 2-pole types start critically damped (k = 2). Resonance scales k down proportionally. */
    float k;
    if (type == FILTER_BP_12) {
        /* Measured on the microKORG: Q 0.33 at res 0, 1.47 at res 63; exponential so it stays stable (Q ~6.8 at 127) */
        k = tinyk_tuning.bpf_k0 * exp2f(-4.38f * res);
    } else {
        float k0 = (type == FILTER_LP_24) ? 1.4142f : 2.0f;
        k = k0 * (1.0f - 0.5f * tinyk_tuning.res_damping_range * res);
    }
    if (k < 0.05f) k = 0.05f;

    /* Sanitize and clamp input signal */
    if (isnan(in) || isinf(in)) in = 0.0f;
    in = fmaxf(-3.0f, fminf(3.0f, in));

    /* Input soft clip with resonance gain compensation (compensating passband volume drop). Distortion is
     * not here: it is in the amp section, after the filter (see the voice loop). */
    float input_gain = 1.0f + res * 0.5f;
    float v0 = fast_tanh(in * input_gain);

    /* Zero-delay-feedback (TPT) SVF high-pass node: hp = (x - (k + g) s1 - s2) / (1 + g (g + k)).
     * The (k + g) term matters: with k alone the solution was wrong by a g*s1 term that grows with the
     * cutoff, and near the 13 kHz ceiling the filter went unstable and filled the band with hiss (a 131 Hz
     * saw through LPF12 res 0.35 at 12.9 kHz had an 8 kHz centroid). The loop is linear, which is stable
     * for any k > 0; the drive saturation stays on the input above. */
    float denom = 1.0f + g * (g + k);
    if (denom < 1e-6f) denom = 1e-6f;
    float u = (v0 - (k + g) * svf->s1 - svf->s2) / denom;

    /* Integrator bandpass and lowpass state updates */
    float v1 = g * u + svf->s1;
    float next_s1 = g * u + v1;

    float v2 = g * v1 + svf->s2;
    float next_s2 = g * v1 + v2;

    /* NaN/Infinity sanitization and state clamping */
    if (isnan(next_s1) || isinf(next_s1)) next_s1 = 0.0f;
    if (isnan(next_s2) || isinf(next_s2)) next_s2 = 0.0f;
    if (fabsf(next_s1) < 1e-15f) next_s1 = 0.0f;
    if (fabsf(next_s2) < 1e-15f) next_s2 = 0.0f;

    svf->s1 = fmaxf(-SVF_STATE_LIMIT, fminf(SVF_STATE_LIMIT, next_s1));
    svf->s2 = fmaxf(-SVF_STATE_LIMIT, fminf(SVF_STATE_LIMIT, next_s2));

    float out = 0.0f;
    switch (type) {
        case FILTER_LP_12:
        case FILTER_LP_24:
            out = v2;
            break;
        case FILTER_BP_12:
            out = v1;
            break;
        case FILTER_HP_12:
            out = u;
            break;
        default:
            out = v2;
            break;
    }

    DIAG_CHECK(out);
    DIAG_CHECK(next_s1);
    DIAG_CHECK(next_s2);
    if (isnan(out) || isinf(out)) out = 0.0f;
    return fmaxf(-2.0f, fminf(2.0f, out));
}

/* Exponential cutoff map: 0..1 -> ~15 Hz .. ~19.6 kHz (5.1 octaves per half-turn) */

/* Soft-clipping saturation for the voice mix: unity gain for small signals, bounded at +/-1 */
static inline float soft_clip(float x) {
    return tanhf(x);
}

/* Build a single-cycle table for the Vox (formant) and DWGS (digital waveform) oscillators.
 * These are additive approximations, not the hardware's sampled waves. */
static void build_wavetable(float *table, int wave, float dwgs01) {
    enum { MAX_HARMONICS = 16 };
    int idx = (int)(dwgs01 * 63.0f + 0.5f);
    float amp[MAX_HARMONICS + 1];

    for (int h = 1; h <= MAX_HARMONICS; h++) {
        float fh = (float)h;
        if (wave == OSC1_WAVE_VOX) {
            /* Two formant peaks over a saw-like rolloff */
            float f1 = (fh - 3.0f) / 1.2f;
            float f2 = (fh - 9.0f) / 1.5f;
            amp[h] = (1.0f / fh) * (0.3f + 4.0f * expf(-0.5f * f1 * f1) + 2.5f * expf(-0.5f * f2 * f2));
        } else {
            /* 8 spectral-peak positions x 8 rolloff slopes; odd indices thin out even harmonics */
            float center = 1.0f + (float)(idx >> 3) * 1.6f;
            float slope = 0.4f + 0.2f * (float)(idx & 7);
            float peak = (fh - center) / 1.2f;
            amp[h] = powf(fh, -slope) * (1.0f + 4.0f * expf(-0.5f * peak * peak));
            if ((idx & 1) && (h % 2 == 0)) amp[h] *= 0.25f;
        }
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
    extra->dwgs = clamp01f(t->dwgs);
    extra->osc1_ctrl[0] = clamp01f(t->osc1_ctrl1);
    extra->osc1_ctrl[1] = clamp01f(t->osc1_ctrl2);
    extra->assign = (int)lroundf(clamp01f(t->assign) * 2.0f);
    extra->unison_cents = clamp01f(t->unison_detune) * 127.0f;
    extra->pan = (clamp01f(t->pan) - 0.5f) * 2.0f;
    extra->multi_trigger = t->trigger_multi >= 0.5f;

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

/* Load patch data into the active engine and voices; index is the bank slot it occupies */
static void load_preset_from(synth_engine_t *synth, const struct Preset *p, int index) {
    float saved_timbre_balance = (synth->params[PARAM_TIMBRE_BALANCE] > 0.0f) ? synth->params[PARAM_TIMBRE_BALANCE] : synth->timbre_balance;

    synth->current_preset = index;
    int bank_side = index / 64;
    int genre_category = (index % 64) / 8;
    int slot_in_bank = index % 8;
    int program_num = (bank_side == 0) ? (slot_in_bank + 1) : (slot_in_bank + 9);

    synth->bank_side = bank_side;
    synth->genre_category = genre_category;
    synth->program_num = program_num;

    /* Timbre 1 into the active parameter set, then the shared FX section */
    apply_timbre(synth->params, &synth->timbre_extra[0], &p->t1);
    synth->params[PARAM_CHORUS_MIX] = clamp01f(p->chorus_mix);
    synth->modfx_speed = clamp01f(p->modfx_speed);
    synth->modfx_type = (int)lroundf(clamp01f(p->modfx_type) * 2.0f);
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
    if (synth->params[PARAM_VEL_SENS] <= 0.001f) {
        synth->params[PARAM_VEL_SENS] = 0.3f;
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
    synth->bank_side = side;
    synth->genre_category = row;
    synth->program_num = side ? col + 9 : col + 1;
    load_preset(synth, side * 64 + row * 8 + col);
}

/* Switches the active bank (0 = built-in, 1..N = .syx files) and reloads the current program from it. */
static void select_bank_file(synth_engine_t *synth, int b) {
    if (b < 0) b = 0;
    if (b > g_syx_bank_count) b = g_syx_bank_count;
    synth->bank_file = b;
    load_preset(synth, synth->current_preset);
}

/* Category (matrix row) and per-category program names for the Category / Program controls: each of
 * the 8 categories holds 16 programs, A1..A8 then B1..B8 (bank sides A and B of that row). */
static const char *const CATEGORY_NAMES[8] = {
    "Trance", "Techno/House", "Electronica", "DnB/Breaks", "Hiphop/Vintage", "Retro", "SE/Hit", "Vocoder"
};
static const char *const PATCH_NAMES[16] = {
    "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8", "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8"
};

/* Program within the category: 0..15 = side * 8 + column */
static int current_patch(const synth_engine_t *synth) {
    return synth->program_num - 1;
}

/* Preset index of Program `patch` (0..15) in Category `category` (0..7) */
static int patch_preset(int category, int patch) {
    return (patch / 8) * 64 + category * 8 + patch % 8;
}

/* Index of `val` in names[], or -1 */
static int name_index(const char *val, const char *const *names, int n) {
    for (int i = 0; i < n; i++) {
        if (strcmp(val, names[i]) == 0) return i;
    }
    return -1;
}

/* Index of `val` among the current category's Program labels ("B.12 ARPEJMATR"), or -1 */
static int patch_label_index(const synth_engine_t *synth, const char *val) {
    char label[48];
    for (int i = 0; i < 16; i++) {
        format_preset_name(synth, patch_preset(synth->genre_category, i), label, sizeof label);
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

/* Everything the served labels depend on: the Program names (bank, category) and the timbre badge */
static int label_context(const synth_engine_t *synth) {
    return (synth->bank_file * 8 + synth->genre_category) * 3 + timbre_badge(synth);
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

static const selector_t SELECTORS[] = {
    { "wave1", "Wave 1", WAVE1_NAMES, WAVE1_SHORT, 7, NULL },
    { "wave2", "Wave 2", WAVE2_NAMES, WAVE2_SHORT, 3, NULL },
    { "sync_ring", "Sync / Ring", SYNC_RING_NAMES, SYNC_RING_SHORT, 4, SYNC_RING_TO_ENGINE },
    { "filter_type", "Filter Type", FILTER_NAMES, FILTER_NAMES, 4, NULL },
    { "voice_assign", "Voice", ASSIGN_NAMES, ASSIGN_SHORT, 3, NULL },
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
    int col = (synth->program_num > 8) ? synth->program_num - 9 : synth->program_num - 1;
    return (synth->genre_category + 1) * 10 + col + 1;
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
    /* Mod Wheel knob (0..127) for a Move without a wheel: it sets the same patch source (7) as CC1, so
     * whichever moved last wins and an external wheel takes over as soon as it sends */
    if (strcmp(key, "mod_wheel") == 0) {
        synth->modwheel_src = clamp01f(val / 127.0f);
        return;
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
        if (g > 7) g = 7;
        synth->genre_category = g;
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(synth, preset_index);
        return;
    }

    if (strcmp(key, "program_num") == 0) {
        int p = (val >= 1.0f && val <= 16.0f) ? (int)roundf(val) : (1 + (int)roundf(val * 15.0f));
        if (p < 1) p = 1;
        if (p > 16) p = 16;
        synth->program_num = p;
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(synth, preset_index);
        return;
    }

    if (strcmp(key, "bank_side") == 0) {
        int col = (synth->program_num > 8) ? synth->program_num - 9 : synth->program_num - 1;
        select_program(synth, (val >= 0.5f) ? 1 : 0, synth->genre_category, col);
        return;
    }

    if (strcmp(key, "bank_file") == 0) {
        select_bank_file(synth, (int)roundf(val));
        return;
    }

    if (strcmp(key, "category") == 0) {
        int patch = current_patch(synth);
        select_program(synth, patch / 8, index_from_value(val, 7), patch % 8);
        return;
    }

    if (strcmp(key, "patch") == 0) {
        int patch = index_from_value(val, 15);
        select_program(synth, patch / 8, synth->genre_category, patch % 8);
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
        load_preset(synth, index_from_value(val, 127));
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
        return (float)synth->current_preset / (float)(NUM_PRESETS - 1);
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
    if (strcmp(key, "mod_wheel") == 0) { /* follows CC1 too */
        return synth->modwheel_src * 127.0f;
    }
    if (strcmp(key, "arp_on") == 0) {
        return (float)synth->arp.set.on;
    }
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
    v->osc1_phase = voice_rand_phase(synth);
    v->osc2_phase = voice_rand_phase(synth);
    v->sub_phase = voice_rand_phase(synth);
    v->amp_env.value = 0.0f;
    v->filter_env.value = 0.0f;
    v->declick_pos = 0;
    v->vel_gain = v->velocity;
}

/* A timbre's voices: in Layer mode t and t + 2 (two per timbre), in Single mode all four */
static int timbre_voices(const synth_engine_t *synth, int t, int *idx) {
    int is_layer = (synth->params[PARAM_VOICE_MODE] > 0.5f) || (synth->voice_mode == 1);
    if (is_layer) {
        idx[0] = t;
        idx[1] = t + 2;
        return 2;
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
    float place = n > 1 ? 2.0f * (float)k / (float)(n - 1) - 1.0f : 0.0f; /* -1 .. +1 */
    v->active = true;
    v->gate = true;
    v->note = note;
    v->velocity = vel01;
    v->age = synth->voice_counter;
    v->timbre_index = t;
    v->is_timbre_2 = is_layer ? t : 0;
    v->layer_partner = -1;
    v->unison_cents = 0.5f * place * x->unison_cents * tinyk_tuning.unison_cents_scale;
    v->unison_pan = place * tinyk_tuning.unison_spread;
    v->unison_gain = 1.0f / sqrtf((float)n);
    v->kill = false;
    if (!was_active || synth->timbre_params[t][PARAM_PORTAMENTO] < 0.005f) v->current_pitch = target_pitch;
    v->target_pitch = target_pitch;
    if (!was_active) voice_fresh_state(synth, v);
    if (retrigger || !was_gated) {
        adsr_gate_on(&v->filter_env, atk1_coef);
        adsr_gate_on(&v->amp_env, atk2_coef);
    }
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
    int stack = (x->assign == ASSIGN_UNISON) ? n : 1;
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

/* Layer mode, one timbre: its voice for a note among its two (timbre t uses voices t and t + 2): the one already
 * playing the note, else an idle one, else the released one with the lower level, else the older */
static int layer_voice_for(const synth_engine_t *synth, int t, uint8_t note) {
    const voice_t *a = &synth->voices[t], *b = &synth->voices[t + 2];
    if (a->active && a->note == note) return t;
    if (b->active && b->note == note) return t + 2;
    if (!a->active || a->amp_env.stage == ENV_IDLE) return t;
    if (!b->active || b->amp_env.stage == ENV_IDLE) return t + 2;
    if (!a->gate && !b->gate) return (a->amp_env.value <= b->amp_env.value) ? t : t + 2;
    if (!a->gate) return t;
    if (!b->gate) return t + 2;
    return (a->age <= b->age) ? t : t + 2;
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
    /* A sounding voice (retrigger, steal) keeps its filter state and EG levels: the EGs restart from where they are,
     * so its sound never jumps (crushing the filter state to 5 % stepped an open filter's output) */
    if (!was_active) voice_fresh_state(synth, v);
    adsr_gate_on(&v->filter_env, atk1_coef);
    adsr_gate_on(&v->amp_env, atk2_coef);
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
        /* Layer mode: each timbre in its own two voices (t and t + 2), as its voice assign says */
        for (int t = 0; t < 2; t++) {
            if (!((mask >> t) & 1)) continue;
            float atk1 = t ? t2_atk1_coef : t1_atk1_coef, atk2 = t ? t2_atk2_coef : t1_atk2_coef;
            if (synth->timbre_extra[t].assign == ASSIGN_POLY) layer_voice_start(synth, t, note, vel01, target_pitch, atk1, atk2);
            else group_note_on(synth, t, note, vel01, target_pitch, atk1, atk2);
        }
    } else if (synth->timbre_extra[0].assign != ASSIGN_POLY) {
        /* Single mode, Mono or Unison: one voice, or all four stacked */
        group_note_on(synth, 0, note, vel01, target_pitch, t1_atk1_coef, t1_atk2_coef);
    } else {
        /* =========================================================
         * SINGLE MODE (4-Voice Polyphonic Engine)
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

        /* as in layer_voice_start: a sounding voice keeps its filter state and EG levels */
        if (!was_active) voice_fresh_state(synth, v);

        adsr_gate_on(&v->filter_env, t1_atk1_coef);
        adsr_gate_on(&v->amp_env, t1_atk2_coef);

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
 *   Ensemble: three sine-modulated taps 120 degrees apart, the middle one shared, ENS_* (chosen, not measured).
 *   Phaser: six first-order all-pass stages per channel swept exponentially by a triangle LFO (right channel a
 *     quarter cycle on), with feedback, summed with the dry signal at 0.707 each, PHASER_* (chosen, not measured). */
#define ENS_CENTER_MS   7.0f
#define ENS_DEPTH_MS    1.6f
#define PHASER_LO_HZ    250.0f
#define PHASER_OCTAVES  4.0f
#define PHASER_FEEDBACK 0.55f

static inline float modfx_tri(float ph) { /* phase 0..1 -> triangle -1 (0) .. +1 (0.5) */
    ph -= floorf(ph);
    return 1.0f - 4.0f * fabsf(ph - 0.5f);
}

static inline float chorus_tap(const float *buf, uint32_t wpos, float delay) {
    float rpos = (float)wpos - delay;
    while (rpos < 0.0f) rpos += (float)CHORUS_BUFFER_SIZE;
    int i0 = (int)rpos % CHORUS_BUFFER_SIZE, i1 = (i0 + 1) % CHORUS_BUFFER_SIZE;
    float fr = rpos - floorf(rpos);
    return buf[i0] * (1.0f - fr) + buf[i1] * fr;
}

/* One sample of the program's Mod FX on (l, r); wpos is where this sample was written into the chorus line */
static inline void modfx_process(synth_engine_t *s, float *l, float *r, uint32_t wpos, float depth, float fs) {
    float hz = tinyk_tuning.modfx_rate_lo_hz * powf(tinyk_tuning.modfx_rate_span, s->modfx_speed);
    s->chorus_lfo_phase += hz / fs;
    if (s->chorus_lfo_phase >= 1.0f) s->chorus_lfo_phase -= 1.0f;
    float ph = s->chorus_lfo_phase, ms = fs * 0.001f, maxd = (float)(CHORUS_BUFFER_SIZE - 4);
    if (s->modfx_type == 2) { /* Phaser */
        float in[2] = { *l, *r };
        for (int c = 0; c < 2; c++) {
            float sweep = 0.5f + 0.5f * modfx_tri(ph + 0.25f * (float)c);
            float fc = fminf(PHASER_LO_HZ * exp2f(PHASER_OCTAVES * depth * sweep), 0.45f * fs);
            float t = tanf((float)M_PI * fc / fs), a = (t - 1.0f) / (t + 1.0f);
            float x = in[c] + PHASER_FEEDBACK * depth * s->phaser_fb[c];
            for (int k = 0; k < 6; k++) { /* y = a x + x[n-1] - a y[n-1], as one state */
                float y = a * x + s->phaser_ap[c][k];
                s->phaser_ap[c][k] = x - a * y;
                x = y;
            }
            s->phaser_fb[c] = fast_tanh(x);
            in[c] = 0.7071f * (in[c] + x); /* full-depth notches, about the dry level on average */
        }
        *l = in[0];
        *r = in[1];
        return;
    }
    if (s->modfx_type == 1) { /* Ensemble */
        float t0 = chorus_tap(s->chorus_buf_l, wpos, fminf(maxd, (ENS_CENTER_MS + depth * ENS_DEPTH_MS * sinf(2.0f * (float)M_PI * ph)) * ms));
        float t1 = 0.5f * (chorus_tap(s->chorus_buf_l, wpos, fminf(maxd, (ENS_CENTER_MS + depth * ENS_DEPTH_MS * sinf(2.0f * (float)M_PI * (ph + 1.0f / 3.0f))) * ms))
                         + chorus_tap(s->chorus_buf_r, wpos, fminf(maxd, (ENS_CENTER_MS + depth * ENS_DEPTH_MS * sinf(2.0f * (float)M_PI * (ph + 1.0f / 3.0f))) * ms)));
        float t2 = chorus_tap(s->chorus_buf_r, wpos, fminf(maxd, (ENS_CENTER_MS + depth * ENS_DEPTH_MS * sinf(2.0f * (float)M_PI * (ph + 2.0f / 3.0f))) * ms));
        *l += tinyk_tuning.chorus_wet * 0.8f * (t0 + 0.5f * t1);
        *r += tinyk_tuning.chorus_wet * 0.8f * (t2 + 0.5f * t1);
        return;
    }
    /* Chorus/Flanger */
    float dl = (tinyk_tuning.chorus_center_ms + depth * tinyk_tuning.chorus_depth_ms * modfx_tri(ph)) * ms;
    float dr = (tinyk_tuning.chorus_center_ms + depth * tinyk_tuning.chorus_depth_ms * modfx_tri(ph + 0.5f)) * ms;
    *l += tinyk_tuning.chorus_wet * chorus_tap(s->chorus_buf_l, wpos, fmaxf(1.0f, fminf(maxd, dl)));
    *r += tinyk_tuning.chorus_wet * chorus_tap(s->chorus_buf_r, wpos, fmaxf(1.0f, fminf(maxd, dr)));
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
    const float kill_coef = expf(-4.60517f / (KILL_RELEASE_S * fs));

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
        int xmod_on;            /* Sine cross-mod pitch envelope (sine_xmod_applies) */
        float osc_mix;
        float sub_level;
        float glide_coeff;
        float transpose_semi;
        float noise_level;
        float level;
        float pan;              /* timbre pan, -1..+1 */
        const float *wavetable;

        float cutoff_pitch;   /* knob position in octaves above cutoff_base_hz */
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
        t_cfg[t].pw = 0.5f + 0.45f * pw_p; /* 0..1 -> 50%..95% duty */
        t_cfg[t].osc2_wave = (int)(wave2_p * (float)(OSC2_WAVE_COUNT - 1) + 0.5f);
        t_cfg[t].detune_semi = (detune_p - 0.5f) * 48.0f;
        t_cfg[t].sync_ring_mode = (int)(sync_ring_p * (float)(SYNC_RING_COUNT - 1) + 0.5f);
        t_cfg[t].xmod_on = sine_xmod_applies(extra, t_cfg[t].osc1_wave, t_cfg[t].sync_ring_mode);
        t_cfg[t].transpose_semi = extra->transpose_semi;
        t_cfg[t].noise_level = extra->noise_level;
        t_cfg[t].level = extra->level;
        t_cfg[t].pan = extra->pan;

        /* Vox / DWGS wavetable: rebuild only when the selection changes */
        t_cfg[t].wavetable = synth->wavetable[t];
        if (t_cfg[t].osc1_wave == OSC1_WAVE_VOX || t_cfg[t].osc1_wave == OSC1_WAVE_DWGS) {
            int key = t_cfg[t].osc1_wave * 64 + (int)(extra->dwgs * 63.0f + 0.5f);
            if (synth->wavetable_key[t] != key) {
                build_wavetable(synth->wavetable[t], t_cfg[t].osc1_wave, extra->dwgs);
                synth->wavetable_key[t] = key;
            }
        }
        t_cfg[t].osc_mix = osc_mix_p;
        t_cfg[t].sub_level = sub_level_p;

        t_cfg[t].glide_coeff = 1.0f;
        if (portamento_p > 0.005f) {
            float glide_time = 0.005f * powf(400.0f, portamento_p);
            t_cfg[t].glide_coeff = 1.0f - expf(-1.0f / (glide_time * fs));
        }

        t_cfg[t].resonance = resonance_p;
        t_cfg[t].filter_type = (filter_type_t)(int)(filter_type_p * (float)(FILTER_TYPE_COUNT - 1) + 0.5f);
        /* BPF12's centre follows the knob on its own curve (fitted on VST takes at cutoff 32/64/96, res 63) */
        if (t_cfg[t].filter_type == FILTER_BP_12)
            t_cfg[t].cutoff_pitch = clamp01f(cutoff_p) * tinyk_tuning.bpf_cutoff_octaves + tinyk_tuning.bpf_cutoff_offset;
        else
            t_cfg[t].cutoff_pitch = clamp01f(cutoff_p) * tinyk_tuning.cutoff_octaves;
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
            /* Pan has its own depth curve: the measured 2.4 (pitch / cutoff) left +20 at a +-0.7 dB swing */
            float a = extra->patch_int[p];
            float curve = (extra->patch_dst[p] == PATCH_DST_PAN) ? tinyk_tuning.patch_pan_curve : tinyk_tuning.patch_int_curve;
            t_cfg[t].patch_amt[p] = copysignf(powf(fabsf(a), curve), a);
        }
        int lfo2_rate_modulated = 0;
        for (int p = 0; p < 4; p++) {
            if (extra->patch_dst[p] == PATCH_DST_LFO2_FREQ && extra->patch_int[p] != 0.0f) lfo2_rate_modulated = 1;
        }
        for (int l = 0; l < 2; l++) {
            float hz;
            if (extra->lfo_sync_note[l] >= 0) {
                /* tempo sync: period = note length (fraction of a whole note) at the session tempo */
                float note = LFO_SYNC_NOTES[extra->lfo_sync_note[l]];
                hz = synth->tempo_bpm / (240.0f * note);
                /* Free-running (no key sync) synced LFOs also lock their phase to the transport while it runs,
                 * cycle start on beat 0; key-synced ones restart on each note, as on the hardware. An LFO2 whose
                 * rate a patch modulates is left to run. */
                if (beat >= 0.0 && extra->lfo_keysync[l] == 0 && !(l == 1 && lfo2_rate_modulated)) {
                    double cycles = beat / (4.0 * (double)note);
                    synth->patch_lfo[t][l].phase = (float)(cycles - floor(cycles));
                }
            } else {
                hz = 0.05f * powf(600.0f, extra->lfo_rate[l]); /* same 0.05..30 Hz curve as the UI LFOs */
            }
            t_cfg[t].lfo_dt[l] = hz / fs;
        }
    }

    /* LFO Frequencies: 0.05 Hz to 30 Hz */
    float lfo1_freq = 0.05f * powf(600.0f, lfo1_rate_p);
    float lfo2_freq = 0.05f * powf(600.0f, lfo2_rate_p);
    float lfo1_dt = lfo1_freq / fs;
    float lfo2_dt = lfo2_freq / fs;

    /* Delay config. The hardware has one delay depth that sets both the repeats and the level;
     * depth 0 means the delay is off. Feedback is capped at 0.6 so repeats die away instead of
     * building into a pseudo-reverb wash. */
    float target_delay_samples;
    if (synth->delay_sync_note >= 0) {
        /* tempo-synced: the Delay Time knob steps the time base (1/32 .. 1/1) at the session tempo */
        int base = (int)lroundf(clamp01f(delay_time_p) * 14.0f);
        target_delay_samples = 240.0f / synth->tempo_bpm * DELAY_SYNC_NOTES[base] * fs;
        target_delay_samples = fmaxf(100.0f, fminf((float)(DELAY_BUFFER_SIZE - 2), target_delay_samples));
    } else {
        target_delay_samples = 100.0f + delay_time_p * (float)(DELAY_FREE_MAX_SAMPLES - 200);
    }
    float delay_feedback = fminf(delay_fdbk_p * 0.75f, 0.6f);
    float delay_send = delay_mix_p * tinyk_tuning.delay_send_scale;
    int delay_on = (delay_mix_p > 0.005f);

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
    /* The audible noise's pre-shaping: the inverse of the same shelf at noise_tilt_db (see heard_noise below) */
    float noise_b0, noise_b1, noise_a1;
    {
        float G = powf(10.0f, tinyk_tuning.noise_tilt_db / 20.0f);
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
        /* The noise you hear keeps part of the brightness tilt: the tilt models the VST oscillators' extra top end
         * and on raw noise added ~18 dB around 17 kHz (A.21's off-beat hat came out level with the kick, thin
         * hiss), while fully whitened noise left the hat 21 dB under the kick, against 8.7 dB on the VST
         * (ref_a21_timbre2_drum_c3). So the audible noise is pre-shaped by the inverse of a lower shelf,
         * noise_tilt_db (1 + a1 z^-1) / (b0 + b1 z^-1), stable since the shelf's zero is inside the unit circle.
         * S&H keeps the raw source. */
        float heard_noise = (white_noise + noise_a1 * synth->noise_x1) / noise_b0 - (noise_b1 / noise_b0) * synth->noise_y1;
        synth->noise_x1 = white_noise;
        synth->noise_y1 = heard_noise;

        /* Patch-matrix LFOs, per timbre. LFO2's rate can itself be a patch destination; only the
         * timbre-wide sources (LFO1, pitch bend, mod wheel) can drive it, since it is shared by the timbre's voices. */
        float plfo[2][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        for (int t = 0; t < (is_layer_mode ? 2 : 1); t++) {
            const timbre_extra_t *ex = t_cfg[t].extra;
            float lfo2_fmod = 0.0f;
            for (int p = 0; p < 4; p++) {
                if (ex->patch_dst[p] != PATCH_DST_LFO2_FREQ || ex->patch_int[p] == 0.0f) continue;
                float sv = 0.0f;
                if (ex->patch_src[p] == PATCH_SRC_LFO1) sv = synth->patch_lfo[t][0].out;
                else if (ex->patch_src[p] == PATCH_SRC_PITCH_BEND) sv = synth->bend_src;
                else if (ex->patch_src[p] == PATCH_SRC_MOD_WHEEL) sv = synth->modwheel_src;
                lfo2_fmod += t_cfg[t].patch_amt[p] * sv;
            }
            for (int l = 0; l < 2; l++) {
                lfo_t *lf = &synth->patch_lfo[t][l];
                lf->phase += t_cfg[t].lfo_dt[l] * (l == 1 ? exp2f(4.0f * lfo2_fmod) : 1.0f);
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
            for (int p = 0; p < 4; p++) {
                float amt = cfg->patch_amt[p];
                if (amt == 0.0f) continue;
                float sv;
                switch (cfg->extra->patch_src[p]) {
                    case PATCH_SRC_EG1:      sv = v->filter_env.value; break;
                    case PATCH_SRC_EG2:      sv = v->amp_env.value; break;
                    case PATCH_SRC_LFO1:     sv = plfo[t_idx][0]; break;
                    case PATCH_SRC_LFO2:     sv = plfo[t_idx][1]; break;
                    case PATCH_SRC_VELOCITY: sv = v->velocity; break;
                    case PATCH_SRC_KBD:      sv = ((float)v->note - 60.0f) / 64.0f; break;
                    case PATCH_SRC_PITCH_BEND: sv = synth->bend_src; break;
                    case PATCH_SRC_MOD_WHEEL:  sv = synth->modwheel_src; break;
                    default:                 sv = 0.0f; break;
                }
                if (cfg->extra->patch_dst[p] == PATCH_DST_AMP) amp_gain *= fmaxf(0.0f, 1.0f + amt * sv);
                else pmod[cfg->extra->patch_dst[p]] += amt * sv;
            }
            amp_gain = fminf(amp_gain * amp_gain, 1.0f / fmaxf(cfg->level, 0.05f));

            /* Portamento Pitch Glide */
            v->current_pitch += (v->target_pitch - v->current_pitch) * cfg->glide_coeff;

            /* LFO1 pitch mod (vibrato) + Pitch Bend + patch -> pitch */
            float pitch_mod = lfo1_val * (cfg->mod_int * 0.5f) + synth->pitch_bend_semi
                            + pmod[PATCH_DST_PITCH] * tinyk_tuning.patch_pitch_scale + v->unison_cents * 0.01f;

            float final_note1 = v->current_pitch + cfg->transpose_semi + pitch_mod;
            float freq1 = note_to_freq(final_note1);
            /* Sine cross-mod as a pitch envelope (synced, within the measured depths: sine_xmod_applies) */
            if (cfg->xmod_on) freq1 *= sine_xmod_ratio(sine_xmod_depth(cfg->extra, plfo[t_idx][0]));
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
            v->osc1_phase += dt1;
            if (v->osc1_phase >= 1.0f) {
                v->osc1_phase -= 1.0f;
                sync_triggered = true;
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
            switch (cfg->osc1_wave) {
                case OSC1_WAVE_SAW:
                    osc1_out = (2.0f * v->osc1_phase - 1.0f) - poly_blep(v->osc1_phase, dt1);
                    break;
                case OSC1_WAVE_SQUARE: {
                    float pw = cfg->pw + 0.45f * pmod[PATCH_DST_OSC1_CTRL1];
                    if (pw < 0.05f) pw = 0.05f;
                    if (pw > 0.95f) pw = 0.95f;
                    float raw = (v->osc1_phase < pw) ? 1.0f : -1.0f;
                    osc1_out = raw + poly_blep(v->osc1_phase, dt1) - poly_blep(fmod_pos(v->osc1_phase - pw), dt1);
                    osc1_out -= 2.0f * pw - 1.0f; /* remove the duty-cycle DC offset */
                    break;
                }
                case OSC1_WAVE_TRIANGLE:
                    osc1_out = 2.0f * fabsf(2.0f * v->osc1_phase - 1.0f) - 1.0f;
                    break;
                case OSC1_WAVE_SINE:
                    osc1_out = sinf(2.0f * (float)M_PI * v->osc1_phase);
                    break;
                case OSC1_WAVE_VOX:
                case OSC1_WAVE_DWGS: {
                    float pos = v->osc1_phase * (float)WAVETABLE_SIZE;
                    int i0 = (int)pos;
                    float frac = pos - (float)i0;
                    i0 &= (WAVETABLE_SIZE - 1);
                    int i1 = (i0 + 1) & (WAVETABLE_SIZE - 1);
                    osc1_out = cfg->wavetable[i0] * (1.0f - frac) + cfg->wavetable[i1] * frac;
                    break;
                }
                case OSC1_WAVE_NOISE:
                    osc1_out = heard_noise;
                    break;
            }

            /* --- Oscillator 2 Signal Generation --- */
            float osc2_out = 0.0f;
            switch (cfg->osc2_wave) {
                case OSC2_WAVE_SAW:
                    osc2_out = (2.0f * v->osc2_phase - 1.0f) - poly_blep(v->osc2_phase, dt2);
                    break;
                case OSC2_WAVE_SQUARE: {
                    float raw = (v->osc2_phase < 0.5f) ? 1.0f : -1.0f;
                    osc2_out = raw + poly_blep(v->osc2_phase, dt2) - poly_blep(fmod_pos(v->osc2_phase - 0.5f), dt2);
                    break;
                }
                case OSC2_WAVE_TRIANGLE:
                    osc2_out = 2.0f * fabsf(2.0f * v->osc2_phase - 1.0f) - 1.0f;
                    break;
            }

            /* Ring Modulation */
            float osc2_final = osc2_out;
            if (cfg->sync_ring_mode == SYNC_RING_RING || cfg->sync_ring_mode == SYNC_RING_BOTH) {
                osc2_final = osc1_out * osc2_out * 1.5f;
            }

            /* Sub-oscillator: square wave 1 octave down */
            float sub_out = (v->sub_phase < 0.5f) ? 1.0f : -1.0f;

            /* Mixer: crossfade Osc1/Osc2 (peak <= 1.0), plus sub and noise; the voice-mix soft clip handles overs */
            /* osc_mix: 0 = osc1 only, 0.5 = both at full level, 1 = osc2 only (like the hardware
             * mixer, the two oscillators add). cfg->level is the patch's overall level. */
            float g1 = fminf(1.0f, 2.0f * (1.0f - cfg->osc_mix));
            float g2 = fminf(1.0f, 2.0f * cfg->osc_mix);
            float osc_sum = (g1 * osc1_out + g2 * osc2_final
                          + cfg->sub_level * sub_out
                          + fmaxf(0.0f, fminf(1.0f, cfg->noise_level + pmod[PATCH_DST_NOISE])) * heard_noise)
                          * cfg->level * tinyk_tuning.mixer_trim;

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
            float keytrack_mod = (v->note - KEYTRACK_PIVOT) * ((cfg->keytrack * 2.0f - 1.0f) * (KEYTRACK_SLOPE / 12.0f));
            float lfo2_mod = cfg->mod_int * lfo2_val * 2.0f;

            /* Bipolar filter envelope: env_int 0.5 = off, > 0.5 opens, < 0.5 closes on note strike */
            float bipolar_env = (cfg->env_int - 0.5f) * 2.0f;
            bipolar_env *= (1.0f - cfg->vel_sens + cfg->vel_sens * v->velocity);
            float eg_mod = bipolar_env * f_env * tinyk_tuning.env_octaves;

            float cutoff_pitch = cfg->cutoff_pitch + keytrack_mod + lfo2_mod + eg_mod
                               + pmod[PATCH_DST_CUTOFF] * tinyk_tuning.patch_cutoff_octaves;
            float fc = tinyk_tuning.cutoff_base_hz * exp2f(fmaxf(-4.0f, fminf(16.0f, cutoff_pitch)));
            fc = fmaxf(tinyk_tuning.cutoff_floor_hz,
                       fminf(cfg->filter_type == FILTER_HP_12 ? tinyk_tuning.hpf_ceil_hz : tinyk_tuning.cutoff_ceil_hz, fc));

            /* SVF Multimode Filter with feedback tanh saturation & bass preservation */
            float filtered = 0.0f;
            if (cfg->filter_type == FILTER_LP_24) {
                float stage1 = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance * tinyk_tuning.lp24_res_scale, FILTER_LP_24, fs);
                filtered = svf_process_2pole(&v->filter_svf[1], stage1, fc, cfg->resonance * tinyk_tuning.lp24_res_scale, FILTER_LP_24, fs);
            } else {
                filtered = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance, cfg->filter_type, fs);
            }

            /* Distortion: the microKORG's is an amp-section switch (timbre byte 27, beside amp level and pan),
             * so it shapes the filter output, not its input. Gain 1 + drive * drive_gain (drive 0.5 = on) into a
             * soft clip at +-dist_ceiling: on the VST it is mostly level (+19 dB on B.11, whose spectrum it
             * leaves almost unchanged), saturating only loud signals. */
            if (cfg->drive > 0.0f) {
                float ceil_ = tinyk_tuning.dist_ceiling;
                filtered = ceil_ * fast_tanh(filtered * (1.0f + cfg->drive * tinyk_tuning.drive_gain) / ceil_);
            }

            if (isnan(filtered) || isinf(filtered)) filtered = 0.0f;

            float voice_vel = v->velocity;
            if (voice_vel <= 0.01f) voice_vel = 0.8f;
            v->vel_gain += (voice_vel - v->vel_gain) * vel_glide_k;

            /* De-click: a fresh voice fades in (raised cosine, zero slope at the start) */
            float declick = 1.0f;
            if (v->declick_pos < DECLICK_SAMPLES) {
                declick = 0.5f - 0.5f * cosf((float)M_PI * (float)v->declick_pos / (float)DECLICK_SAMPLES);
                v->declick_pos++;
            }

            /* Amp Envelope & Velocity Scaling */
            float voice_audio = filtered * a_env * v->vel_gain * declick * amp_gain * v->unison_gain;
            DIAG_CHECK(voice_audio);
            if (isnan(voice_audio) || isinf(voice_audio)) voice_audio = 0.0f;

            /* Stereo pan spread & gain scaling. Layer mode: Voice A (Timbre 1) panned left and
             * scaled by its balance share, Voice B (Timbre 2) panned right and scaled likewise. */
            float v_gain_l = 1.0f;
            float v_gain_r = 1.0f;

            /* Level and place: the timbre balance (Layer mode), then the timbre's pan (byte +26) plus the voice's
             * place in a unison stack, constant power with unity at the centre (the soft clip below handles overs) */
            {
                float vol = is_layer_mode ? (v->is_timbre_2 ? tB_vol : tA_vol) : 1.0f;
                float place = fmaxf(-1.0f, fminf(1.0f, cfg->pan + v->unison_pan));
                v_gain_l = vol * cosf((1.0f + place) * 0.25f * (float)M_PI) * 1.4142f;
                v_gain_r = vol * sinf((1.0f + place) * 0.25f * (float)M_PI) * 1.4142f;
            }

            if (pmod[PATCH_DST_PAN] != 0.0f) { /* patch -> pan: constant-power, -1 = left, +1 = right */
                float pp = fmaxf(-1.0f, fminf(1.0f, pmod[PATCH_DST_PAN]));
                v_gain_l *= cosf((1.0f + pp) * 0.25f * (float)M_PI) * 1.4142f;
                v_gain_r *= sinf((1.0f + pp) * 0.25f * (float)M_PI) * 1.4142f;
            }

            voice_sum_l += voice_audio * v_gain_l;
            voice_sum_r += voice_audio * v_gain_r;
        }

        /* Brightness tilt: the VST's oscillators are brighter than an ideal 1/n saw (measured on co_127, filter
         * open: +2 dB at 3 kHz rising to +18 dB at 17 kHz). A first-order high shelf matches that within
         * 1.3 dB rms. It sits after the filters, where it is linear and commutes with them, so it leaves the
         * pre-filter drive stage (and its measured behaviour) untouched. Measured alternative: per voice before
         * the filter scored worse (total 106.8 vs 104.1; the boosted edges clip in the drive stage). */
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
        voice_sum_l = soft_clip(voice_sum_l);
        voice_sum_r = soft_clip(voice_sum_r);

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
                synth->delay_filter_l = 0.0f;
                synth->delay_filter_r = 0.0f;
                synth->delay_active = 0;
            }
        } else {
            synth->delay_active = 1;
            uint32_t dwpos = synth->delay_write_pos;

            float drpos_l = (float)dwpos - target_delay_samples;
            while (drpos_l < 0.0f) drpos_l += (float)DELAY_BUFFER_SIZE;
            int didx_l0 = (int)drpos_l % DELAY_BUFFER_SIZE;
            int didx_l1 = (didx_l0 + 1) % DELAY_BUFFER_SIZE;
            float dfrac_l = drpos_l - (int)drpos_l;
            float dtap_l = synth->delay_buf_l[didx_l0] * (1.0f - dfrac_l) + synth->delay_buf_l[didx_l1] * dfrac_l;

            float drpos_r = (float)dwpos - (target_delay_samples * 0.75f);
            while (drpos_r < 0.0f) drpos_r += (float)DELAY_BUFFER_SIZE;
            int didx_r0 = (int)drpos_r % DELAY_BUFFER_SIZE;
            int didx_r1 = (didx_r0 + 1) % DELAY_BUFFER_SIZE;
            float dfrac_r = drpos_r - (int)drpos_r;
            float dtap_r = synth->delay_buf_r[didx_r0] * (1.0f - dfrac_r) + synth->delay_buf_r[didx_r1] * dfrac_r;

            /* 6dB/oct high-damp filter (~2.5 kHz) to roll off highs on each repeat and avoid muddy reverberant wash */
            synth->delay_filter_l += 0.28f * (dtap_l - synth->delay_filter_l);
            synth->delay_filter_r += 0.28f * (dtap_r - synth->delay_filter_r);

            if (isnan(synth->delay_filter_l) || isinf(synth->delay_filter_l)) synth->delay_filter_l = 0.0f;
            if (isnan(synth->delay_filter_r) || isinf(synth->delay_filter_r)) synth->delay_filter_r = 0.0f;
            if (fabsf(synth->delay_filter_l) < 1e-15f) synth->delay_filter_l = 0.0f;
            if (fabsf(synth->delay_filter_r) < 1e-15f) synth->delay_filter_r = 0.0f;

            /* Soft-clipped feedback path clamped strictly < 0.92f */
            synth->delay_buf_l[dwpos] = chorus_l + fast_tanh(synth->delay_filter_l * delay_feedback);
            synth->delay_buf_r[dwpos] = chorus_r + fast_tanh(synth->delay_filter_r * delay_feedback);
            synth->delay_write_pos = (dwpos + 1) % DELAY_BUFFER_SIZE;

            /* Dry stays at unity; the repeats are added as an aux send */
            out_l = chorus_l + dtap_l * delay_send;
            out_r = chorus_r + dtap_r * delay_send;
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

        DIAG_CHECK(out_l);
        DIAG_CHECK(out_r);
        float pre_lim_l = out_l * master_vol_p * pan_l;
        float pre_lim_r = out_r * master_vol_p * pan_r;

        float lim_l = soft_knee_limiter(pre_lim_l);
        float lim_r = soft_knee_limiter(pre_lim_r);

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
                    synth->pitch_bend_semi = synth->bend_src * 2.0f;
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
#define STATE_EXTRA_COUNT 30     /* older states have the first 24 (before Osc 1 Ctrl 1 / 2) or 26 (before assign / pan) */
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
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
        if (genre > 7) genre = 7;
        synth->genre_category = genre;
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(synth, preset_index);
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
        synth->program_num = p;
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(synth, preset_index);
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
        int col = (synth->program_num > 8) ? synth->program_num - 9 : synth->program_num - 1;
        select_program(synth, side, synth->genre_category, col);
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
        int byname = (strcmp(key, "category") == 0) ? name_index(val, CATEGORY_NAMES, 8) : name_index(val, PATCH_NAMES, 16);
        if (byname < 0 && strcmp(key, "patch") == 0) byname = patch_label_index(synth, val);
        synth_set_param(synth, key, byname >= 0 ? (float)byname : (float)atof(val));
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
    { "drive", "L2.DRV", "L2 Drive" }, { "level", "L2.LVL", "L2 Level" },
    { "wave1", "L2.WV1", "L2 Wave 1" }, { "pulse_width", "L2.PW", "L2 Pulse Width" },
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
static void json_put_patch_enum(json_out_t *o, const synth_engine_t *synth) {
    char label[48];
    json_put(o, "{\"key\":\"patch\",\"name\":\"Program\",\"type\":\"enum\",\"options\":[");
    for (int i = 0; i < 16; i++) {
        if (i) json_put(o, ",");
        format_preset_name(synth, patch_preset(synth->genre_category, i), label, sizeof label);
        json_put_string(o, label);
    }
    json_put(o, "],\"short_options\":[");
    for (int i = 0; i < 16; i++) {
        if (i) json_put(o, ",");
        json_put_string(o, PATCH_NAMES[i]);
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
    static const char *const TIMBRES_SINGLE_SHORT[2] = { "N/A", "N/A" };
    char num[160];
    int badge = timbre_badge(synth);
    buf[0] = '\0';
    bank_names[0] = "Built-in";
    for (int i = 0; i < g_syx_bank_count; i++) bank_names[i + 1] = g_syx_banks[i].name;

    json_put(&o, "[");
    json_put_enum(&o, "category", "Category", CATEGORY_NAMES, NULL, 8, 0);
    json_put_patch_enum(&o, synth);
    json_put_enum(&o, "bank_file", "Bank", bank_names, NULL, g_syx_bank_count + 1, 0);
    json_put_enum(&o, "voice_mode", "Mode", VOICE_MODES, VOICE_MODES_SHORT, 2, 0);
    json_put_enum(&o, "timbre_edit", "Layer", badge ? TIMBRES : TIMBRES_SINGLE,
                  badge ? TIMBRES_SHORT : TIMBRES_SINGLE_SHORT, 2, 0);
    for (size_t i = 0; i < sizeof SELECTORS / sizeof SELECTORS[0]; i++) {
        const selector_t *s = &SELECTORS[i];
        json_put_enum(&o, s->key, s->name, s->options, s->shorts, s->count, badge);
    }
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
    "ram\":\"preset_name\",\"params\":[{\"level\":\"perf\",\"label\":\"Perf\"},{\"level\":\"osc\",\"label\":\"Osc\"},{\"level\":\"env\""
    ",\"label\":\"Envelopes\"},{\"level\":\"mix\",\"label\":\"Mix/Filter\"},{\"level\":\"fx\",\"label\":\"Effects\"},{\"level\":\"arpse"
    "t\",\"label\":\"Arp Settings\"},{\"level\":\"steps\",\"label\":\"Arp Steps\"},{\"level\":\"bank\",\"label\":\"Bank\"}],\"knobs\":["
    "]},\"perf\":{\"name\":\"Perf\",\"label\":\"Perf\",\"params\":[{\"key\":\"category\",\"label\":\"Category\",\"type\":\"enum\",\"opt"
    "ions\":[\"Trance\",\"Techno/House\",\"Electronica\",\"DnB/Breaks\",\"Hiphop/Vintage\",\"Retro\",\"SE/Hit\",\"Vocoder\"],\"default"
    "\":0,\"short_name\":\"CAT\"},{\"key\":\"patch\",\"label\":\"Program\",\"short_name\":\"PROG\",\"type\":\"enum\",\"options\":[\"A1"
    "\",\"A2\",\"A3\",\"A4\",\"A5\",\"A6\",\"A7\",\"A8\",\"B1\",\"B2\",\"B3\",\"B4\",\"B5\",\"B6\",\"B7\",\"B8\"],\"default\":0},{\"key"
    "\":\"arp_on\",\"label\":\"Arp\",\"short_name\":\"ARP\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"short_options\":[\"OFF\","
    "\"ON\"],\"default\":0},{\"key\":\"voice_mode\",\"label\":\"Mode\",\"type\":\"enum\",\"options\":[\"Single\",\"Layer\"],\"short_opt"
    "ions\":[\"SNGL\",\"LAYR\"],\"default\":0,\"short_name\":\"MODE\"},{\"key\":\"cutoff\",\"label\":\"Cutoff\",\"type\":\"float\",\"mi"
    "n\":0.0,\"max\":1.0,\"default\":0.7,\"step\":0.01,\"short_name\":\"CUT\"},{\"key\":\"resonance\",\"label\":\"Resonance\",\"type\":"
    "\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01,\"short_name\":\"RES\"},{\"key\":\"release2\",\"label\":\"Amp Rel"
    "\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01,\"short_name\":\"REL\"},{\"key\":\"timbre_edit\",\"lab"
    "el\":\"Layer\",\"type\":\"enum\",\"options\":[\"Layer 1\",\"Layer 2\"],\"short_options\":[\"L1\",\"L2\"],\"default\":0,\"short_nam"
    "e\":\"LAYER\"}],\"knobs\":[\"category\",\"patch\",\"arp_on\",\"voice_mode\",\"cutoff\",\"resonance\",\"release2\",\"timbre_edit\"]"
    "},\"osc\":{\"name\":\"Osc [L1]\",\"label\":\"Osc\",\"params\":[{\"key\":\"wave1\",\"label\":\"Wave 1\",\"type\":\"enum\",\"options"
    "\":[\"Saw\",\"Square\",\"Triangle\",\"Sine\",\"Vox\",\"DWGS\",\"Noise\"],\"short_options\":[\"SAW\",\"SQR\",\"TRI\",\"SIN\",\"VOX"
    "\",\"DWG\",\"NZ\"],\"default\":0},{\"key\":\"wave2\",\"label\":\"Wave 2\",\"type\":\"enum\",\"options\":[\"Saw\",\"Square\",\"Tria"
    "ngle\"],\"short_options\":[\"SAW\",\"SQR\",\"TRI\"],\"default\":0},{\"key\":\"pulse_width\",\"label\":\"Pulse Width\",\"type\":\"f"
    "loat\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"osc2_semi\",\"label\":\"Semi\",\"type\":\"int\",\"min\":-"
    "24,\"max\":24,\"default\":0,\"unit\":\"st\"},{\"key\":\"osc2_tune\",\"label\":\"Tune\",\"type\":\"int\",\"min\":-50,\"max\":50,\"d"
    "efault\":0,\"unit\":\"ct\"},{\"key\":\"voice_assign\",\"label\":\"Voice\",\"short_name\":\"VOICE\",\"type\":\"enum\",\"options\":["
    "\"Mono\",\"Poly\",\"Unison\"],\"short_options\":[\"MONO\",\"POLY\",\"UNIS\"],\"default\":1},{\"key\":\"timbre_balance\",\"label\":"
    "\"Layer Bal\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01,\"short_name\":\"BAL\"},{\"key\":\"mod_whee"
    "l\",\"label\":\"Mod Wheel\",\"short_name\":\"MOD\",\"type\":\"int\",\"min\":0,\"max\":127,\"default\":0}],\"knobs\":[\"wave1\",\"w"
    "ave2\",\"pulse_width\",\"osc2_semi\",\"osc2_tune\",\"voice_assign\",\"timbre_balance\",\"mod_wheel\"]},\"env\":{\"name\":\"Envelop"
    "es [L1]\",\"label\":\"Envelopes\",\"params\":[{\"key\":\"attack1\",\"label\":\"Filter Atk\",\"type\":\"float\",\"min\":0.0,\"max\""
    ":1.0,\"default\":0.01,\"step\":0.01},{\"key\":\"decay1\",\"label\":\"Filter Dcy\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"def"
    "ault\":0.4,\"step\":0.01},{\"key\":\"sustain1\",\"label\":\"Filter Sus\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0."
    "5,\"step\":0.01},{\"key\":\"release1\",\"label\":\"Filter Rel\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step"
    "\":0.01},{\"key\":\"decay2\",\"label\":\"Amp Dcy\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},{\"ke"
    "y\":\"sustain2\",\"label\":\"Amp Sus\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.8,\"step\":0.01},{\"key\":\"keytra"
    "ck\",\"label\":\"Key Track\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},{\"key\":\"env_int\",\"labe"
    "l\":\"EG Int\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01}],\"knobs\":[\"attack1\",\"decay1\",\"sust"
    "ain1\",\"release1\",\"decay2\",\"sustain2\",\"keytrack\",\"env_int\"]},\"mix\":{\"name\":\"Mix/Filter [L1]\",\"label\":\"Mix/Filte"
    "r\",\"params\":[{\"key\":\"osc_mix\",\"label\":\"Osc Mix\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.0"
    "1},{\"key\":\"noise_level\",\"label\":\"Noise\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\""
    ":\"sync_ring\",\"label\":\"Sync / Ring\",\"type\":\"enum\",\"options\":[\"Off\",\"Ring\",\"Sync\",\"Ring Sync\"],\"short_options\""
    ":[\"OFF\",\"RING\",\"SYNC\",\"R.SNC\"],\"default\":0},{\"key\":\"filter_type\",\"label\":\"Filter Type\",\"type\":\"enum\",\"optio"
    "ns\":[\"LPF24\",\"LPF12\",\"BPF12\",\"HPF12\"],\"short_options\":[\"LPF24\",\"LPF12\",\"BPF12\",\"HPF12\"],\"default\":0},{\"key\""
    ":\"drive\",\"label\":\"Drive\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.2,\"step\":0.01},{\"key\":\"level\",\"labe"
    "l\":\"Level\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.9,\"step\":0.01},{\"key\":\"portamento\",\"label\":\"Portam"
    "ento\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"attack2\",\"label\":\"Amp Atk\",\"type"
    "\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.01,\"step\":0.01}],\"knobs\":[\"osc_mix\",\"noise_level\",\"sync_ring\",\"filte"
    "r_type\",\"drive\",\"level\",\"portamento\",\"attack2\"]},\"fx\":{\"name\":\"Effects\",\"label\":\"Effects\",\"params\":[{\"key\":"
    "\"chorus_mix\",\"label\":\"Chorus Mix\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"delay"
    "_time\",\"label\":\"Delay Time\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01},{\"key\":\"delay_feedba"
    "ck\",\"label\":\"Delay Fdbk\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01},{\"key\":\"delay_mix\",\"l"
    "abel\":\"Delay Mix\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.0,\"step\":0.01},{\"key\":\"master_vol\",\"label\":"
    "\"Master Vol\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.8,\"step\":0.01,\"short_name\":\"VOL\"},{\"key\":\"pan\","
    "\"label\":\"Pan\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},{\"key\":\"lfo1_rate\",\"label\":\"LFO"
    "1 Rate\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01,\"short_name\":\"LFO1\"},{\"key\":\"lfo2_rate\","
    "\"label\":\"LFO2 Rate\",\"type\":\"float\",\"min\":0.0,\"max\":1.0,\"default\":0.3,\"step\":0.01,\"short_name\":\"LFO2\"}],\"knobs"
    "\":[\"chorus_mix\",\"delay_time\",\"delay_feedback\",\"delay_mix\",\"master_vol\",\"pan\",\"lfo1_rate\",\"lfo2_rate\"]},\"arpset\""
    ":{\"name\":\"Arp Settings\",\"label\":\"Arp Settings\",\"params\":[{\"key\":\"arp_type\",\"label\":\"Type\",\"short_name\":\"TYPE"
    "\",\"type\":\"enum\",\"options\":[\"UP\",\"DOWN\",\"ALT1\",\"ALT2\",\"RANDOM\",\"TRIGGER\"],\"short_options\":[\"UP\",\"DOWN\",\"A"
    "LT1\",\"ALT2\",\"RND\",\"TRIG\"],\"default\":0},{\"key\":\"arp_range\",\"label\":\"Range\",\"short_name\":\"RANGE\",\"type\":\"enu"
    "m\",\"options\":[\"1 Oct\",\"2 Oct\",\"3 Oct\",\"4 Oct\"],\"short_options\":[\"1OCT\",\"2OCT\",\"3OCT\",\"4OCT\"],\"default\":0},{"
    "\"key\":\"arp_resolution\",\"label\":\"Resolution\",\"short_name\":\"RESO\",\"type\":\"enum\",\"options\":[\"1/24\",\"1/16\",\"1/1"
    "2\",\"1/8\",\"1/6\",\"1/4\"],\"short_options\":[\"1/24\",\"1/16\",\"1/12\",\"1/8\",\"1/6\",\"1/4\"],\"default\":1},{\"key\":\"arp_"
    "gate\",\"label\":\"Gate\",\"short_name\":\"GATE\",\"type\":\"int\",\"min\":0,\"max\":100,\"default\":80,\"unit\":\"%\"},{\"key\":"
    "\"arp_swing\",\"label\":\"Swing\",\"short_name\":\"SWING\",\"type\":\"int\",\"min\":-100,\"max\":100,\"default\":0,\"unit\":\"%\"}"
    ",{\"key\":\"arp_latch\",\"label\":\"Latch\",\"short_name\":\"LATCH\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"],\"short_option"
    "s\":[\"OFF\",\"ON\"],\"default\":0},{\"key\":\"arp_key_sync\",\"label\":\"Key Sync\",\"short_name\":\"KSYNC\",\"type\":\"enum\",\""
    "options\":[\"Off\",\"On\"],\"short_options\":[\"OFF\",\"ON\"],\"default\":0},{\"key\":\"arp_target\",\"label\":\"Target\",\"short_"
    "name\":\"TARGT\",\"type\":\"enum\",\"options\":[\"Both\",\"Layer 1\",\"Layer 2\"],\"short_options\":[\"BOTH\",\"L1\",\"L2\"],\"def"
    "ault\":0}],\"knobs\":[\"arp_type\",\"arp_range\",\"arp_resolution\",\"arp_gate\",\"arp_swing\",\"arp_latch\",\"arp_key_sync\",\"ar"
    "p_target\"]},\"steps\":{\"name\":\"Arp Steps\",\"label\":\"Arp Steps\",\"params\":[{\"key\":\"arp_step1\",\"label\":\"Step 1\",\"s"
    "hort_name\":\"ST1\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":"
    "{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step2\",\"label\":\"Step 2\",\"short_name\":\"S"
    "T2\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"cus"
    "tom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step3\",\"label\":\"Step 3\",\"short_name\":\"ST3\",\"type\":"
    "\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step"
    "\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step4\",\"label\":\"Step 4\",\"short_name\":\"ST4\",\"type\":\"enum\",\"opti"
    "ons\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys"
    "\":[\"arp_playhead\"]}},{\"key\":\"arp_step5\",\"label\":\"Step 5\",\"short_name\":\"ST5\",\"type\":\"enum\",\"options\":[\"Rest\""
    ",\"Play\"],\"short_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playh"
    "ead\"]}},{\"key\":\"arp_step6\",\"label\":\"Step 6\",\"short_name\":\"ST6\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"sh"
    "ort_options\":[\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key"
    "\":\"arp_step7\",\"label\":\"Step 7\",\"short_name\":\"ST7\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":["
    "\"REST\",\"PLAY\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}},{\"key\":\"arp_step8"
    "\",\"label\":\"Step 8\",\"short_name\":\"ST8\",\"type\":\"enum\",\"options\":[\"Rest\",\"Play\"],\"short_options\":[\"REST\",\"PLA"
    "Y\"],\"default\":1,\"viz\":{\"kind\":\"custom:tinyk_step\",\"extra_keys\":[\"arp_playhead\"]}}],\"knobs\":[\"arp_step1\",\"arp_ste"
    "p2\",\"arp_step3\",\"arp_step4\",\"arp_step5\",\"arp_step6\",\"arp_step7\",\"arp_step8\"]},\"bank\":{\"name\":\"Bank\",\"label\":"
    "\"Bank\",\"params\":[{\"key\":\"bank_file\",\"label\":\"Bank\",\"type\":\"enum\",\"options\":[\"Built-in\"],\"default\":0},{\"leve"
    "l\":\"bank_list\",\"label\":\"Browse banks\"}],\"knobs\":[\"bank_file\"]},\"bank_list\":{\"name\":\"Banks\",\"label\":\"Select Ban"
    "k\",\"items_param\":\"bank_list\",\"select_param\":\"bank_file\",\"navigate_to\":\"root\"}}}";

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

    if (strcmp(key, "preset") == 0) {
        return snprintf(buf, buf_len, "%d", synth->current_preset);
    }

    if (strcmp(key, "preset_count") == 0) {
        return snprintf(buf, buf_len, "%d", 128);
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
        int idx = atoi(key + 12);
        if (idx >= 0 && idx < 128) {
            return format_preset_name(synth, idx, buf, buf_len);
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
