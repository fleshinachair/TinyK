/*
 * microKORG arpeggiator: settings, held keys and playback state. The implementation is src/dsp/arp.c, which dsp.c
 * includes once (one translation unit, like syx_bank.h), so every build and tool keeps compiling dsp.c alone.
 */
#ifndef TINYK_ARP_H
#define TINYK_ARP_H

#include <stdint.h>

#define ARP_MAX_HELD   16                    /* keys the arpeggiator remembers */
#define ARP_MAX_CHORD  8                     /* notes one TRIGGER step can sound */
#define ARP_MAX_SEQ    (2 * ARP_MAX_HELD * 4) /* ALT2 over 16 keys and 4 octaves */

typedef enum { ARP_UP = 0, ARP_DOWN, ARP_ALT1, ARP_ALT2, ARP_RANDOM, ARP_TRIGGER, ARP_TYPE_COUNT } arp_type_t;

/* The program's settings (program bytes 14, 15, 32-36). `on` is also the Arp knob, and is saved in the slot state. */
typedef struct {
    int on, latch, key_sync;
    int target;      /* Layer mode: 0 both timbres, 1 Timbre 1, 2 Timbre 2 (the other timbre plays the keys directly) */
    int type;        /* arp_type_t */
    int range;       /* octaves, 1..4 */
    int resolution;  /* 0..5: 1/24, 1/16, 1/12, 1/8, 1/6, 1/4 */
    int length;      /* steps in the trigger pattern, 1..8 */
    uint8_t pattern; /* bit n set = step n + 1 rests */
    float gate;      /* 0..1 of a step */
    float swing;     /* -1..+1: moves every second step by up to a third of a step */
} arp_settings_t;

typedef struct {
    arp_settings_t set;

    /* Keys: `held` is what the arpeggio plays (kept after release while latched), `down` what is physically held */
    uint8_t held_note[ARP_MAX_HELD], held_vel[ARP_MAX_HELD];
    int held_count;
    uint8_t down_note[ARP_MAX_HELD];
    int down_count;

    /* Playback, in beats on the arpeggiator's clock: the transport's beat position while it runs (key sync off),
     * else a free-running count from the first key */
    int running;
    int locked;              /* following the transport */
    double pos;              /* current position, beats */
    int64_t step_k;          /* index of the next step */
    double next_step;        /* its time, beats */
    double gate_end;         /* when the sounding notes end, beats (< 0: none) */
    int seq_pos;             /* position in the note order, advanced per played step */
    uint8_t sounding[ARP_MAX_CHORD];
    int sounding_count;
    uint32_t rng;            /* RANDOM (deterministic, reset with the arpeggiator) */
} arp_t;

#endif /* TINYK_ARP_H */
