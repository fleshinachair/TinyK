#ifndef DSP_H
#define DSP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "arp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE          44100
#define MOVE_FRAMES_PER_BLOCK     128
#define NUM_VOICES                4
#define NUM_PARAMS                35
#define NUM_PRESETS               128

/* Max buffer sizes for static allocation */
#define DELAY_BUFFER_SIZE         88200  /* 2.0 seconds @ 44.1 kHz: tempo-synced delays up to 1/1 at 120 BPM */
#define DELAY_FREE_MAX_SAMPLES    44100  /* the free (unsynced) delay time range: up to 1 s */
#define CHORUS_BUFFER_SIZE        2048   /* ~46 ms @ 44.1 kHz */
#define WAVETABLE_SIZE            1024   /* Single-cycle table for Vox / DWGS oscillators */

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

    /* Global / Voice Mode & Timbre controls */
    PARAM_VOICE_MODE,
    PARAM_TIMBRE_EDIT,
    PARAM_TIMBRE_BALANCE,

    PARAM_COUNT
} param_id_t;

/* Oscillator 1 Waveforms (normalized wave1 param = index / (OSC1_WAVE_COUNT - 1)) */
typedef enum {
    OSC1_WAVE_SAW = 0,
    OSC1_WAVE_SQUARE,
    OSC1_WAVE_TRIANGLE,
    OSC1_WAVE_SINE,
    OSC1_WAVE_VOX,    /* Formant-style wavetable */
    OSC1_WAVE_DWGS,   /* Digital waveform wavetable, selected by timbre_extra.dwgs */
    OSC1_WAVE_NOISE,
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
    float lim;        /* patch LFOs: the slew-limited value (see LFO_SLEW_S) */
    float out;        /* ...and the output: that value through a short one-pole (LFO_SMOOTH_S) */
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
    int is_timbre_2;        /* 0 for Voice A (Timbre 1), 1 for Voice B (Timbre 2) */

    /* Envelopes */
    adsr_t filter_env;
    adsr_t amp_env;

    /* Filter 2-pole SVF states (2 stages for up to 4-pole / 24dB) */
    svf_t filter_svf[2];

    /* Unison: this voice's detune (cents) and stereo place (-1..+1) in the stack, and its share of the level */
    float unison_cents;
    float unison_pan;
    float unison_gain;

    /* De-click: a voice starting from silence fades in over DECLICK_SAMPLES (raised cosine, declick_pos counts up);
     * vel_gain is the velocity as heard, gliding to velocity so a steal or retrigger never steps the level; kill is
     * an all-notes-off fade (a fast release) instead of a cut */
    int declick_pos;
    float vel_gain;
    bool kill;
} voice_t;

/* Per-timbre settings that come from the patch but have no PARAM_* slot */
typedef struct {
    float transpose_semi;   /* Timbre transpose + tune, in semitones */
    float noise_level;      /* 0..1 white noise in the mixer */
    float level;            /* 0..1 overall timbre level (osc levels x amp level) */
    float dwgs;             /* 0..1 -> DWGS waveform 0..63 */
    float osc1_ctrl[2];     /* Osc 1 Control 1 / 2 (raw / 127); for Sine: cross-mod depth / LFO1 modulation of it */
    int assign;             /* voice assign: 0 Mono, 1 Poly, 2 Unison */
    float unison_cents;     /* unison detune: the spread of the stacked voices, cents */
    float pan;              /* timbre pan, -1 left .. +1 right */
    int multi_trigger;      /* mono / unison: 1 = every key restarts the EGs, 0 = legato keys do not */

    /* Per-timbre LFOs (index 0 = LFO1, 1 = LFO2) and the 4-slot virtual patch matrix */
    int lfo_wave[2];        /* LFO1: saw, square, triangle, S&H; LFO2: saw, square, sine, S&H */
    int lfo_keysync[2];     /* 0 off, 1 timbre, 2 voice: restart the LFO on note-on */
    float lfo_rate[2];      /* 0..1 free-running rate knob */
    int lfo_sync_note[2];   /* -1 = free running, else index into the tempo-sync note table */
    int patch_src[4];       /* patch_source_t */
    int patch_dst[4];       /* patch_dest_t */
    float patch_int[4];     /* -1..+1 (hardware -63..+63); 0 = slot unused */
} timbre_extra_t;

/* Virtual patch sources and destinations (microKORG order) */
typedef enum { PATCH_SRC_EG1 = 0, PATCH_SRC_EG2, PATCH_SRC_LFO1, PATCH_SRC_LFO2, PATCH_SRC_VELOCITY,
               PATCH_SRC_KBD, PATCH_SRC_PITCH_BEND, PATCH_SRC_MOD_WHEEL, PATCH_SRC_COUNT } patch_source_t;
typedef enum { PATCH_DST_PITCH = 0, PATCH_DST_OSC2_PITCH, PATCH_DST_OSC1_CTRL1, PATCH_DST_NOISE,
               PATCH_DST_CUTOFF, PATCH_DST_AMP, PATCH_DST_PAN, PATCH_DST_LFO2_FREQ, PATCH_DST_COUNT } patch_dest_t;

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
    int delay_active;       /* 0 while the delay line is bypassed (depth 0) */

    /* DC blocker state (L/R) on the voice mix */
    float dc_x[2];
    float dc_y[2];
    float out_dc_x[2], out_dc_y[2]; /* a second DC blocker on the output, after the Mod FX and the delay */

    /* Brightness tilt (first-order high shelf) on the voice mix: previous input / output, L/R */
    float tilt_x1[2];
    float tilt_y1[2];
    float noise_x1, noise_y1;  /* inverse-tilt state of the audible noise */

    float chorus_buf_l[CHORUS_BUFFER_SIZE];
    float chorus_buf_r[CHORUS_BUFFER_SIZE];
    uint32_t chorus_write_pos;
    float chorus_lfo_phase;

    /* Mod FX (program bytes 23 speed, 24 depth = PARAM_CHORUS_MIX, 25 type): 0 Chorus/Flanger, 1 Ensemble, 2 Phaser */
    int modfx_type;
    float modfx_speed;          /* 0..1 */
    float phaser_ap[2][6];      /* phaser all-pass stage states, per channel */
    float phaser_fb[2];

    /* Program-change flush: sounding voices are killed with a fade (synth_all_notes_off), and while audio is still
     * sounding the output ducks to silence, the effect memories are cleared there and the output comes back. */
    float out_level;            /* decaying peak of the output, tells whether anything is audible */
    int flush_stage;            /* 0 idle, 1 ducking (clear at silence), 2 coming back */
    float flush_gain;

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
    timbre_extra_t timbre_extra[2];

    /* Cached Vox/DWGS wavetables, rebuilt when the selection changes */
    float wavetable[2][WAVETABLE_SIZE];
    int wavetable_key[2];

    /* Octave Transpose & Pitch Bend */
    int octave_transpose;
    float pitch_bend_semi;

    /* Patch-matrix LFOs, one pair per timbre ([timbre][0 = LFO1, 1 = LFO2]), and the controller patch
     * sources: pitch bend (-1..+1, centre 0) and the mod wheel (CC1, 0..+1) */
    lfo_t patch_lfo[2][2];
    float bend_src;
    float modwheel_src;

    /* Session tempo (host get_bpm, else lfo_tempo_bpm), read once per block, and the program's delay
     * tempo sync: -1 = free, else the time base index (DELAY_SYNC_NOTES) the Delay Time knob starts on */
    float tempo_bpm;
    int delay_sync_note;

    /* The program's arpeggiator (src/dsp/arp.c): MIDI keys go through it while it is on */
    arp_t arp;

    /* Mono / Unison timbres: their held keys, last on top (last-note priority) */
    uint8_t mono_keys[2][16];
    uint8_t mono_vel[2][16];
    int mono_count[2];
    uint32_t phase_rng;     /* oscillator start phases (seeded by synth_init: renders stay repeatable) */
    uint32_t noise_state;   /* white noise (xorshift32, seeded by synth_init) */

    /* Host side, per instance: each slot picks its own bank, and reports its own label changes (is_loading) */
    int bank_file;          /* 0 = built-in, 1..N = the .syx banks found at start-up */
    int labels_reported;    /* label_context() the host last read */
} synth_engine_t;

/* Move Plugin Host API v1: field for field Schwung's src/host/plugin_api_v1.h (the order is the ABI). Every
 * callback may be NULL on an older host, so each use is guarded. Never append a field here: a module cannot
 * extend this struct, and reading one the host lacks reads someone else's memory. */
#define MOVE_CLOCK_STATUS_UNAVAILABLE 0
#define MOVE_CLOCK_STATUS_STOPPED 1
#define MOVE_CLOCK_STATUS_RUNNING 2
typedef int (*move_mod_emit_value_fn)(void *ctx, const char *source_id, const char *target, const char *param,
                                      float signal, float depth, float offset, int bipolar, int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);
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
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);              /* session tempo: MIDI clock -> Set tempo -> settings -> 120 */
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    double (*get_beat_position)(void);   /* beats since transport start; < 0 when stopped */
    void *reserved[8];
} host_api_v1_t;

/* Schwung's public move_info.h (src/host/move_info.h): Move's own set settings, read live from its song document,
 * so `tempo` is the Set tempo as the tempo knob leaves it, playing or stopped (get_bpm is measured from MIDI
 * clock, which only runs while playing). The shim exports schwung_move_info(out, cap): a seqlocked copy out of
 * an already-mapped page, no syscalls, safe on the audio thread; it returns 1 on a consistent copy. Field for
 * field the header's move_info_t, checked by _Static_assert in dsp.c; extend it only as the header grows. */
#define MOVE_INFO_TRACKS 4
typedef struct {
    char    name[32];
    int16_t color_id;
    int8_t  type;
    uint8_t muted, soloed, selected;
    uint8_t _pad[2];
    float   volume_db;
} move_info_track_t;
typedef struct {
    uint32_t size;
    uint32_t version;
    uint32_t changes;
    uint8_t  valid;            /* 0: Move's document is not being read (set load, unknown firmware): all unknown */
    uint8_t  playing;
    uint8_t  metronome_on;
    uint8_t  midi_clock_sync;  /* Move follows external MIDI clock */
    uint8_t  input_monitoring;
    int8_t   root_note;
    int8_t   selected_track;
    int8_t   global_quant;
    uint8_t  ts_upper, ts_lower;
    uint8_t  _pad[2];
    float    tempo;            /* BPM; <= 0 unknown */
    float    groove;
    float    master_db;
    char     scale[24];
    char     global_quant_name[24];
    double   song_beats;
    move_info_track_t track[MOVE_INFO_TRACKS];
} move_info_t;
typedef int (*move_info_fn)(move_info_t *out, size_t cap);

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

/* Global transfer-function constants. They are plain compile-time constants in the shipped
 * build; tools/calibrate_dsp.py builds the engine with -DTINYK_TUNING so it can adjust them
 * at run time (see tools/test_render.c) and fit them against hardware recordings. */
typedef struct {
    float cutoff_base_hz;     /* cutoff = base * 2^(cutoff_param * cutoff_octaves) */
    float cutoff_octaves;
    float cutoff_floor_hz;
    float cutoff_ceil_hz;
    float bpf_k0;             /* band-pass damping k at resonance 0 (3.06 = Q 0.33, measured) */
    float env_octaves;        /* filter EG at full intensity, in octaves (multiplies the cutoff) */
    float res_damping_range;  /* SVF damping k = 2 - range * resonance */
    float lp24_res_scale;     /* resonance scale per stage of the 24 dB filter */
    float drive_gain;         /* distortion (post-filter tanh) gain added per unit of drive; drive 0.5 = on */
    float attack_scale;       /* multipliers on the EG time ranges (1.0 = 1.5 ms..12 s, 15 ms..20 s) */
    float decay_scale;
    float release_scale;
    float mixer_trim;         /* gain after the oscillators are summed */
    float delay_send_scale;   /* delay repeats added = depth * scale */
    float patch_cutoff_octaves; /* virtual patch -> cutoff at intensity 63, full source, in octaves (VST: >= ~10, set to the knob span) */
    float patch_pitch_scale;  /* virtual patch -> pitch / osc2 pitch at intensity 63, in semitones (VST: 24.1 measured) */
    float lfo_tempo_bpm;      /* tempo when the host gives none (renders, tests, hosts without get_bpm) */
    float patch_int_curve;    /* patch depth = full scale * (|int|/63)^curve (VST: 2.4 measured at +20 / +63) */
    float tilt_db;            /* high-shelf gain on the voice mix, dB (VST open saw vs ideal: 26.7) */
    float tilt_hz;            /* high-shelf corner, Hz (20000): ~+2 dB at 3 kHz, +7 at 8 kHz, +18 at 17 kHz */
    float patch_pan_curve;    /* patch -> pan depth = (|int|/63)^curve; 0.6 (not measured, chosen): LFO -> pan
                               * swings +-3.4 dB at +6, +-7.6 dB at +20, +-14 dB at +40, hard L/R at +63 */
    float bpf_cutoff_octaves; /* BPF12 centre = base * 2^(knob * bpf_cutoff_octaves + bpf_cutoff_offset) */
    float bpf_cutoff_offset;  /* octaves */
    float dist_ceiling;       /* distortion soft-clip level (output = ceiling * tanh(gain * x / ceiling)) */
    float noise_tilt_db;      /* audible noise is pre-shaped by the inverse of a shelf this high (tilt_hz corner), so it
                               * keeps tilt_db - noise_tilt_db of the tilt's top: 13.8 fits A.21's off-beat hat to the
                               * VST (ref_a21_timbre2_drum_c3: hat 8.7 dB under the kick; 26.7 = white left it 21 under) */
    float xmod_semitones;     /* synced Sine cross-mod: Osc 1 pitch offset per unit of depth (ctrl1 + ctrl2 * LFO1),
                               * semitones; 0 = off. Fitted on the A.21 T2 takes (sine_xmod_ratio in dsp.c) */
    float xmod_offset_semitones; /* ...and the offset at depth 0 */
    float hpf_ceil_hz;        /* HPF12's highest cutoff, knob plus modulation (fitted on A.21's hat) */
    float unison_spread;      /* unison: the outer voices' stereo place, 0 (centre) .. 1 (hard L / R) */
    float unison_cents_scale; /* unison detune byte -> cents of spread between the outer voices */
    float modfx_rate_lo_hz;   /* Mod FX LFO: rate = lo * span^speed (speed 0..1, program byte 23 / 127) */
    float modfx_rate_span;
    float chorus_center_ms;   /* Chorus/Flanger: delay centre, and its triangle sweep at full depth (+-), ms */
    float chorus_depth_ms;
    float chorus_wet;         /* wet copy's gain, added to the dry signal at unity */
} tinyk_tuning_t;

#ifdef TINYK_TUNING
extern tinyk_tuning_t tinyk_tuning;
#endif

/* Engine functions */
void synth_init(synth_engine_t *synth);
void synth_note_on(synth_engine_t *synth, uint8_t note, uint8_t velocity);
void synth_note_off(synth_engine_t *synth, uint8_t note);
void synth_all_notes_off(synth_engine_t *synth);
void synth_set_param(synth_engine_t *synth, const char *key, float val);
float synth_get_param(const synth_engine_t *synth, const char *key);
void synth_load_preset(synth_engine_t *synth, int preset_idx);
void synth_render(synth_engine_t *synth, int16_t *out_lr, int frames);

#ifdef __cplusplus
}
#endif

#endif /* DSP_H */
