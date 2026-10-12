#!/usr/bin/env python3
"""
Build TinyK's default bank, banks/TinyK_Default.syx, from a microKORG factory bank dump.

    python tools/make_default_bank.py [banks/MicroKorgFactory.syx] [banks/TinyK_Default.syx]

Every program keeps its sound design but gets an original name (NAMES, in bank order A.11..A.88,
B.11..B.88) and a small, repeatable jitter: one to three continuous values per active timbre move by
1 or 2 steps (filter cutoff, EG attack / decay / release, Osc 2 fine tune). Selectors (waves, filter
type, sync / ring, voice mode, routings) are never touched, nor are values whose character depends on
being exact: an EG time of 0 (instant) or at the top of its range, and Osc 2 tune at 0 (unison).
Vocoder programs use another data layout and are only renamed, except A.81, which TinyK replaces with
"Init" (INIT_SLOT, init_program): a clean, neutral single-layer patch, the whole of the "Init" category (matrix
row 8, which holds nothing else playable because TinyK has no vocoder).

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
SEED = 2026
INIT_SLOT = 56  # A.81: first of matrix row 8, the vocoder row TinyK does not offer

# Original names, bank order: side A rows 1-8 then side B rows 1-8, 8 per row (12 characters max)
NAMES = [
    # A.11-A.18 Trance
    "StarlitArp", "AcidRazor", "WideSawLead", "TwinFilterLd", "TearDrop", "GlidePad", "SunrisePad", "SilkStrings",
    # A.21-A.28 Techno/House
    "ClubPulse", "RaveFire", "CrossPerc", "DeepGroove", "GritBass", "SquareAcid", "SyncDrive", "HardLine",
    # A.31-A.38 Electronica
    "PulseGrid", "Bleepscape", "ChopSync", "Wingbeat", "BackwardLd", "GlitchCloud", "FifthFlange", "VowelDrone",
    # A.41-A.48 DnB/Breaks
    "GarageWobble", "SteelBass", "TubeThump", "OverdriveSub", "EdgeBass", "SyncSweeper", "LabLead", "ChopChord",
    # A.51-A.58 Hiphop/Vintage
    "DustyBass", "FatMono 1", "SilkLead", "PulseStrings", "ReedKeys", "SoulOrgan", "ClavFunk", "TapeVoices",
    # A.61-A.68 Retro
    "VoltArp", "Shoreline", "NeonBass", "RewindSync", "BrightPoly", "ClassicPoly", "WarmFourths", "OctaveSilk",
    # A.71-A.78 SE/Hit
    "SwarmFX", "DataStorm", "Glitchy", "ArcadeZap", "MetalChord", "MinorSweep", "StaticHit", "Minor7Stack",
    # A.81-A.88 Vocoder (A.81 is the Init program)
    "Init", "BaritoneEe", "FifthChoir", "RobotEnsmbl", "RobotChorus", "RobotFifth", "RobotBass", "VoiceMorph",
    # B.11-B.18 Trance
    "CrystalPluck", "RingAcid", "MetalShimmer", "PhaseRider", "PizzStab", "Elation", "StrobePad", "RiverPad",
    # B.21-B.28 Techno/House
    "RandomBeacon", "GrimeMotion", "MetalTick", "BasementOrg", "WideSquare", "DriftBass", "PunchBass", "VoltStab",
    # B.31-B.38 Electronica
    "StaticBurst", "NeoPerc", "PulsingPad", "BreathOrgan", "SlowBend", "NarrowFourth", "HorizonPad", "DuskPad",
    # B.41-B.48 DnB/Breaks
    "CrossBass", "HollowBass", "SuckBass", "IronSync", "RisingBass", "DropZone", "WheelLead", "DirtStorm",
    # B.51-B.58 Hiphop/Vintage
    "ShadowBass", "FatMono 2", "LowEnd", "DiscoLead", "DriveOrgan", "ClickOrgan", "SwirlClav", "StringBox",
    # B.61-B.68 Retro
    "GlassBell", "StepPad", "SoftTriLead", "ChanceComp", "SawHit", "BoxComp", "WobbleComp", "VintageStr",
    # B.71-B.78 SE/Hit
    "WarpZone", "Overlord", "StormCrack", "Siren", "ThinMin7", "MajorThird", "RaveHit", "ArtMaj7",
    # B.81-B.88 Vocoder
    "SopranoAh", "SmallVoice", "WowVoice", "RobotPulse", "RobotSquare", "RobotWah", "RobotVox", "RobotDigi",
]

# Continuous timbre values that may be nudged: timbre offset -> (name, lowest, highest value it may have
# before jitter). Out-of-range values keep their exact meaning and are skipped.
JITTER_FIELDS = {
    20: ("cutoff", 2, 125),
    30: ("filter EG attack", 3, 124), 31: ("filter EG decay", 3, 124), 33: ("filter EG release", 3, 124),
    34: ("amp EG attack", 3, 124), 35: ("amp EG decay", 3, 124), 37: ("amp EG release", 3, 124),
    14: ("osc2 tune", 2, 126),
}


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


def jitter_program(prog, idx, rng, log):
    voice_mode = (prog[16] >> 4) & 0x03
    if voice_mode == 3:
        return  # vocoder layout: renamed only
    for t in TIMBRE_OFFSETS[:2 if voice_mode == 2 else 1]:
        fields = [off for off, (_, lo, hi) in JITTER_FIELDS.items()
                  if lo <= prog[t + off] <= hi and not (off == 14 and prog[t + off] == 64)]
        for off in rng.sample(fields, min(len(fields), rng.randint(1, 3))):
            step = rng.choice((-2, -1, 1, 2))
            old = prog[t + off]
            prog[t + off] = max(0, min(127, old + step))
            log.append((idx, t, JITTER_FIELDS[off][0], old, prog[t + off]))


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
        if idx == INIT_SLOT:
            prog[:] = init_program()
            prog[0:NAME_LEN] = NAMES[idx].ljust(NAME_LEN).encode("ascii")
            continue
        jitter_program(prog, idx, rng, log)

    out = header + pack_7to8(bytes(raw))[:len(packed)] + trailer
    assert len(out) == len(data)
    with open(dst, "wb") as f:
        f.write(out)

    code = lambda i: f"{'AB'[i // 64]}.{(i % 64) // 8 + 1}{i % 8 + 1}"
    print(f"wrote {dst} ({len(out)} bytes): 128 programs renamed, {len(log)} values nudged by 1-2 steps")
    for idx, t, name, old, new in log[:12]:
        print(f"  {code(idx)} {NAMES[idx]:12} timbre {1 if t == 38 else 2} {name}: {old} -> {new}")
    if len(log) > 12:
        print(f"  ... {len(log) - 12} more")


if __name__ == "__main__":
    main()
