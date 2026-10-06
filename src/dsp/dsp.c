#include "dsp.h"
#include "presets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TINYK_TUNING_DEFAULTS { 15.0f, 10.3f, 20.0f, 20000.0f, 8000.0f, 0.0f, 1.92f, 0.7f, 3.5f, 1.0f, 1.0f, 1.0f, 0.7f, 0.4f }
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
    { "voice_mode",     "Voice Mode",     "Mode",   0, 0.0f },
    { "timbre_edit",    "Timbre Edit",    "Edit",   0, 0.0f },
    { "timbre_balance", "Timbre Balance", "Bal",    0, 0.5f }
};

/* Real-time audio constraint: Zero dynamic allocations in dsp.c audio paths.
 * All voice states and buffers are statically allocated. */
static synth_engine_t g_synth;
static const host_api_v1_t *g_host = NULL;

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

/* Process single 2-pole TPT State Variable Filter with soft non-linear saturation
 * in the resonance feedback path and pre-filter drive stage, preserving low-end weight */
static inline float svf_process_2pole(svf_t *svf, float in, float fc, float res, float drive, filter_type_t type, float fs) {
    /* 1. NaN/Infinity sanitization on existing filter states */
    if (isnan(svf->s1) || isinf(svf->s1)) svf->s1 = 0.0f;
    if (isnan(svf->s2) || isinf(svf->s2)) svf->s2 = 0.0f;

    /* 2. Denormal flushing on existing filter states */
    if (fabsf(svf->s1) < 1e-15f) svf->s1 = 0.0f;
    if (fabsf(svf->s2) < 1e-15f) svf->s2 = 0.0f;

    /* 3. State clamping before feedback calculations */
    svf->s1 = fmaxf(-2.5f, fminf(2.5f, svf->s1));
    svf->s2 = fmaxf(-2.5f, fminf(2.5f, svf->s2));

    /* Clamp cutoff frequency to Nyquist safe range */
    if (isnan(fc) || isinf(fc)) fc = 1000.0f;
    if (fc < 20.0f) fc = 20.0f;
    if (fc > fs * 0.45f) fc = fs * 0.45f;

    /* g = tan(pi * fc / fs) */
    float g = tanf((float)M_PI * fc / fs);
    /* Resonance mapping: res in [0, 1] -> damping k in [2.0, 0.08]. Non-finite or out-of-range
     * values are sanitized so the feedback can never become negatively damped and blow up. */
    if (!(res >= 0.0f)) res = 0.0f;
    if (res > 1.0f) res = 1.0f;
    float k = 2.0f - tinyk_tuning.res_damping_range * res;
    if (k < 0.05f) k = 0.05f;

    /* Sanitize and clamp input signal */
    if (isnan(in) || isinf(in)) in = 0.0f;
    in = fmaxf(-3.0f, fminf(3.0f, in));

    /* Pre-filter drive stage with resonance gain compensation (compensating passband volume drop) */
    float input_gain = (1.0f + drive * tinyk_tuning.drive_gain) * (1.0f + res * 0.5f);
    float v0 = fast_tanh(in * input_gain);

    /* Soft non-linear saturation in the resonance feedback loop to prevent runaway spikes */
    float sat_s1 = fast_tanh(svf->s1);
    float denom = 1.0f + g * (g + k);
    if (denom < 1e-6f) denom = 1e-6f;
    float u = (v0 - k * sat_s1 - svf->s2) / denom;

    /* Soft-clip the resonance feedback loop */
    u = fast_tanh(u);

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

    svf->s1 = fmaxf(-2.0f, fminf(2.0f, next_s1));
    svf->s2 = fmaxf(-2.0f, fminf(2.0f, next_s2));

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
static inline float cutoff_to_hz(float cutoff) {
    return tinyk_tuning.cutoff_base_hz * powf(2.0f, cutoff * tinyk_tuning.cutoff_octaves);
}

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
}

/* Load preset from static FACTORY_PRESETS table into active engine and voices */
void load_preset(int index) {
    if (index < 0) index = 0;
    if (index >= 128) index = 127;

    float saved_timbre_balance = (g_synth.params[PARAM_TIMBRE_BALANCE] > 0.0f) ? g_synth.params[PARAM_TIMBRE_BALANCE] : g_synth.timbre_balance;

    const struct Preset *p = &FACTORY_PRESETS[index];

    g_synth.current_preset = index;
    int bank_side = index / 64;
    int genre_category = (index % 64) / 8;
    int slot_in_bank = index % 8;
    int program_num = (bank_side == 0) ? (slot_in_bank + 1) : (slot_in_bank + 9);

    g_synth.bank_side = bank_side;
    g_synth.genre_category = genre_category;
    g_synth.program_num = program_num;

    /* Timbre 1 into the active parameter set, then the shared FX section */
    apply_timbre(g_synth.params, &g_synth.timbre_extra[0], &p->t1);
    g_synth.params[PARAM_CHORUS_MIX] = clamp01f(p->chorus_mix);
    g_synth.params[PARAM_DELAY_TIME] = clamp01f(p->delay_time);
    g_synth.params[PARAM_DELAY_FEEDBACK] = clamp01f(p->delay_feedback);
    g_synth.params[PARAM_DELAY_MIX] = clamp01f(p->delay_mix);

    /* Parameters not stored in Preset struct: guarantee valid audible defaults */
    if (g_synth.params[PARAM_MASTER_VOL] <= 0.05f) {
        g_synth.params[PARAM_MASTER_VOL] = 0.8f;
    }
    if (g_synth.params[PARAM_PAN] <= 0.001f && g_synth.params[PARAM_PAN] >= -0.001f) {
        g_synth.params[PARAM_PAN] = 0.5f;
    }
    if (g_synth.params[PARAM_LFO1_RATE] <= 0.001f) {
        g_synth.params[PARAM_LFO1_RATE] = 0.3f;
    }
    if (g_synth.params[PARAM_LFO2_RATE] <= 0.001f) {
        g_synth.params[PARAM_LFO2_RATE] = 0.3f;
    }
    if (g_synth.params[PARAM_VEL_SENS] <= 0.001f) {
        g_synth.params[PARAM_VEL_SENS] = 0.3f;
    }

    /* Preset 0 specific audible defaults guarantee */
    if (index == 0) {
        if (g_synth.params[PARAM_MASTER_VOL] <= 0.05f) g_synth.params[PARAM_MASTER_VOL] = 0.8f;
        if (g_synth.params[PARAM_PAN] <= 0.001f && g_synth.params[PARAM_PAN] >= -0.001f) g_synth.params[PARAM_PAN] = 0.5f;
    }

    /* Copy loaded preset parameters into both timbre states */
    memcpy(g_synth.timbre_params[0], g_synth.params, sizeof(g_synth.params));
    memcpy(g_synth.timbre_params[1], g_synth.params, sizeof(g_synth.params));

    /* Timbre 2 gets its own complete parameter set (waves, tuning, filter, envelopes) */
    apply_timbre(g_synth.timbre_params[1], &g_synth.timbre_extra[1], &p->t2);

    g_synth.timbre_edit = 0;

    /* Set active voice_mode to the preset's native mode */
    int native_vm = (p->voice_mode == 1) ? 1 : 0;
    g_synth.voice_mode = native_vm;
    g_synth.params[PARAM_VOICE_MODE] = (native_vm == 1) ? 1.0f : 0.0f;
    g_synth.timbre_params[0][PARAM_VOICE_MODE] = g_synth.params[PARAM_VOICE_MODE];
    g_synth.timbre_params[1][PARAM_VOICE_MODE] = g_synth.params[PARAM_VOICE_MODE];

    float tb = (saved_timbre_balance > 0.0f) ? saved_timbre_balance : 0.5f;
    g_synth.timbre_balance = tb;
    g_synth.params[PARAM_TIMBRE_BALANCE] = tb;
    g_synth.timbre_params[0][PARAM_TIMBRE_BALANCE] = tb;
    g_synth.timbre_params[1][PARAM_TIMBRE_BALANCE] = tb;

    /* Smooth transition without audio pops: reset filter states of idle voices,
     * sanitize active voices without hard-zeroing in the middle of active oscillation */
    for (int v = 0; v < NUM_VOICES; v++) {
        if (!g_synth.voices[v].active || g_synth.voices[v].amp_env.stage == ENV_IDLE) {
            g_synth.voices[v].filter_svf[0].s1 = 0.0f;
            g_synth.voices[v].filter_svf[0].s2 = 0.0f;
            g_synth.voices[v].filter_svf[1].s1 = 0.0f;
            g_synth.voices[v].filter_svf[1].s2 = 0.0f;
        } else {
            if (isnan(g_synth.voices[v].filter_svf[0].s1) || isinf(g_synth.voices[v].filter_svf[0].s1)) g_synth.voices[v].filter_svf[0].s1 = 0.0f;
            if (isnan(g_synth.voices[v].filter_svf[0].s2) || isinf(g_synth.voices[v].filter_svf[0].s2)) g_synth.voices[v].filter_svf[0].s2 = 0.0f;
            if (isnan(g_synth.voices[v].filter_svf[1].s1) || isinf(g_synth.voices[v].filter_svf[1].s1)) g_synth.voices[v].filter_svf[1].s1 = 0.0f;
            if (isnan(g_synth.voices[v].filter_svf[1].s2) || isinf(g_synth.voices[v].filter_svf[1].s2)) g_synth.voices[v].filter_svf[1].s2 = 0.0f;
        }
    }
    if (isnan(g_synth.delay_filter_l) || isinf(g_synth.delay_filter_l)) g_synth.delay_filter_l = 0.0f;
    if (isnan(g_synth.delay_filter_r) || isinf(g_synth.delay_filter_r)) g_synth.delay_filter_r = 0.0f;
}

void synth_load_preset(synth_engine_t *synth, int preset_idx) {
    load_preset(preset_idx);

    if (synth && synth != &g_synth) {
        memcpy(synth->params, g_synth.params, sizeof(synth->params));
        memcpy(synth->timbre_params, g_synth.timbre_params, sizeof(synth->timbre_params));
        memcpy(synth->timbre_extra, g_synth.timbre_extra, sizeof(synth->timbre_extra));
        synth->current_preset = g_synth.current_preset;
        synth->bank_side = g_synth.bank_side;
        synth->genre_category = g_synth.genre_category;
        synth->program_num = g_synth.program_num;
        synth->timbre_edit = g_synth.timbre_edit;
        synth->timbre_balance = g_synth.timbre_balance;
        synth->voice_mode = g_synth.voice_mode;
        synth->params[PARAM_VOICE_MODE] = g_synth.params[PARAM_VOICE_MODE];
        synth->params[PARAM_TIMBRE_BALANCE] = g_synth.params[PARAM_TIMBRE_BALANCE];
    }
}

/* Synthesizer Initialization */
void synth_init(synth_engine_t *synth) {
    if (!synth) synth = &g_synth;
    memset(synth, 0, sizeof(*synth));

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
    load_preset(0);
    if (synth != &g_synth) {
        memcpy(synth->timbre_extra, g_synth.timbre_extra, sizeof(synth->timbre_extra));
    }

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

    if (synth != &g_synth) {
        memcpy(g_synth.params, synth->params, sizeof(g_synth.params));
        memcpy(g_synth.timbre_params, synth->timbre_params, sizeof(g_synth.timbre_params));
        g_synth.bank_side = 0;
        g_synth.genre_category = 0;
        g_synth.program_num = 1;
        g_synth.voice_mode = 0;
        g_synth.timbre_edit = 0;
        g_synth.timbre_balance = 0.5f;
        g_synth.params[PARAM_VOICE_MODE] = 0.0f;
        g_synth.params[PARAM_TIMBRE_EDIT] = 0.0f;
        g_synth.params[PARAM_TIMBRE_BALANCE] = 0.5f;
    }

    /* Initialize LFOs */
    synth->lfo1.sh_value = 0.0f;
    synth->lfo2.sh_value = 0.0f;
}

void synth_set_param(synth_engine_t *synth, const char *key, float val) {
    if (!synth) synth = &g_synth;

    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        int mode = (val >= 0.5f) ? 1 : 0;
        synth->voice_mode = mode;
        synth->params[PARAM_VOICE_MODE] = (mode == 1) ? 1.0f : 0.0f;
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        int t = (val >= 0.5f) ? 1 : 0;
        synth->timbre_edit = t;
        synth->params[PARAM_TIMBRE_EDIT] = (t == 1) ? 1.0f : 0.0f;
        return;
    }

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 ||
        strcmp(key, "Balance") == 0 || strcmp(key, "4") == 0 || strcmp(key, "param_4") == 0) {
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        synth->timbre_balance = val;
        synth->params[PARAM_TIMBRE_BALANCE] = val;
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
        load_preset(preset_index);
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
        load_preset(preset_index);
        return;
    }

    if (strcmp(key, "bank_side") == 0) {
        int side = (val >= 0.5f) ? 1 : 0;
        synth->bank_side = side;
        int slot = (synth->program_num > 8) ? (synth->program_num - 8) : synth->program_num;
        synth->program_num = (side == 0) ? slot : (slot + 8);
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(preset_index);
        return;
    }

    if (strcmp(key, "preset") == 0) {
        int idx = (val > 1.0f) ? (int)roundf(val) : (int)roundf(val * 127.0f);
        load_preset(idx);
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
    if (!synth) synth = &g_synth;
    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        return (float)synth->voice_mode;
    }
    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        return (float)synth->timbre_edit;
    }
    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 ||
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
    if (strcmp(key, "preset") == 0) {
        return (float)synth->current_preset / (float)(NUM_PRESETS - 1);
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

void synth_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity) {
    if (!synth) synth = &g_synth;

    if (velocity == 0) {
        synth_note_off(synth, note);
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

    if (!is_layer_mode) {
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

        if (!was_active || portamento < 0.005f) {
            v->current_pitch = target_pitch;
        }
        v->target_pitch = target_pitch;

        if (!was_active) {
            v->filter_svf[0].s1 = 0.0f;
            v->filter_svf[0].s2 = 0.0f;
            v->filter_svf[1].s1 = 0.0f;
            v->filter_svf[1].s2 = 0.0f;
            v->osc1_phase = 0.0f;
            v->osc2_phase = 0.0f;
            v->sub_phase = 0.0f;
            v->amp_env.value = 0.0f;
            v->filter_env.value = 0.0f;
        } else {
            v->filter_svf[0].s1 *= 0.05f;
            v->filter_svf[0].s2 *= 0.05f;
            v->filter_svf[1].s1 *= 0.05f;
            v->filter_svf[1].s2 *= 0.05f;
        }

        adsr_gate_on(&v->filter_env, t1_atk1_coef);
        adsr_gate_on(&v->amp_env, t1_atk2_coef);

    } else {
        /* =========================================================
         * LAYER MODE (2-Voice Polyphony with 2 Linked Voice Instances each)
         * Slot 0: Voices 0 (Voice A / Timbre 1) & 1 (Voice B / Timbre 2)
         * Slot 1: Voices 2 (Voice A / Timbre 1) & 3 (Voice B / Timbre 2)
         * Polyphony is strictly clamped to 2 simultaneous note triggers.
         * ========================================================= */
        float portamento1 = synth->timbre_params[0][PARAM_PORTAMENTO];
        float portamento2 = synth->timbre_params[1][PARAM_PORTAMENTO];
        int slot = -1;

        /* 1. Retrigger if note is already sounding in Slot 0 or 1 */
        if (synth->voices[0].active && synth->voices[0].note == note) {
            slot = 0;
        } else if (synth->voices[2].active && synth->voices[2].note == note) {
            slot = 1;
        }

        /* 2. Find idle slot */
        if (slot < 0) {
            bool slot0_idle = (!synth->voices[0].active || synth->voices[0].amp_env.stage == ENV_IDLE);
            bool slot1_idle = (!synth->voices[2].active || synth->voices[2].amp_env.stage == ENV_IDLE);
            if (slot0_idle) {
                slot = 0;
            } else if (slot1_idle) {
                slot = 1;
            }
        }

        /* 3. Slot Stealing: strictly clamp to 2 simultaneous notes */
        if (slot < 0) {
            bool slot0_released = (!synth->voices[0].gate);
            bool slot1_released = (!synth->voices[2].gate);

            if (slot0_released && !slot1_released) {
                slot = 0;
            } else if (!slot0_released && slot1_released) {
                slot = 1;
            } else if (slot0_released && slot1_released) {
                float amp0 = fmaxf(synth->voices[0].amp_env.value, synth->voices[1].amp_env.value);
                float amp1 = fmaxf(synth->voices[2].amp_env.value, synth->voices[3].amp_env.value);
                slot = (amp0 <= amp1) ? 0 : 1;
            } else {
                uint32_t age0 = synth->voices[0].age;
                uint32_t age1 = synth->voices[2].age;
                slot = (age0 <= age1) ? 0 : 1;
            }
        }

        if (slot < 0 || slot > 1) slot = 0;

        int i1 = slot * 2;
        int i2 = slot * 2 + 1;

        voice_t *v1 = &synth->voices[i1];
        voice_t *v2 = &synth->voices[i2];

        bool was_active1 = v1->active && (v1->amp_env.stage != ENV_IDLE);
        bool was_active2 = v2->active && (v2->amp_env.stage != ENV_IDLE);

        /* Configure Voice A (Timbre 1): base patch pitch */
        v1->active = true;
        v1->gate = true;
        v1->note = note;
        v1->velocity = vel01;
        v1->age = synth->voice_counter;
        v1->timbre_index = 0;
        v1->is_timbre_2 = 0;
        v1->layer_partner = i2;

        if (!was_active1 || portamento1 < 0.005f) {
            v1->current_pitch = target_pitch;
        }
        v1->target_pitch = target_pitch;

        if (!was_active1) {
            v1->filter_svf[0].s1 = 0.0f;
            v1->filter_svf[0].s2 = 0.0f;
            v1->filter_svf[1].s1 = 0.0f;
            v1->filter_svf[1].s2 = 0.0f;
            v1->osc1_phase = 0.0f;
            v1->osc2_phase = 0.0f;
            v1->sub_phase = 0.0f;
            v1->amp_env.value = 0.0f;
            v1->filter_env.value = 0.0f;
        } else {
            v1->filter_svf[0].s1 *= 0.05f;
            v1->filter_svf[0].s2 *= 0.05f;
            v1->filter_svf[1].s1 *= 0.05f;
            v1->filter_svf[1].s2 *= 0.05f;
        }

        /* Configure Voice B (Timbre 2): authentic Timbre 2 parameters */
        v2->active = true;
        v2->gate = true;
        v2->note = note;
        v2->velocity = vel01;
        v2->age = synth->voice_counter;
        v2->timbre_index = 1;
        v2->is_timbre_2 = 1;
        v2->layer_partner = i1;

        if (!was_active2 || portamento2 < 0.005f) {
            v2->current_pitch = target_pitch;
        }
        v2->target_pitch = target_pitch;

        if (!was_active2) {
            v2->filter_svf[0].s1 = 0.0f;
            v2->filter_svf[0].s2 = 0.0f;
            v2->filter_svf[1].s1 = 0.0f;
            v2->filter_svf[1].s2 = 0.0f;
            /* 90-degree initial phase offset prevents comb filtering cancellation */
            v2->osc1_phase = 0.25f;
            v2->osc2_phase = 0.25f;
            v2->sub_phase = 0.25f;
            v2->amp_env.value = 0.0f;
            v2->filter_env.value = 0.0f;
        } else {
            v2->filter_svf[0].s1 *= 0.05f;
            v2->filter_svf[0].s2 *= 0.05f;
            v2->filter_svf[1].s1 *= 0.05f;
            v2->filter_svf[1].s2 *= 0.05f;
        }

        /* Trigger envelopes on both linked timbres with their respective timbre settings */
        adsr_gate_on(&v1->filter_env, t1_atk1_coef);
        adsr_gate_on(&v1->amp_env, t1_atk2_coef);
        adsr_gate_on(&v2->filter_env, t2_atk1_coef);
        adsr_gate_on(&v2->amp_env, t2_atk2_coef);
    }
}

void synth_note_off(synth_engine_t *synth, uint8_t note) {
    if (!synth) synth = &g_synth;

    for (int i = 0; i < NUM_VOICES; i++) {
        if (synth->voices[i].active && synth->voices[i].note == note) {
            if (synth->voices[i].gate) {
                synth->voices[i].gate = false;
                adsr_gate_off(&synth->voices[i].filter_env);
                adsr_gate_off(&synth->voices[i].amp_env);
            }

            int partner = synth->voices[i].layer_partner;
            if (partner >= 0 && partner < NUM_VOICES && synth->voices[partner].active) {
                if (synth->voices[partner].gate) {
                    synth->voices[partner].gate = false;
                    adsr_gate_off(&synth->voices[partner].filter_env);
                    adsr_gate_off(&synth->voices[partner].amp_env);
                }
            }
        }
    }
}

void synth_all_notes_off(synth_engine_t *synth) {
    if (!synth) synth = &g_synth;
    for (int i = 0; i < NUM_VOICES; i++) {
        synth->voices[i].gate = false;
        synth->voices[i].active = false;
        synth->voices[i].amp_env.stage = ENV_IDLE;
        synth->voices[i].amp_env.value = 0.0f;
        synth->voices[i].filter_env.stage = ENV_IDLE;
        synth->voices[i].filter_env.value = 0.0f;
        synth->voices[i].filter_svf[0].s1 = 0.0f;
        synth->voices[i].filter_svf[0].s2 = 0.0f;
        synth->voices[i].filter_svf[1].s1 = 0.0f;
        synth->voices[i].filter_svf[1].s2 = 0.0f;
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
void synth_render(synth_engine_t *synth, int16_t *out_lr, int frames) {
    if (!synth) synth = &g_synth;
    if (!out_lr || frames <= 0) return;

    if (synth->params[PARAM_MASTER_VOL] <= 0.01f) {
        synth_init(synth);
    }

    const float fs = (float)MOVE_SAMPLE_RATE;

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

    /* Stereo panning for Layer mode: Voice A (Timbre 1) at -0.3 (left), Voice B (Timbre 2)
     * at +0.3 (right). Constant-power pan law scaled so a centered voice has unity gain. */
    const float layer_pan = 0.3f;
    float pan_a_l = cosf((1.0f - layer_pan) * 0.25f * (float)M_PI) * 1.4142f;
    float pan_a_r = sinf((1.0f - layer_pan) * 0.25f * (float)M_PI) * 1.4142f;
    float pan_b_l = cosf((1.0f + layer_pan) * 0.25f * (float)M_PI) * 1.4142f;
    float pan_b_r = sinf((1.0f + layer_pan) * 0.25f * (float)M_PI) * 1.4142f;

    /* Precompute per-timbre render configurations (Zero heap allocation) */
    typedef struct {
        int osc1_wave;
        float pw;
        int osc2_wave;
        float detune_semi;
        int sync_ring_mode;
        float osc_mix;
        float sub_level;
        float glide_coeff;
        float transpose_semi;
        float noise_level;
        float level;
        const float *wavetable;

        float base_fc;
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
        t_cfg[t].transpose_semi = extra->transpose_semi;
        t_cfg[t].noise_level = extra->noise_level;
        t_cfg[t].level = extra->level;

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

        t_cfg[t].base_fc = cutoff_to_hz(cutoff_p);
        t_cfg[t].resonance = resonance_p;
        t_cfg[t].filter_type = (filter_type_t)(int)(filter_type_p * (float)(FILTER_TYPE_COUNT - 1) + 0.5f);
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
    }

    /* LFO Frequencies: 0.05 Hz to 30 Hz */
    float lfo1_freq = 0.05f * powf(600.0f, lfo1_rate_p);
    float lfo2_freq = 0.05f * powf(600.0f, lfo2_rate_p);
    float lfo1_dt = lfo1_freq / fs;
    float lfo2_dt = lfo2_freq / fs;

    /* Delay config. The hardware has one delay depth that sets both the repeats and the level;
     * depth 0 means the delay is off. Feedback is capped at 0.6 so repeats die away instead of
     * building into a pseudo-reverb wash. */
    float target_delay_samples = 100.0f + delay_time_p * (float)(DELAY_BUFFER_SIZE - 200);
    float delay_feedback = fminf(delay_fdbk_p * 0.75f, 0.6f);
    float delay_send = delay_mix_p * tinyk_tuning.delay_send_scale;
    int delay_on = (delay_mix_p > 0.005f);

    /* Pan: Left / Right gains */
    float pan_l = cosf(pan_p * (float)(M_PI * 0.5)) * 1.4142f;
    float pan_r = sinf(pan_p * (float)(M_PI * 0.5)) * 1.4142f;

    static uint32_t noise_state = 0x1234ABCDu;

    for (int s = 0; s < frames; s++) {
        /* 1. Update LFO 1 */
        synth->lfo1.phase += lfo1_dt;
        if (synth->lfo1.phase >= 1.0f) {
            synth->lfo1.phase -= 1.0f;
            synth->lfo1.sh_value = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }
        float lfo1_val = 2.0f * fabsf(2.0f * synth->lfo1.phase - 1.0f) - 1.0f;

        /* 2. Update LFO 2 */
        synth->lfo2.phase += lfo2_dt;
        if (synth->lfo2.phase >= 1.0f) {
            synth->lfo2.phase -= 1.0f;
            synth->lfo2.sh_value = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }
        float lfo2_val = sinf(2.0f * (float)M_PI * synth->lfo2.phase);

        /* White noise source shared by all voices (xorshift32, no heap, no libc rand) */
        noise_state ^= noise_state << 13;
        noise_state ^= noise_state >> 17;
        noise_state ^= noise_state << 5;
        float white_noise = (float)(int32_t)noise_state * (1.0f / 2147483648.0f);

        /* 3. Render Voices */
        float voice_sum_l = 0.0f;
        float voice_sum_r = 0.0f;

        for (int v_idx = 0; v_idx < NUM_VOICES; v_idx++) {
            voice_t *v = &synth->voices[v_idx];
            if (!v->active) continue;

            int t_idx = is_layer_mode ? v->is_timbre_2 : 0;
            const timbre_render_cfg_t *cfg = &t_cfg[t_idx];

            /* Portamento Pitch Glide */
            v->current_pitch += (v->target_pitch - v->current_pitch) * cfg->glide_coeff;

            /* LFO1 pitch mod (vibrato) + Pitch Bend */
            float pitch_mod = lfo1_val * (cfg->mod_int * 0.5f) + synth->pitch_bend_semi;

            float final_note1 = v->current_pitch + cfg->transpose_semi + pitch_mod;
            float freq1 = note_to_freq(final_note1);
            float dt1 = freq1 / fs;
            if (dt1 > 0.45f) dt1 = 0.45f;

            float final_note2 = v->current_pitch + cfg->transpose_semi + cfg->detune_semi + pitch_mod;
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
                    float pw = cfg->pw;
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
                    osc1_out = white_noise;
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
                          + cfg->sub_level * sub_out + cfg->noise_level * white_noise)
                          * cfg->level * tinyk_tuning.mixer_trim;

            /* --- Envelopes (Exponential Curves) --- */
            float f_env = adsr_process(&v->filter_env, cfg->dcy1_coef, cfg->sustain1, cfg->rel1_coef);
            float a_env = adsr_process(&v->amp_env, cfg->dcy2_coef, cfg->sustain2, cfg->rel2_coef);

            /* Clean voice deactivation when envelope finishes or drops to zero while released */
            if (v->amp_env.stage == ENV_IDLE || (!v->gate && a_env <= 0.0005f)) {
                v->active = false;
                v->gate = false;
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
            /* Key tracking is bipolar around 0.5 (0.5 = none, 1.0 = one octave per octave) */
            float keytrack_mod = (v->note - 60.0f) * ((cfg->keytrack * 2.0f - 1.0f) * (1.0f / 12.0f));
            float lfo2_mod = cfg->mod_int * lfo2_val * 2.0f;
            float cutoff_hz = cfg->base_fc * powf(2.0f, fmaxf(-10.0f, fminf(10.0f, keytrack_mod + lfo2_mod)));
            cutoff_hz = fmaxf(tinyk_tuning.cutoff_floor_hz, fminf(tinyk_tuning.cutoff_ceil_hz, cutoff_hz)); /* key tracking can never push it out of range */

            /* Bipolar filter envelope: env_int 0.5 = off, > 0.5 opens, < 0.5 closes on note strike */
            float bipolar_env = (cfg->env_int - 0.5f) * 2.0f;
            bipolar_env *= (1.0f - cfg->vel_sens + cfg->vel_sens * v->velocity);
            float eg1_val = f_env;
            if (tinyk_tuning.env_octaves != 0.0f) {
                cutoff_hz *= powf(2.0f, bipolar_env * eg1_val * tinyk_tuning.env_octaves);
            }
            cutoff_hz = fmaxf(tinyk_tuning.cutoff_floor_hz, fminf(tinyk_tuning.cutoff_ceil_hz,
                              cutoff_hz + (bipolar_env * eg1_val * tinyk_tuning.env_depth_hz)));
            float fc = cutoff_hz;

            /* SVF Multimode Filter with feedback tanh saturation & bass preservation */
            float filtered = 0.0f;
            if (cfg->filter_type == FILTER_LP_24) {
                float stage1 = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance * tinyk_tuning.lp24_res_scale, cfg->drive, FILTER_LP_12, fs);
                filtered = svf_process_2pole(&v->filter_svf[1], stage1, fc, cfg->resonance * tinyk_tuning.lp24_res_scale, cfg->drive, FILTER_LP_12, fs);
            } else {
                filtered = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance, cfg->drive, cfg->filter_type, fs);
            }

            if (isnan(filtered) || isinf(filtered)) filtered = 0.0f;

            float voice_vel = v->velocity;
            if (voice_vel <= 0.01f) voice_vel = 0.8f;

            /* Amp Envelope & Velocity Scaling */
            float voice_audio = filtered * a_env * voice_vel;
            DIAG_CHECK(voice_audio);
            if (isnan(voice_audio) || isinf(voice_audio)) voice_audio = 0.0f;

            /* Stereo pan spread & gain scaling. Layer mode: Voice A (Timbre 1) panned left and
             * scaled by its balance share, Voice B (Timbre 2) panned right and scaled likewise. */
            float v_gain_l = 1.0f;
            float v_gain_r = 1.0f;

            if (is_layer_mode) {
                if (v->is_timbre_2 == 0) {
                    /* Voice A: Timbre 1, panned left */
                    v_gain_l = tA_vol * pan_a_l;
                    v_gain_r = tA_vol * pan_a_r;
                } else {
                    /* Voice B: Timbre 2, panned right */
                    v_gain_l = tB_vol * pan_b_l;
                    v_gain_r = tB_vol * pan_b_r;
                }
            } else {
                /* Single mode: centered at unity; the soft clip below handles 4-voice overs */
                v_gain_l = 1.0f;
                v_gain_r = 1.0f;
            }

            voice_sum_l += voice_audio * v_gain_l;
            voice_sum_r += voice_audio * v_gain_r;
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

        /* 4. Stereo Chorus / Ensemble */
        float chorus_l = voice_sum_l;
        float chorus_r = voice_sum_r;

        if (chorus_mix_p > 0.01f) {
            synth->chorus_lfo_phase += 0.8f / fs;
            if (synth->chorus_lfo_phase >= 1.0f) synth->chorus_lfo_phase -= 1.0f;

            float lfo_c1 = sinf(2.0f * (float)M_PI * synth->chorus_lfo_phase);
            float lfo_c2 = cosf(2.0f * (float)M_PI * synth->chorus_lfo_phase);

            float delay_mod_l = 441.0f + lfo_c1 * 220.0f;
            float delay_mod_r = 441.0f + lfo_c2 * 220.0f;

            uint32_t wpos = synth->chorus_write_pos;
            synth->chorus_buf_l[wpos] = voice_sum_l;
            synth->chorus_buf_r[wpos] = voice_sum_r;
            synth->chorus_write_pos = (wpos + 1) % CHORUS_BUFFER_SIZE;

            float rpos_l = (float)wpos - delay_mod_l;
            while (rpos_l < 0.0f) rpos_l += (float)CHORUS_BUFFER_SIZE;
            int idx_l0 = (int)rpos_l % CHORUS_BUFFER_SIZE;
            int idx_l1 = (idx_l0 + 1) % CHORUS_BUFFER_SIZE;
            float frac_l = rpos_l - (int)rpos_l;
            float tap_l = synth->chorus_buf_l[idx_l0] * (1.0f - frac_l) + synth->chorus_buf_l[idx_l1] * frac_l;

            float rpos_r = (float)wpos - delay_mod_r;
            while (rpos_r < 0.0f) rpos_r += (float)CHORUS_BUFFER_SIZE;
            int idx_r0 = (int)rpos_r % CHORUS_BUFFER_SIZE;
            int idx_r1 = (idx_r0 + 1) % CHORUS_BUFFER_SIZE;
            float frac_r = rpos_r - (int)rpos_r;
            float tap_r = synth->chorus_buf_r[idx_r0] * (1.0f - frac_r) + synth->chorus_buf_r[idx_r1] * frac_r;

            /* Proper dry/wet crossfading */
            chorus_l = voice_sum_l * (1.0f - chorus_mix_p * 0.7f) + tap_l * chorus_mix_p;
            chorus_r = voice_sum_r * (1.0f - chorus_mix_p * 0.7f) + tap_r * chorus_mix_p;
        }

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

static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    synth_init(&g_synth);
    if (json_defaults) {
        const char *vm = strstr(json_defaults, "\"voice_mode\"");
        if (vm) {
            const char *colon = strchr(vm, ':');
            if (colon) {
                while (*colon == ' ' || *colon == '\t' || *colon == ':') colon++;
                if (*colon == '1' || strncmp(colon, "\"Layer", 6) == 0) {
                    g_synth.voice_mode = 1;
                    g_synth.params[PARAM_VOICE_MODE] = 1.0f;
                }
            }
        }
    }
    return &g_synth;
}

static void v2_destroy_instance(void *instance) {
    (void)instance;
}

static void parse_midi_buffer(synth_engine_t *synth, const uint8_t *msg, int len) {
    if (!synth) synth = &g_synth;
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
                        synth_note_on(synth, note, vel);
                    } else {
                        synth_note_off(synth, note);
                    }
                } else {
                    break;
                }
            } else if (cmd == 0x80) { /* Note Off */
                if (i + 1 < len) {
                    uint8_t note = msg[i] & 0x7F;
                    i += 2;
                    synth_note_off(synth, note);
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
                    synth->pitch_bend_semi = ((float)(bend - 8192) / 8192.0f) * 2.0f;
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
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : &g_synth;
    parse_midi_buffer(synth, msg, len);
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    synth_engine_t *synth = (synth_engine_t*)instance;
    if (!synth || !key || !val) return;

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
        load_preset(preset_index);
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
        load_preset(preset_index);
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
        synth->bank_side = side;
        int slot = (synth->program_num > 8) ? (synth->program_num - 8) : synth->program_num;
        synth->program_num = (side == 0) ? slot : (slot + 8);
        int bank_offset = (synth->program_num > 8) ? 64 : 0;
        int slot_in_bank = (synth->program_num > 8) ? (synth->program_num - 9) : (synth->program_num - 1);
        int preset_index = bank_offset + (synth->genre_category * 8) + slot_in_bank;
        load_preset(preset_index);
        return;
    }

    if (strcmp(key, "preset") == 0) {
        float f = (float)atof(val);
        int idx = (f > 1.0f) ? (int)roundf(f) : (int)roundf(f * 127.0f);
        load_preset(idx);
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

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 ||
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

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 ||
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

/* UI Hierarchy JSON serving Move's OLED screen matching module.json */
static const char MK_UI_HIERARCHY[] =
    "{\"levels\":{\"root\":{\"label\":\"TinyK\","
    "\"params\":["
    "{\"key\":\"genre_category\",\"label\":\"Genre\",\"type\":\"enum\"},"
    "{\"key\":\"program_num\",\"label\":\"Program\",\"type\":\"int\",\"min\":1,\"max\":16,\"default\":1},"
    "{\"key\":\"voice_mode\",\"label\":\"Voice Mode\",\"type\":\"enum\"},"
    "{\"key\":\"timbre_edit\",\"label\":\"Timbre Edit\",\"type\":\"enum\"},"
    "{\"key\":\"timbre_balance\",\"label\":\"Timbre Bal\",\"type\":\"float\"},"
    "{\"key\":\"wave1\",\"label\":\"Wave 1\"},"
    "{\"key\":\"pulse_width\",\"label\":\"Pulse Width\"},"
    "{\"key\":\"wave2\",\"label\":\"Wave 2\"},"
    "{\"key\":\"detune\",\"label\":\"Detune\"},"
    "{\"key\":\"sync_ring\",\"label\":\"Sync / Ring\"},"
    "{\"key\":\"osc_mix\",\"label\":\"Osc Mix\"},"
    "{\"key\":\"sub_level\",\"label\":\"Sub Level\"},"
    "{\"key\":\"portamento\",\"label\":\"Portamento\"},"
    "{\"key\":\"cutoff\",\"label\":\"Cutoff\"},"
    "{\"key\":\"resonance\",\"label\":\"Resonance\"},"
    "{\"key\":\"filter_type\",\"label\":\"Filter Type\"},"
    "{\"key\":\"keytrack\",\"label\":\"Key Track\"},"
    "{\"key\":\"env_int\",\"label\":\"Env Intensity\"},"
    "{\"key\":\"drive\",\"label\":\"Drive\"},"
    "{\"key\":\"attack1\",\"label\":\"Filter Atk\"},"
    "{\"key\":\"decay1\",\"label\":\"Filter Dcy\"},"
    "{\"key\":\"sustain1\",\"label\":\"Filter Sus\"},"
    "{\"key\":\"release1\",\"label\":\"Filter Rel\"},"
    "{\"key\":\"attack2\",\"label\":\"Amp Atk\"},"
    "{\"key\":\"decay2\",\"label\":\"Amp Dcy\"},"
    "{\"key\":\"sustain2\",\"label\":\"Amp Sus\"},"
    "{\"key\":\"release2\",\"label\":\"Amp Rel\"},"
    "{\"key\":\"chorus_mix\",\"label\":\"Chorus Mix\"},"
    "{\"key\":\"delay_time\",\"label\":\"Delay Time\"},"
    "{\"key\":\"delay_feedback\",\"label\":\"Delay Fdbk\"},"
    "{\"key\":\"delay_mix\",\"label\":\"Delay Mix\"}"
    "],"
    "\"knobs\":[\"genre_category\",\"program_num\",\"voice_mode\",\"timbre_edit\",\"timbre_balance\",\"wave1\",\"pulse_width\",\"wave2\",\"detune\",\"sync_ring\",\"osc_mix\",\"sub_level\",\"portamento\",\"cutoff\",\"resonance\",\"filter_type\",\"keytrack\",\"env_int\",\"drive\",\"attack1\",\"decay1\",\"sustain1\",\"release1\",\"attack2\",\"decay2\",\"sustain2\",\"release2\",\"chorus_mix\",\"delay_time\",\"delay_feedback\",\"delay_mix\"]"
    "}}}";

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    synth_engine_t *synth = (synth_engine_t*)instance;
    if (!synth || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len, "%s", MK_UI_HIERARCHY);
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

    if (strcmp(key, "voice_mode") == 0 || strcmp(key, "Voice Mode") == 0 ||
        strcmp(key, "Mode") == 0 || strcmp(key, "2") == 0 || strcmp(key, "param_2") == 0) {
        return snprintf(buf, buf_len, "%d", synth->voice_mode);
    }

    if (strcmp(key, "timbre_edit") == 0 || strcmp(key, "Timbre Edit") == 0 ||
        strcmp(key, "Edit") == 0 || strcmp(key, "3") == 0 || strcmp(key, "param_3") == 0) {
        return snprintf(buf, buf_len, "%d", synth->timbre_edit);
    }

    if (strcmp(key, "timbre_balance") == 0 || strcmp(key, "Timbre Bal") == 0 ||
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
        char bank = (synth->program_num > 8) ? 'B' : 'A';
        int genre_num = synth->genre_category + 1;
        int slot = (synth->program_num > 8) ? (synth->program_num - 8) : synth->program_num;
        const char *raw_name = FACTORY_PRESETS[synth->current_preset].label;
        const char *name_part = raw_name;
        if ((raw_name[0] == 'A' || raw_name[0] == 'B') && raw_name[1] == '.') {
            const char *sp = strchr(raw_name, ' ');
            if (sp) name_part = sp + 1;
        }
        return snprintf(buf, buf_len, "%c.%d%d %s", bank, genre_num, slot, name_part);
    }

    if (strncmp(key, "preset_name:", 12) == 0) {
        int idx = atoi(key + 12);
        if (idx >= 0 && idx < 128) {
            char bank = (idx >= 64) ? 'B' : 'A';
            int g_num = ((idx % 64) / 8) + 1;
            int s_num = (idx % 8) + 1;
            const char *raw_name = FACTORY_PRESETS[idx].label;
            const char *name_part = raw_name;
            if ((raw_name[0] == 'A' || raw_name[0] == 'B') && raw_name[1] == '.') {
                const char *sp = strchr(raw_name, ' ');
                if (sp) name_part = sp + 1;
            }
            return snprintf(buf, buf_len, "%c.%d%d %s", bank, g_num, s_num, name_part);
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
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : &g_synth;
    if (!out_interleaved_lr || frames <= 0) return;
    synth_render(synth, out_interleaved_lr, frames);
}

static void v2_audio_fx_process_block(void *instance, const int16_t *in_interleaved_lr, int16_t *out_interleaved_lr, int frames) {
    (void)in_interleaved_lr;
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : &g_synth;
    if (!out_interleaved_lr || frames <= 0) return;
    synth_render(synth, out_interleaved_lr, frames);
}

static plugin_api_v2_t g_plugin_api_v2;

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
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
        parse_midi_buffer(&g_synth, actual_msg, actual_len);
        return;
    }
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : &g_synth;
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
            synth_render(&g_synth, actual_out, actual_frames);
        }
        return;
    }
    synth_engine_t *synth = instance ? (synth_engine_t*)instance : &g_synth;
    if (out_interleaved_lr && frames > 0) {
        synth_render(synth, out_interleaved_lr, frames);
    }
}
