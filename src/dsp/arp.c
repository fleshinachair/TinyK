/*
 * microKORG arpeggiator (included once by dsp.c, after presets.h: see arp.h).
 *
 * Program data (unpacked program bytes, checked against the factory banks):
 *   14 trigger length - 1 (0..7), 15 trigger pattern (bit n = step n + 1; SET = rest: 17 of the factory's 23 arp
 *   programs store 0 and play every step), 32 bit 7 on / bit 6 latch / bits 4-5 target / bit 0 key sync,
 *   33 bits 0-3 type / bits 4-7 range - 1, 34 gate 0..100 %, 35 resolution 0..5, 36 swing (signed %).
 *   30-31 hold the program's own arp tempo; TinyK follows the session tempo instead.
 *
 * Timing: steps sit on a grid of the resolution's note length; every second step is moved by swing / 3 of a step
 * (+100 % = a triplet shuffle). While the Move's transport runs and key sync is off, the grid is the transport's
 * beat position (steps land on the beat and the pattern on the bar, as with the hardware on external clock).
 * Otherwise it runs free at the session tempo from the first key. A step fires its note(s) unless the pattern rests
 * it; they last gate x the step (100 % = until the next step). The note order advances on played steps only.
 *
 * The real-time path allocates nothing: keys, the note order and the sounding notes live in arp_t.
 */
#include "arp.h"

typedef void (*arp_emit_fn)(void *ctx, int on, uint8_t note, uint8_t vel);

/* Step length per resolution, in beats: 1/24, 1/16, 1/12, 1/8, 1/6, 1/4 */
static const double ARP_STEP_BEATS[6] = { 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0 };

/* Shortest gate, as a fraction of the step, so gate 0 % still sounds */
#define ARP_MIN_GATE 0.02

static int arp_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void arp_settings_from_program(arp_settings_t *s, const struct ArpParams *p) {
    s->on = p->on >= 0.5f;
    s->latch = p->latch >= 0.5f;
    s->key_sync = p->key_sync >= 0.5f;
    s->target = arp_clampi((int)lroundf(p->target * 2.0f), 0, 2);
    s->type = arp_clampi((int)lroundf(p->type * 5.0f), 0, ARP_TYPE_COUNT - 1);
    s->range = 1 + arp_clampi((int)lroundf(p->range * 3.0f), 0, 3);
    s->resolution = arp_clampi((int)lroundf(p->resolution * 5.0f), 0, 5);
    s->length = 1 + arp_clampi((int)lroundf(p->length * 7.0f), 0, 7);
    s->pattern = (uint8_t)arp_clampi((int)lroundf(p->pattern * 255.0f), 0, 255);
    s->gate = p->gate < 0.0f ? 0.0f : (p->gate > 1.0f ? 1.0f : p->gate);
    s->swing = fmaxf(-1.0f, fminf(1.0f, (p->swing - 0.5f) * 2.0f));
}

/* Forgets keys and playback (not the settings). The caller releases any sounding notes first. */
static void arp_reset(arp_t *a) {
    a->held_count = a->down_count = 0;
    a->running = a->locked = 0;
    a->pos = 0.0;
    a->step_k = 0;
    a->next_step = 0.0;
    a->gate_end = -1.0;
    a->seq_pos = 0;
    a->sounding_count = 0;
    a->rng = 0x9E3779B9u;
}

static double arp_step_beats(const arp_t *a) { return ARP_STEP_BEATS[arp_clampi(a->set.resolution, 0, 5)]; }

/* Time of step k (k >= 0), beats: pairs of steps, the second moved by the swing */
static double arp_step_time(const arp_t *a, int64_t k) {
    double sb = arp_step_beats(a);
    double t = (double)(k / 2) * 2.0 * sb;
    return (k & 1) ? t + sb * (1.0 + (double)a->set.swing / 3.0) : t;
}

/* The first step at or after pos */
static int64_t arp_step_from(const arp_t *a, double pos) {
    if (pos <= 0.0) return 0;
    int64_t k = 2 * (int64_t)floor(pos / (2.0 * arp_step_beats(a)));
    while (arp_step_time(a, k) < pos - 1e-9) k++;
    return k;
}

static void arp_resync(arp_t *a) {
    a->step_k = arp_step_from(a, a->pos);
    a->next_step = arp_step_time(a, a->step_k);
}

static void arp_release(arp_t *a, arp_emit_fn emit, void *ctx) {
    for (int i = 0; i < a->sounding_count; i++) emit(ctx, 0, a->sounding[i], 0);
    a->sounding_count = 0;
    a->gate_end = -1.0;
}

static void arp_sound(arp_t *a, uint8_t note, uint8_t vel, arp_emit_fn emit, void *ctx) {
    if (a->sounding_count >= ARP_MAX_CHORD) return;
    a->sounding[a->sounding_count++] = note;
    emit(ctx, 1, note, vel);
}

/* The held keys by ascending pitch; returns how many */
static int arp_sorted_keys(const arp_t *a, uint8_t *sn, uint8_t *sv) {
    int n = a->held_count;
    for (int i = 0; i < n; i++) {
        int j = i;
        while (j > 0 && sn[j - 1] > a->held_note[i]) { sn[j] = sn[j - 1]; sv[j] = sv[j - 1]; j--; }
        sn[j] = a->held_note[i];
        sv[j] = a->held_vel[i];
    }
    return n;
}

/* The note order for the held keys over the octave range (not TRIGGER): returns its length */
static int arp_sequence(const arp_t *a, uint8_t *notes, uint8_t *vels) {
    uint8_t sn[ARP_MAX_HELD], sv[ARP_MAX_HELD], up[ARP_MAX_HELD * 4], upv[ARP_MAX_HELD * 4];
    int n = arp_sorted_keys(a, sn, sv), un = 0, len = 0;
    for (int o = 0; o < a->set.range; o++) {
        for (int i = 0; i < n; i++) {
            int note = sn[i] + 12 * o;
            if (note > 127) continue;
            up[un] = (uint8_t)note;
            upv[un++] = sv[i];
        }
    }
    if (a->set.type == ARP_DOWN) {
        for (int i = un - 1; i >= 0; i--) { notes[len] = up[i]; vels[len++] = upv[i]; }
        return len;
    }
    for (int i = 0; i < un; i++) { notes[len] = up[i]; vels[len++] = upv[i]; }
    if (a->set.type == ARP_ALT1) {        /* up then down, the top and bottom notes once */
        for (int i = un - 2; i >= 1; i--) { notes[len] = up[i]; vels[len++] = upv[i]; }
    } else if (a->set.type == ARP_ALT2) { /* up then down, the top and bottom notes twice */
        for (int i = un - 1; i >= 0; i--) { notes[len] = up[i]; vels[len++] = upv[i]; }
    }
    return len; /* UP, and RANDOM picks from it */
}

/* Fires step step_k: ends the previous notes, plays this step's unless the pattern rests it */
static void arp_fire_step(arp_t *a, arp_emit_fn emit, void *ctx) {
    arp_release(a, emit, ctx);
    double t0 = arp_step_time(a, a->step_k), t1 = arp_step_time(a, a->step_k + 1);
    int step = (int)(a->step_k % a->set.length);
    if (a->held_count > 0 && !((a->set.pattern >> step) & 1)) {
        if (a->set.type == ARP_TRIGGER) { /* every held key at once, an octave higher each step over the range */
            uint8_t sn[ARP_MAX_HELD], sv[ARP_MAX_HELD];
            int n = arp_sorted_keys(a, sn, sv), oct = a->seq_pos % a->set.range;
            for (int i = 0; i < n; i++) {
                if (sn[i] + 12 * oct <= 127) arp_sound(a, (uint8_t)(sn[i] + 12 * oct), sv[i], emit, ctx);
            }
        } else {
            uint8_t notes[ARP_MAX_SEQ], vels[ARP_MAX_SEQ];
            int len = arp_sequence(a, notes, vels);
            if (len > 0) {
                int i;
                if (a->set.type == ARP_RANDOM) {
                    a->rng ^= a->rng << 13;
                    a->rng ^= a->rng >> 17;
                    a->rng ^= a->rng << 5;
                    i = (int)(a->rng % (uint32_t)len);
                } else {
                    i = a->seq_pos % len;
                }
                arp_sound(a, notes[i], vels[i], emit, ctx);
            }
        }
        a->seq_pos++;
        if (a->sounding_count > 0) {
            double g = a->set.gate < ARP_MIN_GATE ? ARP_MIN_GATE : a->set.gate;
            a->gate_end = (g >= 0.999) ? t1 : t0 + g * (t1 - t0);
        }
    }
    a->step_k++;
    a->next_step = t1;
}

/* Clock: before each block, lock to the transport (beat >= 0, key sync off) or keep counting freely. A jump in the
 * transport (a loop, a restart) re-finds the step. */
static void arp_clock(arp_t *a, double beat) {
    int lock = beat >= 0.0 && !a->set.key_sync;
    if (lock) {
        int jumped = beat < a->pos - 0.5 * arp_step_beats(a) || beat > a->next_step + arp_step_beats(a);
        a->pos = beat;
        if (!a->locked || jumped) arp_resync(a);
    }
    a->locked = lock;
}

/* Fires everything due at the current position */
static void arp_process(arp_t *a, arp_emit_fn emit, void *ctx) {
    if (!a->running) return;
    if (a->gate_end >= 0.0 && a->pos >= a->gate_end - 1e-9) arp_release(a, emit, ctx);
    for (int guard = 0; guard < 4 && a->pos >= a->next_step - 1e-9; guard++) arp_fire_step(a, emit, ctx);
    if (a->pos >= a->next_step - 1e-9) arp_resync(a); /* far behind: skip ahead rather than burst */
}

/* Frames from now to the next step or gate end, at least 1 and at most max */
static int arp_frames_to_event(const arp_t *a, float bpm, float fs, int max) {
    if (!a->running || bpm <= 0.0f) return max;
    double t = a->next_step;
    if (a->gate_end >= 0.0 && a->gate_end < t) t = a->gate_end;
    double fr = (t - a->pos) * 60.0 / (double)bpm * (double)fs;
    if (fr >= (double)max) return max;
    int n = (int)ceil(fr - 1e-6);
    return n < 1 ? 1 : n;
}

static void arp_advance(arp_t *a, int frames, float bpm, float fs) {
    a->pos += (double)frames * (double)bpm / (60.0 * (double)fs);
}

/* A key: returns 1 when the arpeggio (re)starts. Latch keeps the keys after release; the next key after all were
 * released starts a new set. */
static int arp_key_on(arp_t *a, uint8_t note, uint8_t vel, arp_emit_fn emit, void *ctx) {
    if (a->set.latch && a->down_count == 0 && a->held_count > 0) {
        a->held_count = 0;
        if (a->set.key_sync) {
            arp_release(a, emit, ctx);
            a->running = 0;
        }
    }
    int i;
    for (i = 0; i < a->down_count && a->down_note[i] != note; i++) {}
    if (i == a->down_count && a->down_count < ARP_MAX_HELD) a->down_note[a->down_count++] = note;
    for (i = 0; i < a->held_count && a->held_note[i] != note; i++) {}
    if (i < a->held_count) {
        a->held_vel[i] = vel;
    } else if (a->held_count < ARP_MAX_HELD) {
        a->held_note[a->held_count] = note;
        a->held_vel[a->held_count++] = vel;
    }
    if (a->running) return 0;
    a->running = 1;
    a->seq_pos = 0;
    a->gate_end = -1.0;
    if (a->locked) {
        arp_resync(a); /* the next step on the transport's grid */
    } else {
        a->pos = 0.0;  /* free: start now */
        a->step_k = 0;
        a->next_step = 0.0;
    }
    return 1;
}

static void arp_key_off(arp_t *a, uint8_t note, arp_emit_fn emit, void *ctx) {
    for (int i = 0; i < a->down_count; i++) {
        if (a->down_note[i] != note) continue;
        a->down_note[i] = a->down_note[--a->down_count];
        break;
    }
    if (a->set.latch) return;
    for (int i = 0; i < a->held_count; i++) {
        if (a->held_note[i] != note) continue;
        a->held_note[i] = a->held_note[a->held_count - 1];
        a->held_vel[i] = a->held_vel[a->held_count - 1];
        a->held_count--;
        break;
    }
    if (a->held_count == 0) {
        arp_release(a, emit, ctx);
        a->running = 0;
    }
}
