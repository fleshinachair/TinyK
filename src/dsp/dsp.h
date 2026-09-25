#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE          44100
#define MOVE_FRAMES_PER_BLOCK     128
#define NUM_VOICES                4
#define NUM_PARAMS                32
#define NUM_PRESETS               128

/* Max buffer sizes for static allocation */
#define DELAY_BUFFER_SIZE         44100  /* 1.0 second @ 44.1 kHz */
#define CHORUS_BUFFER_SIZE        2048   /* ~46 ms @ 44.1 kHz */

/* Parameter Indices */
typedef enum {
    /* Page 1: OSC */
    PARAM_WAVE1 = 0,
    PARAM_PULSE_WIDTH,
    PARAM_WAVE2,
    PARAM_DETUNE,
    PARAM_SYNC_RING,
    PARAM_OSC_MIX,
    PARAM_SUB_LEVEL,
    PARAM_PORTAMENTO,

    /* Page 2: FILTER */
    PARAM_CUTOFF,
    PARAM_RESONANCE,
    PARAM_FILTER_TYPE,
    PARAM_KEYTRACK,
    PARAM_ENV_INT,
    PARAM_DRIVE,
    PARAM_MOD_INT,
    PARAM_VEL_SENS,

    /* Page 3: ENVs */
    PARAM_ATTACK1,
    PARAM_DECAY1,
    PARAM_SUSTAIN1,
    PARAM_RELEASE1,
    PARAM_ATTACK2,
    PARAM_DECAY2,
    PARAM_SUSTAIN2,
    PARAM_RELEASE2,

    /* Page 4: FX/MOD */
    PARAM_LFO1_RATE,
    PARAM_LFO2_RATE,
    PARAM_CHORUS_MIX,
    PARAM_DELAY_TIME,
    PARAM_DELAY_FEEDBACK,
    PARAM_DELAY_MIX,
    PARAM_MASTER_VOL,
    PARAM_PAN,

    PARAM_COUNT
} param_id_t;

/* Oscillator 1 Waveforms */
typedef enum {
    OSC1_WAVE_SAW = 0,
    OSC1_WAVE_SQUARE,
    OSC1_WAVE_TRIANGLE,
    OSC1_WAVE_SINE,
    OSC1_WAVE_COUNT
} osc1_wave_t;

/* Oscillator 2 Waveforms */
typedef enum {
    OSC2_WAVE_SAW = 0,
    OSC2_WAVE_SQUARE,
    OSC2_WAVE_TRIANGLE,
    OSC2_WAVE_COUNT
} osc2_wave_t;

/* Sync / Ring Modes */
typedef enum {
    SYNC_RING_OFF = 0,
    SYNC_RING_SYNC,
    SYNC_RING_RING,
    SYNC_RING_BOTH,
    SYNC_RING_COUNT
} sync_ring_mode_t;

/* Filter Types */
typedef enum {
    FILTER_LP_24 = 0, /* 4-pole 24dB Lowpass */
    FILTER_LP_12,     /* 2-pole 12dB Lowpass */
    FILTER_BP_12,     /* 2-pole 12dB Bandpass */
    FILTER_HP_12,     /* 2-pole 12dB Highpass */
    FILTER_TYPE_COUNT
} filter_type_t;

/* ADSR Envelope Stages */
typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_DECAY,
    ENV_SUSTAIN,
    ENV_RELEASE
} env_stage_t;

/* ADSR State */
typedef struct {
    env_stage_t stage;
    float value;
    float target;
    float rate;
} adsr_t;

/* LFO State */
typedef struct {
    float phase;
    float sh_value;
    float prev_phase;
} lfo_t;

/* State-Variable Filter (SVF) State */
typedef struct {
    float s1;
    float s2;
} svf_t;

/* Voice State */
typedef struct {
    bool active;
    bool gate;
    uint8_t note;
    float velocity;
    uint32_t age;            /* Note-on timestamp for voice stealing */

    /* Pitch & Portamento */
    float current_pitch;    /* Fractional semitones */
    float target_pitch;

    /* Oscillators */
    float osc1_phase;
    float osc2_phase;
    float sub_phase;

    /* Dual-Timbre Layer Properties */
    int timbre_index;       /* 0 = Timbre 1, 1 = Timbre 2 */
    int layer_partner;      /* Index of linked voice or -1 */

    /* Envelopes */
    adsr_t filter_env;
    adsr_t amp_env;

    /* Filter 2-pole SVF states (2 stages for up to 4-pole / 24dB) */
    svf_t filter_svf[2];
} voice_t;

/* Preset Definition */
typedef struct {
    const char *name;
    const char *category;
    float params[NUM_PARAMS];
} preset_t;

/* Global Engine State */
typedef struct {
    /* Parameter values: strictly 0.0f to 1.0f */
    float params[NUM_PARAMS];

    /* Voices */
    voice_t voices[NUM_VOICES];
    uint32_t voice_counter;

    /* LFOs */
    lfo_t lfo1;
    lfo_t lfo2;

    /* Effects buffers (statically allocated) */
    float delay_buf_l[DELAY_BUFFER_SIZE];
    float delay_buf_r[DELAY_BUFFER_SIZE];
    uint32_t delay_write_pos;
    float delay_filter_l;
    float delay_filter_r;

    float chorus_buf_l[CHORUS_BUFFER_SIZE];
    float chorus_buf_r[CHORUS_BUFFER_SIZE];
    uint32_t chorus_write_pos;
    float chorus_lfo_phase;

    /* Preset State */
    int current_preset;
    int bank_side;      /* 0 = Side A, 1 = Side B */
    int genre_category; /* 0..7 */
    int program_num;    /* 1..8 */

    /* Voice Mode: 0 = Single (4-Voice), 1 = Layer (2-Voice) */
    int voice_mode;

    /* Timbre editing & balance */
    int timbre_edit;          /* 0 = Timbre 1, 1 = Timbre 2 */
    float timbre_balance;     /* 0.0f (100% Timbre 1) to 1.0f (100% Timbre 2), default 0.5f */
    float timbre_params[2][NUM_PARAMS]; /* Independent timbre parameter states */

    /* Octave Transpose & Pitch Bend */
    int octave_transpose;
    float pitch_bend_semi;
} synth_engine_t;

/* Move Plugin Host API v1 */
typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
} host_api_v1_t;

/* Move Plugin API v2 (sound_generator) */
typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

/* Move Audio FX Plugin API v2 (audio_fx) */
typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, const int16_t *in_interleaved_lr, int16_t *out_interleaved_lr, int frames);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
} audio_fx_api_v2_t;

/* Plugin Entry Points */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);

/* Direct MIDI and Process Entry Points */
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source);
void move_plugin_on_midi(void *instance, const uint8_t *msg, int len, int source);
void move_audio_fx_process(void *instance, int16_t *out_interleaved_lr, int frames);

/* Engine functions */
void synth_init(synth_engine_t *synth);
void synth_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity);
void synth_note_off(synth_engine_t *synth, uint8_t note);
void synth_all_notes_off(synth_engine_t *synth);
void synth_set_param(synth_engine_t *synth, const char *key, float val);
float synth_get_param(const synth_engine_t *synth, const char *key);
void synth_load_preset(synth_engine_t *synth, int preset_idx);
void load_preset(int index);
void synth_render(synth_engine_t *synth, int16_t *out_lr, int frames);

#ifdef __cplusplus
}
#endif

#endif /* DSP_H */
