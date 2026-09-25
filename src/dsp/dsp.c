#include "dsp.h"
#include "presets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
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
    { "pulse_width",  "Pulse Width",    "Width",  1, 0.5f },
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
    { "pan",          "Pan",            "Pan",    4, 0.5f }
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

/* ADSR calculation helpers */
static inline float time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    /* Exponential curve for decay/release time control */
    float sec = min_sec * powf(max_sec / min_sec, time_val_01);
    if (sec < 0.0005f) sec = 0.0005f;
    float coeff = expf(-4.60517f / (sec * fs));
    if (coeff < 0.0f) coeff = 0.0f;
    if (coeff > 0.999999f) coeff = 0.999999f;
    return coeff;
}

static inline float attack_time_to_coeff(float time_val_01, float min_sec, float max_sec, float fs) {
    /* Analog RC exponential curve for attack time control targeting 1.35 overshoot for punch */
    float sec = min_sec * powf(max_sec / min_sec, time_val_01);
    if (sec < 0.0005f) sec = 0.0005f;
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
    /* Resonance mapping: res in [0, 1] -> damping k in [2.0, 0.08] */
    float k = 2.0f - 1.92f * res;

    /* Sanitize and clamp input signal */
    if (isnan(in) || isinf(in)) in = 0.0f;
    in = fmaxf(-4.0f, fminf(4.0f, in));

    /* Pre-filter drive stage with soft non-linear saturation */
    float drive_gain = 1.0f + drive * 5.0f;
    float v0 = fast_tanh(in * drive_gain);

    /* Low-end weight preservation: compensate for passband attenuation under high resonance */
    float v0_comp = v0 * (1.0f + 0.65f * res);

    /* Soft non-linear saturation in the resonance feedback path */
    float sat_s1 = fast_tanh(svf->s1 * (1.0f + 0.5f * res));
    float denom = 1.0f + g * (g + k);
    if (denom < 1e-6f) denom = 1e-6f;
    float u = (v0_comp - k * sat_s1 - svf->s2) / denom;

    /* Sanitize feedback term u */
    if (isnan(u) || isinf(u)) u = 0.0f;
    u = fmaxf(-4.0f, fminf(4.0f, u));

    /* Soft saturation on integrator bandpass state */
    float v1 = fast_tanh(g * u + svf->s1);
    float next_s1 = g * u + v1;

    float v2 = g * v1 + svf->s2;
    float next_s2 = g * v1 + v2;

    /* NaN/Infinity sanitization and state clamping */
    if (isnan(next_s1) || isinf(next_s1)) next_s1 = 0.0f;
    if (isnan(next_s2) || isinf(next_s2)) next_s2 = 0.0f;
    if (fabsf(next_s1) < 1e-15f) next_s1 = 0.0f;
    if (fabsf(next_s2) < 1e-15f) next_s2 = 0.0f;

    svf->s1 = fmaxf(-2.5f, fminf(2.5f, next_s1));
    svf->s2 = fmaxf(-2.5f, fminf(2.5f, next_s2));

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

    if (isnan(out) || isinf(out)) out = 0.0f;
    return fmaxf(-2.5f, fminf(2.5f, out));
}

/* Load preset from static FACTORY_PRESETS table into active engine and voices */
void load_preset(int index) {
    if (index < 0) index = 0;
    if (index >= 128) index = 127;

    const struct Preset *p = &FACTORY_PRESETS[index];

    g_synth.current_preset = index;
    int bank_side = index / 64;
    int genre_category = (index % 64) / 8;
    int slot_in_bank = index % 8;
    int program_num = (bank_side == 0) ? (slot_in_bank + 1) : (slot_in_bank + 9);

    g_synth.bank_side = bank_side;
    g_synth.genre_category = genre_category;
    g_synth.program_num = program_num;

    /* Copy preset values directly into the active voice parameters */
    g_synth.params[PARAM_WAVE1] = fmaxf(0.0f, fminf(1.0f, p->wave1));
    g_synth.params[PARAM_PULSE_WIDTH] = fmaxf(0.0f, fminf(1.0f, p->pulse_width));
    g_synth.params[PARAM_WAVE2] = fmaxf(0.0f, fminf(1.0f, p->wave2));
    g_synth.params[PARAM_DETUNE] = fmaxf(0.0f, fminf(1.0f, p->detune));
    g_synth.params[PARAM_SYNC_RING] = fmaxf(0.0f, fminf(1.0f, p->sync_ring));
    g_synth.params[PARAM_OSC_MIX] = fmaxf(0.0f, fminf(1.0f, p->osc_mix));
    g_synth.params[PARAM_SUB_LEVEL] = fmaxf(0.0f, fminf(1.0f, p->sub_level));
    g_synth.params[PARAM_PORTAMENTO] = fmaxf(0.0f, fminf(1.0f, p->portamento));
    g_synth.params[PARAM_CUTOFF] = fmaxf(0.0f, fminf(1.0f, p->cutoff));
    g_synth.params[PARAM_RESONANCE] = fmaxf(0.0f, fminf(1.0f, p->resonance));
    g_synth.params[PARAM_FILTER_TYPE] = fmaxf(0.0f, fminf(1.0f, p->filter_type));
    g_synth.params[PARAM_KEYTRACK] = fmaxf(0.0f, fminf(1.0f, p->keytrack));
    g_synth.params[PARAM_ENV_INT] = fmaxf(0.0f, fminf(1.0f, p->env_int));
    g_synth.params[PARAM_DRIVE] = fmaxf(0.0f, fminf(1.0f, p->drive));
    g_synth.params[PARAM_ATTACK1] = fmaxf(0.0f, fminf(1.0f, p->attack1));
    g_synth.params[PARAM_DECAY1] = fmaxf(0.0f, fminf(1.0f, p->decay1));
    g_synth.params[PARAM_SUSTAIN1] = fmaxf(0.0f, fminf(1.0f, p->sustain1));
    g_synth.params[PARAM_RELEASE1] = fmaxf(0.0f, fminf(1.0f, p->release1));
    g_synth.params[PARAM_ATTACK2] = fmaxf(0.0f, fminf(1.0f, p->attack2));
    g_synth.params[PARAM_DECAY2] = fmaxf(0.0f, fminf(1.0f, p->decay2));
    g_synth.params[PARAM_SUSTAIN2] = fmaxf(0.0f, fminf(1.0f, p->sustain2));
    g_synth.params[PARAM_RELEASE2] = fmaxf(0.0f, fminf(1.0f, p->release2));
    g_synth.params[PARAM_CHORUS_MIX] = fmaxf(0.0f, fminf(1.0f, p->chorus_mix));
    g_synth.params[PARAM_DELAY_TIME] = fmaxf(0.0f, fminf(1.0f, p->delay_time));
    g_synth.params[PARAM_DELAY_FEEDBACK] = fmaxf(0.0f, fminf(1.0f, p->delay_feedback));
    g_synth.params[PARAM_DELAY_MIX] = fmaxf(0.0f, fminf(1.0f, p->delay_mix));

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

    /* Preset 0 specific audible defaults guarantee:
     * master_vol = 0.8f, osc_mix = 0.5f, cutoff = 0.75f, amp_sustain = 0.8f, amp_decay = 0.5f, filter_type = 0 (Lowpass) */
    if (index == 0) {
        g_synth.params[PARAM_MASTER_VOL] = 0.8f;
        g_synth.params[PARAM_PAN] = 0.5f;
        g_synth.params[PARAM_OSC_MIX] = 0.5f;
        g_synth.params[PARAM_CUTOFF] = 0.75f;
        g_synth.params[PARAM_SUSTAIN2] = 0.8f;
        g_synth.params[PARAM_DECAY2] = 0.5f;
        g_synth.params[PARAM_FILTER_TYPE] = 0.0f; /* Lowpass */
    }

    /* Copy loaded preset parameters into both timbre states */
    memcpy(g_synth.timbre_params[0], g_synth.params, sizeof(g_synth.params));
    memcpy(g_synth.timbre_params[1], g_synth.params, sizeof(g_synth.params));
    g_synth.timbre_edit = 0;
    g_synth.timbre_balance = 0.5f;

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
        synth->current_preset = g_synth.current_preset;
        synth->bank_side = g_synth.bank_side;
        synth->genre_category = g_synth.genre_category;
        synth->program_num = g_synth.program_num;
        synth->timbre_edit = g_synth.timbre_edit;
        synth->timbre_balance = g_synth.timbre_balance;
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

    for (int v = 0; v < NUM_VOICES; v++) {
        synth->voices[v].timbre_index = 0;
        synth->voices[v].layer_partner = -1;
    }

    /* Initialize to Default Preset 0 (A.11 Saw Lead) */
    load_preset(0);

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
    }

    /* Initialize LFOs */
    synth->lfo1.sh_value = 0.0f;
    synth->lfo2.sh_value = 0.0f;
}

void synth_set_param(synth_engine_t *synth, const char *key, float val) {
    if (!synth) synth = &g_synth;

    if (strcmp(key, "voice_mode") == 0) {
        synth->voice_mode = (val >= 0.5f) ? 1 : 0;
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "timbre_edit") == 0) {
        int t = (val >= 0.5f) ? 1 : 0;
        synth->timbre_edit = t;
        for (int i = 0; i < PARAM_LFO1_RATE; i++) {
            synth->params[i] = synth->timbre_params[t][i];
        }
        return;
    }

    if (strcmp(key, "timbre_balance") == 0) {
        if (val < 0.0f) val = 0.0f;
        if (val > 1.0f) val = 1.0f;
        synth->timbre_balance = val;
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
            synth->params[i] = val;
            if (i < PARAM_LFO1_RATE) {
                if (synth->voice_mode == 1) {
                    /* Layer mode: edit the selected timbre */
                    synth->timbre_params[synth->timbre_edit][i] = val;
                } else {
                    /* Single mode: keep both timbres in sync */
                    synth->timbre_params[0][i] = val;
                    synth->timbre_params[1][i] = val;
                }
            } else {
                /* Global FX/Master params: keep both timbres in sync */
                synth->timbre_params[0][i] = val;
                synth->timbre_params[1][i] = val;
            }
            return;
        }
    }
}

float synth_get_param(const synth_engine_t *synth, const char *key) {
    if (!synth) synth = &g_synth;
    if (strcmp(key, "voice_mode") == 0) {
        return (float)synth->voice_mode;
    }
    if (strcmp(key, "timbre_edit") == 0) {
        return (float)synth->timbre_edit;
    }
    if (strcmp(key, "timbre_balance") == 0) {
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
            if (synth->voice_mode == 1 && i < PARAM_LFO1_RATE) {
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
    if (t1_atk1_p < 0.001f) t1_atk1_p = 0.01f;
    if (t1_atk2_p < 0.001f) t1_atk2_p = 0.01f;
    float t1_atk1_coef = attack_time_to_coeff(t1_atk1_p, 0.001f, 8.0f, fs);
    float t1_atk2_coef = attack_time_to_coeff(t1_atk2_p, 0.001f, 8.0f, fs);

    float t2_atk1_p = synth->timbre_params[1][PARAM_ATTACK1];
    float t2_atk2_p = synth->timbre_params[1][PARAM_ATTACK2];
    if (t2_atk1_p < 0.001f) t2_atk1_p = 0.01f;
    if (t2_atk2_p < 0.001f) t2_atk2_p = 0.01f;
    float t2_atk1_coef = attack_time_to_coeff(t2_atk1_p, 0.001f, 8.0f, fs);
    float t2_atk2_coef = attack_time_to_coeff(t2_atk2_p, 0.001f, 8.0f, fs);

    if (synth->voice_mode == 0) {
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
        v->layer_partner = -1;

        if (!was_active || portamento < 0.005f) {
            v->current_pitch = target_pitch;
        }
        v->target_pitch = target_pitch;

        /* Clean reset of filter states on note-on to prevent feedback clicks */
        v->filter_svf[0].s1 = 0.0f;
        v->filter_svf[0].s2 = 0.0f;
        v->filter_svf[1].s1 = 0.0f;
        v->filter_svf[1].s2 = 0.0f;

        if (!was_active) {
            v->osc1_phase = 0.0f;
            v->osc2_phase = 0.0f;
            v->sub_phase = 0.0f;
            v->amp_env.value = 0.0f;
            v->filter_env.value = 0.0f;
        }

        adsr_gate_on(&v->filter_env, t1_atk1_coef);
        adsr_gate_on(&v->amp_env, t1_atk2_coef);

    } else {
        /* =========================================================
         * LAYER MODE (2-Voice Polyphony with 2 Linked Voice Instances each)
         * Slot 0: Voices 0 (Timbre 1) & 1 (Timbre 2)
         * Slot 1: Voices 2 (Timbre 1) & 3 (Timbre 2)
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
            bool slot0_idle = (!synth->voices[0].active || synth->voices[0].amp_env.stage == ENV_IDLE) &&
                              (!synth->voices[1].active || synth->voices[1].amp_env.stage == ENV_IDLE);
            bool slot1_idle = (!synth->voices[2].active || synth->voices[2].amp_env.stage == ENV_IDLE) &&
                              (!synth->voices[3].active || synth->voices[3].amp_env.stage == ENV_IDLE);
            if (slot0_idle) {
                slot = 0;
            } else if (slot1_idle) {
                slot = 1;
            }
        }

        /* 3. Slot Stealing:
         * - First priority: steal released slot (gate == false)
         * - Second priority: steal oldest slot
         */
        if (slot < 0) {
            bool slot0_released = (!synth->voices[0].gate) && (!synth->voices[1].gate);
            bool slot1_released = (!synth->voices[2].gate) && (!synth->voices[3].gate);

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

        /* Configure Timbre 1 */
        v1->active = true;
        v1->gate = true;
        v1->note = note;
        v1->velocity = vel01;
        v1->age = synth->voice_counter;
        v1->timbre_index = 0;
        v1->layer_partner = i2;

        if (!was_active1 || portamento1 < 0.005f) {
            v1->current_pitch = target_pitch;
        }
        v1->target_pitch = target_pitch;

        v1->filter_svf[0].s1 = 0.0f;
        v1->filter_svf[0].s2 = 0.0f;
        v1->filter_svf[1].s1 = 0.0f;
        v1->filter_svf[1].s2 = 0.0f;

        if (!was_active1) {
            v1->osc1_phase = 0.0f;
            v1->osc2_phase = 0.0f;
            v1->sub_phase = 0.0f;
            v1->amp_env.value = 0.0f;
            v1->filter_env.value = 0.0f;
        }

        /* Configure Timbre 2 */
        v2->active = true;
        v2->gate = true;
        v2->note = note;
        v2->velocity = vel01;
        v2->age = synth->voice_counter;
        v2->timbre_index = 1;
        v2->layer_partner = i1;

        if (!was_active2 || portamento2 < 0.005f) {
            v2->current_pitch = target_pitch;
        }
        v2->target_pitch = target_pitch;

        v2->filter_svf[0].s1 = 0.0f;
        v2->filter_svf[0].s2 = 0.0f;
        v2->filter_svf[1].s1 = 0.0f;
        v2->filter_svf[1].s2 = 0.0f;

        if (!was_active2) {
            /* 90-degree initial phase offset prevents comb filtering cancellation */
            v2->osc1_phase = 0.25f;
            v2->osc2_phase = 0.25f;
            v2->sub_phase = 0.25f;
            v2->amp_env.value = 0.0f;
            v2->filter_env.value = 0.0f;
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
        if (synth->voices[i].active && synth->voices[i].note == note && synth->voices[i].gate) {
            synth->voices[i].gate = false;
            adsr_gate_off(&synth->voices[i].filter_env);
            adsr_gate_off(&synth->voices[i].amp_env);
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

    /* Sanity fallback checks to guarantee audible defaults */
    if (master_vol_p <= 0.01f) master_vol_p = 0.8f;
    if (pan_p <= 0.001f && pan_p >= -0.001f && synth->params[PARAM_PAN] <= 0.001f) pan_p = 0.5f;

    /* Equal-power crossfade between Timbre 1 and Timbre 2 in Layer mode */
    float bal = synth->timbre_balance;
    if (bal < 0.0f) bal = 0.0f;
    if (bal > 1.0f) bal = 1.0f;
    float t1_crossfade = cosf(bal * (float)(M_PI * 0.5));
    float t2_crossfade = sinf(bal * (float)(M_PI * 0.5));

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
        const float *tp = (synth->voice_mode == 1) ? synth->timbre_params[t] : synth->params;

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

        if (cutoff_p < 0.15f && tp[PARAM_CUTOFF] < 0.15f) cutoff_p = 0.75f;
        if (sustain2_p < 0.05f && tp[PARAM_SUSTAIN2] < 0.05f) sustain2_p = 0.8f;
        if (decay2_p < 0.05f && tp[PARAM_DECAY2] < 0.05f) decay2_p = 0.5f;

        t_cfg[t].osc1_wave = (int)(wave1_p * 3.99f);
        t_cfg[t].pw = pw_p;
        t_cfg[t].osc2_wave = (int)(wave2_p * 2.99f);
        t_cfg[t].detune_semi = (detune_p - 0.5f) * 48.0f;
        t_cfg[t].sync_ring_mode = (int)(sync_ring_p * 3.99f);
        t_cfg[t].osc_mix = osc_mix_p;
        t_cfg[t].sub_level = sub_level_p;

        t_cfg[t].glide_coeff = 1.0f;
        if (portamento_p > 0.005f) {
            float glide_time = 0.005f * powf(400.0f, portamento_p);
            t_cfg[t].glide_coeff = 1.0f - expf(-1.0f / (glide_time * fs));
        }

        t_cfg[t].base_fc = 20.0f * powf(900.0f, cutoff_p);
        t_cfg[t].resonance = resonance_p;
        t_cfg[t].filter_type = (filter_type_t)(int)(filter_type_p * 3.99f);
        t_cfg[t].keytrack = keytrack_p;
        t_cfg[t].env_int = env_int_p;
        t_cfg[t].drive = drive_p;
        t_cfg[t].mod_int = mod_int_p;
        t_cfg[t].vel_sens = vel_sens_p;

        t_cfg[t].dcy1_coef = time_to_coeff(decay1_p, 0.001f, 10.0f, fs);
        t_cfg[t].sustain1 = sustain1_p;
        t_cfg[t].rel1_coef = time_to_coeff(release1_p, 0.001f, 10.0f, fs);
        t_cfg[t].dcy2_coef = time_to_coeff(decay2_p, 0.001f, 10.0f, fs);
        t_cfg[t].sustain2 = sustain2_p;
        t_cfg[t].rel2_coef = time_to_coeff(release2_p, 0.001f, 10.0f, fs);
    }

    /* LFO Frequencies: 0.05 Hz to 30 Hz */
    float lfo1_freq = 0.05f * powf(600.0f, lfo1_rate_p);
    float lfo2_freq = 0.05f * powf(600.0f, lfo2_rate_p);
    float lfo1_dt = lfo1_freq / fs;
    float lfo2_dt = lfo2_freq / fs;

    /* Delay buffer config */
    float target_delay_samples = 100.0f + delay_time_p * (float)(DELAY_BUFFER_SIZE - 200);
    float delay_feedback = delay_fdbk_p * 0.92f;

    /* Pan: Left / Right gains */
    float pan_l = cosf(pan_p * (float)(M_PI * 0.5));
    float pan_r = sinf(pan_p * (float)(M_PI * 0.5));
    if (pan_l <= 0.01f) pan_l = 0.7071f;
    if (pan_r <= 0.01f) pan_r = 0.7071f;

    for (int s = 0; s < frames; s++) {
        /* 1. Update LFO 1 */
        synth->lfo1.phase += lfo1_dt;
        if (synth->lfo1.phase >= 1.0f) {
            synth->lfo1.phase -= 1.0f;
            /* Sample & Hold random step */
            synth->lfo1.sh_value = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }
        /* Triangle LFO1 */
        float lfo1_val = 2.0f * fabsf(2.0f * synth->lfo1.phase - 1.0f) - 1.0f;

        /* 2. Update LFO 2 */
        synth->lfo2.phase += lfo2_dt;
        if (synth->lfo2.phase >= 1.0f) {
            synth->lfo2.phase -= 1.0f;
            synth->lfo2.sh_value = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        }
        /* Sine/Triangle LFO2 */
        float lfo2_val = sinf(2.0f * (float)M_PI * synth->lfo2.phase);

        /* 3. Render 4 Synth Voices */
        float voice_sum_l = 0.0f;
        float voice_sum_r = 0.0f;

        for (int v_idx = 0; v_idx < NUM_VOICES; v_idx++) {
            voice_t *v = &synth->voices[v_idx];
            if (!v->active) continue;

            int t_idx = (synth->voice_mode == 1) ? v->timbre_index : 0;
            const timbre_render_cfg_t *cfg = &t_cfg[t_idx];

            /* Portamento Pitch Glide */
            v->current_pitch += (v->target_pitch - v->current_pitch) * cfg->glide_coeff;

            /* LFO1 pitch mod (vibrato) + Pitch Bend + Timbre 2 Detune in Layer Mode */
            float pitch_mod = lfo1_val * (cfg->mod_int * 0.5f) + synth->pitch_bend_semi;
            float timbre_detune = (synth->voice_mode == 1 && v->timbre_index == 1) ? 0.05f : 0.0f;

            float final_note1 = v->current_pitch + pitch_mod + timbre_detune;
            float freq1 = note_to_freq(final_note1);
            float dt1 = freq1 / fs;
            if (dt1 > 0.45f) dt1 = 0.45f;

            float final_note2 = v->current_pitch + cfg->detune_semi + pitch_mod + timbre_detune;
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
                /* Reset Osc 2 phase with fractional alignment */
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
                    /* Pulse width modulated slightly by LFO1 */
                    float pw = cfg->pw + lfo1_val * 0.15f;
                    if (pw < 0.05f) pw = 0.05f;
                    if (pw > 0.95f) pw = 0.95f;
                    float raw = (v->osc1_phase < pw) ? 1.0f : -1.0f;
                    osc1_out = raw + poly_blep(v->osc1_phase, dt1) - poly_blep(fmod_pos(v->osc1_phase - pw), dt1);
                    break;
                }
                case OSC1_WAVE_TRIANGLE:
                    osc1_out = 2.0f * fabsf(2.0f * v->osc1_phase - 1.0f) - 1.0f;
                    break;
                case OSC1_WAVE_SINE:
                    osc1_out = sinf(2.0f * (float)M_PI * v->osc1_phase);
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

            /* Mixer */
            float osc_sum = (1.0f - cfg->osc_mix) * osc1_out + cfg->osc_mix * osc2_final + cfg->sub_level * sub_out;

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

            /* Sanity: if gate is held down, ensure amp envelope does not die to silence */
            if (v->gate && a_env < 0.001f && v->amp_env.stage != ENV_ATTACK) {
                a_env = (cfg->sustain2 > 0.05f) ? cfg->sustain2 : 0.8f;
            }

            /* --- Filter Processing --- */
            /* Key tracking relative to Middle C (note 60) */
            float keytrack_mod = (v->note - 60.0f) * (cfg->keytrack * (1.0f / 12.0f));

            /* Filter envelope intensity with velocity sensitivity */
            float effective_env_int = (cfg->env_int * 2.0f - 1.0f) * (1.0f - cfg->vel_sens + cfg->vel_sens * v->velocity);
            float env_mod = effective_env_int * f_env * 4.0f; /* +/- 4 octaves */

            /* LFO 2 modulation */
            float lfo2_mod = cfg->mod_int * lfo2_val * 2.0f;

            /* Timbre 2 filter cutoff offset in Layer Mode */
            float timbre_filter_mod = (synth->voice_mode == 1 && v->timbre_index == 1) ? 0.25f : 0.0f;

            float octaves = keytrack_mod + env_mod + lfo2_mod + timbre_filter_mod;
            float fc = cfg->base_fc * powf(2.0f, octaves);

            /* SVF Multimode Filter with feedback tanh saturation & bass preservation */
            float filtered = 0.0f;
            if (cfg->filter_type == FILTER_LP_24) {
                /* 4-pole: cascade of two 2-pole SVF stages */
                float stage1 = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance * 0.7f, cfg->drive, FILTER_LP_12, fs);
                filtered = svf_process_2pole(&v->filter_svf[1], stage1, fc, cfg->resonance * 0.7f, cfg->drive, FILTER_LP_12, fs);
            } else {
                /* 2-pole multimode */
                filtered = svf_process_2pole(&v->filter_svf[0], osc_sum, fc, cfg->resonance, cfg->drive, cfg->filter_type, fs);
            }

            /* Sanity tone: if filter output is silent or NaN, fallback to oscillator signal */
            if (isnan(filtered) || isinf(filtered) || (fabsf(filtered) < 1e-6f && fabsf(osc_sum) > 0.01f)) {
                filtered = osc_sum * 0.5f;
            }

            /* Velocity guarantee: never multiply by 0.0 */
            float voice_vel = v->velocity;
            if (voice_vel <= 0.01f) voice_vel = 0.8f;

            /* Amp Envelope & Velocity Scaling */
            float voice_audio = filtered * a_env * voice_vel;
            if (isnan(voice_audio) || isinf(voice_audio)) {
                voice_audio = 0.0f;
            } else if (fabsf(voice_audio) < 1e-7f && v->gate && fabsf(osc_sum) > 0.01f) {
                voice_audio = osc_sum * 0.4f;
            }

            /* Stereo pan spread & gain scaling in Layer Mode with Timbre Balance crossfade */
            float v_gain_l = 1.0f;
            float v_gain_r = 1.0f;
            if (synth->voice_mode == 1) {
                if (v->timbre_index == 0) {
                    /* Timbre 1: slight left tilt, scaled by t1_crossfade */
                    v_gain_l = 1.15f * t1_crossfade;
                    v_gain_r = 0.85f * t1_crossfade;
                } else {
                    /* Timbre 2: slight right tilt, scaled by t2_crossfade */
                    v_gain_l = 0.85f * t2_crossfade;
                    v_gain_r = 1.15f * t2_crossfade;
                }
            }

            voice_sum_l += voice_audio * v_gain_l;
            voice_sum_r += voice_audio * v_gain_r;
        }

        /* 4. Stereo Chorus / Ensemble */
        float chorus_l = voice_sum_l;
        float chorus_r = voice_sum_r;

        if (chorus_mix_p > 0.01f) {
            synth->chorus_lfo_phase += 0.8f / fs; /* ~0.8 Hz chorus rate */
            if (synth->chorus_lfo_phase >= 1.0f) synth->chorus_lfo_phase -= 1.0f;

            /* Quadrature LFOs for stereo spread */
            float lfo_c1 = sinf(2.0f * (float)M_PI * synth->chorus_lfo_phase);
            float lfo_c2 = cosf(2.0f * (float)M_PI * synth->chorus_lfo_phase);

            float delay_mod_l = 441.0f + lfo_c1 * 220.0f; /* 5ms to 15ms */
            float delay_mod_r = 441.0f + lfo_c2 * 220.0f;

            /* Write to circular chorus buffer */
            uint32_t wpos = synth->chorus_write_pos;
            synth->chorus_buf_l[wpos] = voice_sum_l;
            synth->chorus_buf_r[wpos] = voice_sum_r;
            synth->chorus_write_pos = (wpos + 1) % CHORUS_BUFFER_SIZE;

            /* Read interpolated delay taps */
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

            chorus_l = voice_sum_l * (1.0f - chorus_mix_p * 0.5f) + tap_l * chorus_mix_p;
            chorus_r = voice_sum_r * (1.0f - chorus_mix_p * 0.5f) + tap_r * chorus_mix_p;
        }

        /* 5. Digital Delay with Feedback */
        float out_l = chorus_l;
        float out_r = chorus_r;

        if (delay_mix_p > 0.01f) {
            uint32_t dwpos = synth->delay_write_pos;

            /* Read delay taps (stereo ping-pong offset) */
            float drpos_l = (float)dwpos - target_delay_samples;
            while (drpos_l < 0.0f) drpos_l += (float)DELAY_BUFFER_SIZE;
            int didx_l0 = (int)drpos_l % DELAY_BUFFER_SIZE;
            int didx_l1 = (didx_l0 + 1) % DELAY_BUFFER_SIZE;
            float dfrac_l = drpos_l - (int)drpos_l;
            float dtap_l = synth->delay_buf_l[didx_l0] * (1.0f - dfrac_l) + synth->delay_buf_l[didx_l1] * dfrac_l;

            float drpos_r = (float)dwpos - (target_delay_samples * 0.75f); /* Rhythmic ping-pong offset */
            while (drpos_r < 0.0f) drpos_r += (float)DELAY_BUFFER_SIZE;
            int didx_r0 = (int)drpos_r % DELAY_BUFFER_SIZE;
            int didx_r1 = (didx_r0 + 1) % DELAY_BUFFER_SIZE;
            float dfrac_r = drpos_r - (int)drpos_r;
            float dtap_r = synth->delay_buf_r[didx_r0] * (1.0f - dfrac_r) + synth->delay_buf_r[didx_r1] * dfrac_r;

            /* Feedback lowpass filter damping (~4 kHz) */
            synth->delay_filter_l += 0.35f * (dtap_l - synth->delay_filter_l);
            synth->delay_filter_r += 0.35f * (dtap_r - synth->delay_filter_r);

            /* Denormal flushing and NaN/Inf sanitization for delay filter states */
            if (isnan(synth->delay_filter_l) || isinf(synth->delay_filter_l)) synth->delay_filter_l = 0.0f;
            if (isnan(synth->delay_filter_r) || isinf(synth->delay_filter_r)) synth->delay_filter_r = 0.0f;
            if (fabsf(synth->delay_filter_l) < 1e-15f) synth->delay_filter_l = 0.0f;
            if (fabsf(synth->delay_filter_r) < 1e-15f) synth->delay_filter_r = 0.0f;

            /* Write to delay line with soft-clipped feedback */
            synth->delay_buf_l[dwpos] = chorus_l + fast_tanh(synth->delay_filter_l * delay_feedback);
            synth->delay_buf_r[dwpos] = chorus_r + fast_tanh(synth->delay_filter_r * delay_feedback);
            synth->delay_write_pos = (dwpos + 1) % DELAY_BUFFER_SIZE;

            out_l = chorus_l * (1.0f - delay_mix_p * 0.5f) + dtap_l * delay_mix_p;
            out_r = chorus_r * (1.0f - delay_mix_p * 0.5f) + dtap_r * delay_mix_p;
        }

        /* 6. Master Volume, Pan, Soft Limiter & Convert to int16 */
        if (pan_l <= 0.01f) pan_l = 0.7071f;
        if (pan_r <= 0.01f) pan_r = 0.7071f;
        if (master_vol_p <= 0.01f) master_vol_p = 0.8f;

        out_l = fast_tanh(out_l * master_vol_p * pan_l);
        out_r = fast_tanh(out_r * master_vol_p * pan_r);

        int32_t sample_l = (int32_t)(out_l * 32767.0f);
        int32_t sample_r = (int32_t)(out_r * 32767.0f);

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
    (void)json_defaults;
    synth_init(&g_synth);
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

    if (strcmp(key, "voice_mode") == 0) {
        int mode = 0;
        if (strstr(val, "Layer") || strstr(val, "layer") || strcmp(val, "1") == 0 || strcmp(val, "1.0") == 0) {
            mode = 1;
        } else {
            float f = (float)atof(val);
            mode = (f >= 0.5f) ? 1 : 0;
        }
        synth->voice_mode = mode;
        synth_all_notes_off(synth);
        return;
    }

    if (strcmp(key, "timbre_edit") == 0) {
        int t = 0;
        if (strstr(val, "2") || strcmp(val, "1") == 0 || strcmp(val, "1.0") == 0) {
            t = 1;
        } else {
            float f = (float)atof(val);
            t = (f >= 0.5f) ? 1 : 0;
        }
        synth->timbre_edit = t;
        for (int i = 0; i < PARAM_LFO1_RATE; i++) {
            synth->params[i] = synth->timbre_params[t][i];
        }
        return;
    }

    if (strcmp(key, "timbre_balance") == 0) {
        float f = (float)atof(val);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        synth->timbre_balance = f;
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

    if (strcmp(key, "voice_mode") == 0) {
        return snprintf(buf, buf_len, "%d", synth->voice_mode);
    }

    if (strcmp(key, "timbre_edit") == 0) {
        return snprintf(buf, buf_len, "%d", synth->timbre_edit);
    }

    if (strcmp(key, "timbre_balance") == 0) {
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
            if (synth->voice_mode == 1 && i < PARAM_LFO1_RATE) {
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
