#!/usr/bin/env python3
"""
Build TinyK's default bank, banks/TinyK_Default.syx, from a microKORG factory bank dump.

    python tools/make_default_bank.py [banks/MicroKorgFactory.syx] [banks/TinyK_Default.syx]

Every program keeps its sound design but gets an original name (NAMES, in bank order A.11..A.88,
B.11..B.88) and a small, repeatable jitter: one to three continuous values per active timbre move by
1 or 2 steps (filter cutoff, EG attack / decay / release, Osc 2 fine tune). Selectors (waves, filter
type, sync / ring, voice mode, routings) are never touched, nor are values whose character depends on
being exact: an EG time of 0 (instant) or at the top of its range, and Osc 2 tune at 0 (unison).
Vocoder programs use another data layout and are only renamed.

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
    # A.81-A.88 Vocoder
    "BaritoneAh", "BaritoneEe", "FifthChoir", "RobotEnsmbl", "RobotChorus", "RobotFifth", "RobotBass", "VoiceMorph",
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
