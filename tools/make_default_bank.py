#!/usr/bin/env python3
"""
Build TinyK's default bank, banks/TinyK_Default.syx, from a microKORG factory bank dump.

    python tools/make_default_bank.py [banks/MicroKorgFactory.syx] [banks/TinyK_Default.syx]

Every program keeps its sound design's structure (waves, routings, selectors, voice mode, arpeggiator) but gets
a generic name (NAMES, in bank order A.11..A.88, B.11..B.88) and a seeded musical variation pass (vary_program):
filter cutoff, resonance, filter EG depth, EG attack / decay / release, LFO rates, Osc 2 tune and the delay /
Mod FX settings move by several steps each, so no program is the dump's values. Values whose character depends
on being exact are never touched: an EG time of 0 (instant) or at the top of its range, a filter that is fully
open or closed, Osc 2 tune at 0 (unison), sustain levels, tempo-synced times.
Vocoder programs (TinyK has no vocoder, it never offers them) are replaced by a flagged copy of the Init
program, so no vocoder data is carried; A.81 is the Init program itself (INIT_SLOT, init_program): a clean,
neutral single-layer patch, the whole of the "Init" category (matrix row 8).

The output is an ALL PROGRAM DATA dump in the input's framing; feed it to tools/extracts_presets.py
to regenerate the built-in bank (presets.h / presets.json).
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from extracts_presets import unpack_7to8  # noqa: E402

PROGRAM_SIZE, NAME_LEN = 254, 12
TIMBRE_OFFSETS = (38, 146)
SEED = 2030
INIT_SLOT = 56  # A.81: first of matrix row 8, the vocoder row TinyK does not offer

# Original names, bank order: side A rows 1-8 then side B rows 1-8, 8 per row (12 characters max)
NAMES = [
    # A.11-A.18 Trance
    "Spark Arp", "Acid Lead", "Wide Saw", "Dual Filter", "Soft Lead", "Glide Pad", "Warm Pad", "Soft Strings",
    # A.21-A.28 Techno/House
    "Club Pulse", "Rave Stab", "Cross Perc", "Deep Groove", "Gritty Bass", "Square Acid", "Sync Drive", "Hard Line",
    # A.31-A.38 Electronica
    "Pulse Grid", "Bleep Scape", "Chop Sync", "Wing Beat", "Reverse Lead", "Glitch Cloud", "Fifth Flange", "Vowel Drone",
    # A.41-A.48 DnB/Breaks
    "Wobble Bass", "Steel Bass", "Tube Thump", "Drive Sub", "Edge Bass", "Sync Sweep", "Lab Lead", "Chop Chord",
    # A.51-A.58 Hiphop/Vintage
    "Dusty Bass", "Fat Mono 1", "Silk Lead", "Pulse Strgs", "Reed Keys", "Soul Organ", "Funk Clav", "Tape Voices",
    # A.61-A.68 Retro
    "Volt Arp", "Shoreline", "Neon Bass", "Rewind Sync", "Bright Poly", "Classic Poly", "Warm Fourths", "Octave Silk",
    # A.71-A.78 SE/Hit
    "Swarm FX", "Data Storm", "Glitchy", "Arcade Zap", "Metal Chord", "Minor Sweep", "Static Hit", "Minor7 Stack",
    # A.81-A.88: A.81 is the Init program, the rest are unused (the vocoder row, never offered)
    "Init", "Unused 1", "Unused 2", "Unused 3", "Unused 4", "Unused 5", "Unused 6", "Unused 7",
    # B.11-B.18 Trance
    "Glass Pluck", "Ring Acid", "Metal Shine", "Phase Lead", "Pizz Stab", "Uplift", "Pulse Pad", "Flow Pad",
    # B.21-B.28 Techno/House
    "Random Beep", "Grime Motion", "Metal Tick", "Basement Org", "Wide Square", "Drift Bass", "Punch Bass", "Volt Stab",
    # B.31-B.38 Electronica
    "Static Burst", "Neo Perc", "Pulsing Pad", "Breath Organ", "Slow Bend", "Narrow 4th", "Horizon Pad", "Dusk Pad",
    # B.41-B.48 DnB/Breaks
    "Cross Bass", "Hollow Bass", "Suck Bass", "Iron Sync", "Rising Bass", "Drop Zone", "Wheel Lead", "Dirt Storm",
    # B.51-B.58 Hiphop/Vintage
    "Shadow Bass", "Fat Mono 2", "Low End", "Disco Lead", "Drive Organ", "Click Organ", "Swirl Clav", "String Box",
    # B.61-B.68 Retro
    "Glass Bell", "Step Pad", "Soft Tri", "Chance Comp", "Saw Hit", "Box Comp", "Wobble Comp", "Vintage Str",
    # B.71-B.78 SE/Hit
    "Warp Zone", "Overlord", "Storm Crack", "Siren", "Thin Minor 7", "Major Third", "Rave Hit", "Art Major 7",
    # B.81-B.88: unused (the vocoder row)
    "Unused 8", "Unused 9", "Unused 10", "Unused 11", "Unused 12", "Unused 13", "Unused 14", "Unused 15",
]

# Timbre values the variation pass may move: offset -> (name, lowest and highest value it may have before the
# move, smallest and largest move in steps). Values outside the range keep their exact meaning and are skipped.
VARY_FIELDS = {
    20: ("cutoff", 6, 122, 4, 14),
    21: ("resonance", 4, 100, 3, 9),
    22: ("filter EG int", 0, 127, 3, 8),  # moves away from neutral (64) only, see vary_program
    30: ("filter EG attack", 3, 124, 3, 9), 31: ("filter EG decay", 3, 124, 3, 9), 33: ("filter EG release", 3, 124, 3, 9),
    34: ("amp EG attack", 3, 124, 3, 9), 35: ("amp EG decay", 3, 124, 3, 9), 37: ("amp EG release", 3, 124, 3, 9),
    14: ("osc2 tune", 2, 126, 2, 5),
    39: ("LFO1 rate", 4, 123, 3, 8), 42: ("LFO2 rate", 4, 123, 3, 8),
}
# Program-level effect values (offset, name, lowest, highest, smallest, largest move): only when their effect is on
FX_VARY = ((20, "delay time", 6, 121, 4, 10), (23, "Mod FX speed", 6, 121, 4, 10))
VARY_CHANCE = 0.65  # each eligible field moves with this probability
MIN_MOVES = 3       # and every timbre moves at least this many


def voice_mode_of(prog):
    return (prog[16] >> 4) & 0x03


def init_program():
    """The Init program (254 bytes, name left blank): the engine's neutral state. Single mode, Poly, one Saw at
    full level through an open-ish LPF24 with no resonance and no modulation, flat envelopes, no distortion, no
    Mod FX / delay / EQ, arpeggiator off. Timbre 2 repeats timbre 1 so a switch to Layer starts clean too."""
    prog = bytearray(PROGRAM_SIZE)
    prog[12:38] = bytes([0, 0, 7, 0,        # 12-15: -, -, arp trigger length - 1, trigger pattern
                         0, 0, 60, 0,       # 16 voice mode Single (bits 4-5), 17, 18, 19 delay tempo sync off
                         64, 0, 0,          # 20 delay time, 21 delay depth (0 = off), 22 delay type Stereo
                         64, 0, 0,          # 23 Mod FX speed, 24 depth (0 = off), 25 type Chorus/Flanger
                         20, 64, 15, 64,    # 26-29 EQ: Hi freq / gain, Low freq / gain (gain 64 = flat)
                         0, 120, 0, 0,      # 30-31 arp tempo (ignored), 32 arp off, 33 type Up / range 1 oct
                         100, 1, 0, 0])     # 34 gate, 35 resolution, 36 swing, 37 keyboard octave 0
    timbre = bytearray(108)
    timbre[0:7] = bytes([255, 0x70, 0, 64, 66, 64, 64])  # +1: Poly, EG1 / EG2 reset on; tune, bend +-2, transpose, vibrato 0
    timbre[13:19] = bytes([64, 64, 0, 127, 0, 0])        # Osc 2 semitone / tune, portamento, levels Osc 1 / Osc 2 / Noise
    timbre[19:30] = bytes([0, 89, 0, 64, 64, 64, 127, 64, 0, 64, 50])  # LPF24, cutoff 89, res 0, EG int 0, -, key track 0, level, pan C, dist off
    timbre[30:38] = bytes([0, 64, 127, 0, 0, 64, 127, 10])  # EG1 (filter) A D S R, EG2 (amp) A D S R
    timbre[38:44] = bytes([0, 64, 0, 0, 64, 0])          # LFO1 / LFO2: Saw, rate 64, free
    for n in range(4):
        timbre[44 + 2 * n:46 + 2 * n] = bytes([0, 64])   # patches: intensity 64 = no modulation
    for lo, hi, v in ((56, 72, 64), (74, 90, 64), (92, 108, 64)):
        timbre[lo:hi] = bytes([v] * (hi - lo))           # unused arrays, as in the factory programs
    prog[38:146] = timbre
    prog[146:254] = timbre
    return prog


def pack_7to8(raw):
    """Inverse of unpack_7to8: 7 data bytes -> [MSB bits][7 bytes with bit 7 cleared]."""
    out = bytearray()
    for i in range(0, len(raw), 7):
        chunk = raw[i:i + 7]
        out.append(sum(1 << j for j, b in enumerate(chunk) if b & 0x80))
        out.extend(b & 0x7F for b in chunk)
    return bytes(out)


def split_dump(data):
    """(header, packed payload, trailer) of the first bank message: F0 42 3g 58 4C ... F7."""
    start = data.find(b"\xF0")
    end = data.find(b"\xF7", start)
    if start < 0 or end < 0 or data[start + 1] != 0x42 or data[start + 3] != 0x58 or data[start + 4] != 0x4C:
        sys.exit("not a microKORG ALL PROGRAM DATA dump (F0 42 3g 58 4C ... F7)")
    return data[:start + 5], data[start + 5:end], data[end:]


def vary_program(prog, idx, rng, log):
    """The musical variation pass over one synth program (see the module docstring)."""
    voice_mode = (prog[16] >> 4) & 0x03
    for t in TIMBRE_OFFSETS[:2 if voice_mode == 2 else 1]:
        fields = [off for off, (_, lo, hi, _, _) in VARY_FIELDS.items()
                  if lo <= prog[t + off] <= hi and not (off == 14 and prog[t + off] == 64)
                  and not (off == 22 and abs(prog[t + off] - 64) < 10)
                  and not (off in (39, 42) and prog[t + off - 1 + 2] & 0x80)]  # tempo-synced LFO: rate unused
        chosen = [off for off in fields if rng.random() < VARY_CHANCE]
        if len(chosen) < MIN_MOVES:
            chosen = rng.sample(fields, min(len(fields), MIN_MOVES))
        for off in chosen:
            name, lo, hi, small, large = VARY_FIELDS[off]
            step = rng.randint(small, large) * rng.choice((-1, 1))
            old = prog[t + off]
            new = old + step
            if off == 22:  # the filter EG depth only eases toward neutral (a deeper sweep makes the peaks louder)
                new = old - abs(step) if old > 64 else old + abs(step)
            if off == 20:  # a nearly closed filter stays audible: low cutoffs only rise, none drops under 18
                new = old + abs(step) if old < 30 else max(18, new)
            prog[t + off] = max(0, min(127, new))
            log.append((idx, t, name, old, prog[t + off]))
    if prog[21] > 0 and not prog[19] & 0x80:  # delay on and free-running
        _vary_fx(prog, idx, rng, log, FX_VARY[0])
    if prog[24] > 0:  # Mod FX on
        _vary_fx(prog, idx, rng, log, FX_VARY[1])


def _vary_fx(prog, idx, rng, log, spec):
    off, name, lo, hi, small, large = spec
    if lo <= prog[off] <= hi:
        old = prog[off]
        prog[off] = max(0, min(127, old + rng.randint(small, large) * rng.choice((-1, 1))))
        log.append((idx, 0, name, old, prog[off]))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "banks", "MicroKorgFactory.syx")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "banks", "TinyK_Default.syx")
    assert len(NAMES) == 128 and len(set(NAMES)) == 128, "need 128 distinct names"
    assert all(len(n) <= NAME_LEN and n.isascii() for n in NAMES), [n for n in NAMES if len(n) > NAME_LEN]

    data = open(src, "rb").read()
    header, packed, trailer = split_dump(data)
    raw = bytearray(unpack_7to8(packed))
    if len(raw) < 128 * PROGRAM_SIZE:
        sys.exit(f"{src}: {len(raw)} unpacked bytes, expected {128 * PROGRAM_SIZE}")
    if pack_7to8(bytes(raw))[:len(packed)] != packed:
        sys.exit("7-bit repacking does not reproduce the input: refusing to write")

    rng = random.Random(SEED)
    log = []
    for idx in range(128):
        prog = memoryview(raw)[idx * PROGRAM_SIZE:(idx + 1) * PROGRAM_SIZE]
        prog[0:NAME_LEN] = NAMES[idx].ljust(NAME_LEN).encode("ascii")
        if idx == INIT_SLOT or voice_mode_of(prog) == 3:  # Init, and the vocoder slots TinyK never offers
            prog[:] = init_program()
            if idx != INIT_SLOT:
                prog[16] |= 0x30  # flagged vocoder: skipped by the program list, no vocoder data carried
            prog[0:NAME_LEN] = NAMES[idx].ljust(NAME_LEN).encode("ascii")
            continue
        prog[0:NAME_LEN] = NAMES[idx].ljust(NAME_LEN).encode("ascii")
        vary_program(prog, idx, rng, log)

    out = header + pack_7to8(bytes(raw))[:len(packed)] + trailer
    assert len(out) == len(data)
    with open(dst, "wb") as f:
        f.write(out)

    code = lambda i: f"{'AB'[i // 64]}.{(i % 64) // 8 + 1}{i % 8 + 1}"
    print(f"wrote {dst} ({len(out)} bytes): 128 programs renamed, {len(log)} values varied")
    for idx, t, name, old, new in log[:12]:
        print(f"  {code(idx)} {NAMES[idx]:12} timbre {1 if t == 38 else 2} {name}: {old} -> {new}")
    if len(log) > 12:
        print(f"  ... {len(log) - 12} more")


if __name__ == "__main__":
    main()
