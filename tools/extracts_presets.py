#!/usr/bin/env python3
"""
microKORG SysEx preset extractor.

Parses a microKORG "ALL DATA DUMP" (F0 42 3g 58 50 ... F7): 128 programs of
254 bytes each, Korg 7-bit packed. Emits presets.json and presets.h with
Timbre 1 and Timbre 2 parameters, all normalized to [0.0, 1.0].

Program layout (offsets into the 254-byte unpacked program):
    0..11     patch name (ASCII)
    16        bits 4-5: voice mode (0 = Single, 2 = Layer, 3 = Vocoder)
    19..25    delay / mod-FX
    38..145   Timbre 1 (108 bytes)
    146..253  Timbre 2 (108 bytes)

Timbre layout (offset from timbre start):
    3 tune, 5 transpose, 7 osc1 wave, 8 osc1 ctrl1 (pulse width), 10 DWGS wave,
    12 osc2 (bits 4-5 mod select, bits 0-1 wave), 13 osc2 semitone, 14 osc2 tune,
    15 portamento, 16/17/18 osc1/osc2/noise level, 19 filter type, 20 cutoff,
    21 resonance, 22 EG1 intensity, 24 key track, 27 distortion,
    30..33 EG1 (filter) ADSR, 34..37 EG2 (amp) ADSR.
"""

import json
import os
import sys

NUM_PROGRAMS = 128
PROGRAM_SIZE = 254
TIMBRE_OFFSETS = (38, 146)

# The dump stores blank (all-space) names because the hardware only has a 3-digit
# LED. These are the labels shown in the Move UI; they are used whenever the dump
# carries no name. Their provenance is not verified against the official list.
FACTORY_LABELS = [
    "Saw Lead", "Trance Bass", "Euro Pad", "Gate Synth", "Hyper Saw", "Rave Pluck", "Stadium Lead", "Anthem Pad",
    "Acid 303", "Deep Organ", "Detroit Chord", "Tech Stab", "Club Bass", "Minimal Bleep", "Filter Clav", "Pump Lead",
    "Ambient Drone", "Glitch Lead", "Chill Pad", "8-Bit Lo-Fi", "Crystal Bell", "Sub Sine Bass", "Space Texture", "Modular Sequence",
    "Reese Bass", "Jungle Sub", "Siren Lead", "Break Pluck", "Neuro Bass", "Liquid Pad", "2-Step Bass", "Rave Stab",
    "Funk Moog", "G-Funk Lead", "Vintage EP", "West Coast Whistle", "Motown Low", "Oldschool Brass", "P-Funk Lead", "Retro Flute",
    "1984 Brass", "Synthwave Arp", "Stranger Pad", "Poly 800 Strings", "Miami Bass", "Blade Lead", "New Wave Pluck", "Disco Octave",
    "Laser Sweep", "Thunder Drop", "Metallic Zap", "Ring Drone", "Psycho Bell", "Noise Sweep", "Alien UFO", "Space Beacon",
    "Robot Vox", "Choir Vox", "Talkbox Synth", "Vocoder Bass", "Whispering Vox", "Android Pad", "Cyborg Solo", "Vocoder Sweep",
    "Goa Trance", "Hardstyle Bass", "Uplifting Strings", "Psytrance Arp", "Trancegate Pad", "Energy Pluck", "Hard Trance Lead", "Dreamscape",
    "Chicago Bass", "Deep Tech Chord", "French Filter", "Techno Drone", "Garage Bass", "Sub Drop", "Electro Clash", "Jacking Pluck",
    "Glitch Pad", "Metallic Bell", "Warm Glass", "Granular Tone", "Dark Drone", "Modular Bass", "Shimmer Pad", "Space Echoes",
    "Darkstep Reese", "808 Sub Boom", "Hardstep Horn", "Rollers Bass", "Jungle Flute", "Amen Stab", "Screamer Lead", "Atmospheric DnB",
    "Talkin Bass", "Vintage Poly EP", "Warm 5ths", "R&B Sine", "70s Solina", "P-Funk Clav", "Classic Whistle", "Retro Organ",
    "Synthpop Lead", "Vaporwave Pad", "Italo Disco Bass", "Stranger Bells", "VHS Strings", "Cyberpunk Saw", "Analog Brass II", "Retrowave Arp",
    "Alien Radio", "Hydro Sweep", "Cosmic Ray", "Industrial Clang", "Warp Drive", "Seismic Hit", "Cyber Glitch", "Vortex Beam",
    "Vocoder Ensemble", "Android Speech", "Vocoder Strings", "Funky Robot", "Space Vocoder", "Digital Voweller", "Cyber Choir", "Reso Formant Drone",
]

# Timbre float fields, in the order they appear in struct TimbreParams.
TIMBRE_FIELDS = [
    "wave1", "pulse_width", "wave2", "detune", "sync_ring", "osc_mix", "sub_level", "noise_level", "level",
    "portamento", "transpose", "dwgs",
    "cutoff", "resonance", "filter_type", "keytrack", "env_int", "drive",
    "attack1", "decay1", "sustain1", "release1", "attack2", "decay2", "sustain2", "release2",
]
FX_FIELDS = ["chorus_mix", "delay_time", "delay_feedback", "delay_mix"]

# Hardware osc2 mod-select (0 off, 1 ring, 2 sync, 3 ring+sync) -> engine
# sync_ring index (0 off, 1 sync, 2 ring, 3 both).
MODSEL_TO_ENGINE = {0: 0, 1: 2, 2: 1, 3: 3}


# Vocoder programs (voice mode 3) do not use the synth timbre layout: their Timbre 2 area holds
# vocoder channel data (bytes > 127) and the carrier fields are constants (portamento 127,
# osc1 level 0). That layout is not documented here and the engine has no vocoder, so decoding
# them as synth timbres yields near-silent garbage. They get this generic, audible carrier
# instead and are flagged data_valid=false in the JSON.
VOCODER_CARRIER = {
    "wave1": 0.0, "pulse_width": 0.0, "wave2": 0.0, "detune": 0.5 + 0.12 / 48.0, "sync_ring": 0.0,
    "osc_mix": 0.5, "sub_level": 0.0, "noise_level": 0.0, "level": 0.9, "portamento": 0.0, "transpose": 0.5,
    "dwgs": 0.0, "cutoff": 0.72, "resonance": 0.12, "filter_type": 0.0, "keytrack": 0.75,
    "env_int": 0.5, "drive": 0.0,
    "attack1": 0.2, "decay1": 0.5, "sustain1": 1.0, "release1": 0.45,
    "attack2": 0.25, "decay2": 0.5, "sustain2": 1.0, "release2": 0.45,
}


def clamp01(val):
    """Clamp to [0.0, 1.0]."""
    return max(0.0, min(1.0, float(val)))


def unit(raw):
    """0..127 -> 0..1."""
    return clamp01(raw / 127.0)


def bipolar(raw):
    """0..127 with 64 = neutral -> 0..1 with exactly 0.5 = neutral."""
    return clamp01(0.5 + (raw - 64) / 126.0)


def unpack_7to8(packed):
    """Korg 7-bit decode: each 8-byte group is [MSB bits][7 data bytes]."""
    out = bytearray()
    for i in range(0, len(packed), 8):
        msbs = packed[i]
        for j, b in enumerate(packed[i + 1:i + 8]):
            out.append(b | 0x80 if msbs & (1 << j) else b)
    return out


def load_programs(syx_path):
    with open(syx_path, "rb") as f:
        data = f.read()

    start, end = data.find(b"\xF0"), data.rfind(b"\xF7")
    if start == -1 or end == -1:
        raise ValueError("Invalid SysEx file: F0 / F7 framing not found.")
    # F0 42 3g 58 50: Korg, microKORG, ALL DATA DUMP.
    if data[start + 1] != 0x42 or data[start + 3] != 0x58 or data[start + 4] != 0x50:
        raise ValueError("Not a microKORG ALL DATA DUMP (expected F0 42 3g 58 50).")

    unpacked = unpack_7to8(data[start + 5:end])
    if len(unpacked) < NUM_PROGRAMS * PROGRAM_SIZE:
        raise ValueError(
            f"Dump too short: {len(unpacked)} bytes unpacked, need {NUM_PROGRAMS * PROGRAM_SIZE}.")
    print(f"SysEx OK: {end - start + 1} bytes -> {len(unpacked)} unpacked "
          f"({NUM_PROGRAMS} programs x {PROGRAM_SIZE})")
    return [unpacked[i * PROGRAM_SIZE:(i + 1) * PROGRAM_SIZE] for i in range(NUM_PROGRAMS)]


def parse_timbre(prog, t):
    """Parse one timbre starting at byte offset t into normalized floats."""
    osc1_wave = prog[t + 7] & 0x07
    # saw, pulse, triangle, sine, vox, DWGS, noise; audio-in (7) falls back to saw
    wave1 = 0 if osc1_wave > 6 else osc1_wave

    osc2_wave = min(prog[t + 12] & 0x03, 2)
    mod_select = MODSEL_TO_ENGINE[(prog[t + 12] >> 4) & 0x03]

    # Osc2 pitch relative to osc1 in semitones: semitone (+-24) plus tune (+-50 cents).
    osc2_semis = (prog[t + 13] - 64) + (prog[t + 14] - 64) / 63.0 * 0.5
    # Timbre pitch: transpose (+-24) plus tune (+-50 cents).
    transpose_semis = (prog[t + 5] - 64) + (prog[t + 3] - 64) / 100.0

    # Mixer: the hardware sums osc1 and osc2 at their own levels. The engine models that as a
    # balance knob (0 = osc1 only, 0.5 = both full, 1 = osc2 only) plus an overall level, which
    # also carries the timbre's amp level byte.
    osc1_lvl, osc2_lvl = prog[t + 16] / 127.0, prog[t + 17] / 127.0
    loudest = max(osc1_lvl, osc2_lvl)
    if loudest <= 0.0:
        osc_mix = 0.5
    elif osc1_lvl >= osc2_lvl:
        osc_mix = 0.5 * osc2_lvl / osc1_lvl
    else:
        osc_mix = 1.0 - 0.5 * osc1_lvl / osc2_lvl
    level = loudest * unit(prog[t + 25])

    return {
        "wave1": wave1 / 6.0,
        "pulse_width": unit(prog[t + 8]),  # engine maps 0..1 -> 50%..95% duty
        "wave2": osc2_wave / 2.0,
        "detune": clamp01(0.5 + osc2_semis / 48.0),
        "sync_ring": mod_select / 3.0,
        "osc_mix": clamp01(osc_mix),
        "level": clamp01(level),
        "sub_level": 0.0,  # the microKORG has no sub oscillator
        "noise_level": unit(prog[t + 18]),
        "portamento": unit(prog[t + 15]),
        "transpose": clamp01(0.5 + transpose_semis / 48.0),
        "dwgs": clamp01(min(prog[t + 10], 63) / 63.0),
        "cutoff": unit(prog[t + 20]),
        "resonance": unit(prog[t + 21]),
        "filter_type": (prog[t + 19] & 0x03) / 3.0,
        "keytrack": bipolar(prog[t + 24]),
        "env_int": bipolar(prog[t + 22]),
        "drive": 0.5 if prog[t + 27] & 1 else 0.0,
        "attack1": unit(prog[t + 30]), "decay1": unit(prog[t + 31]),
        "sustain1": unit(prog[t + 32]), "release1": unit(prog[t + 33]),
        "attack2": unit(prog[t + 34]), "decay2": unit(prog[t + 35]),
        "sustain2": unit(prog[t + 36]), "release2": unit(prog[t + 37]),
    }


def parse_program(idx, prog):
    code = f"{'A' if idx < 64 else 'B'}.{((idx % 64) // 8) + 1}{(idx % 8) + 1}"

    raw_name = bytes(c for c in prog[0:12] if 32 <= c <= 126).decode("ascii").strip()
    name = raw_name or FACTORY_LABELS[idx]

    mode_bits = (prog[16] >> 4) & 0x03
    mode = {0: "single", 2: "layer", 3: "vocoder"}.get(mode_bits, "single")

    # The hardware has one delay depth (byte 21) that sets repeats and level together, and no
    # separate on/off: depth 0 is off. The engine adds the repeats as a send (dry stays unity),
    # so mix and feedback both come from the depth, and depth 0 bypasses the delay line.
    delay_fb = unit(prog[21])
    fx = {
        "chorus_mix": unit(prog[24]),
        "delay_time": unit(prog[20]),
        "delay_feedback": delay_fb,
        "delay_mix": delay_fb,
    }

    data_valid = mode != "vocoder"
    if data_valid:
        t1, t2 = parse_timbre(prog, TIMBRE_OFFSETS[0]), parse_timbre(prog, TIMBRE_OFFSETS[1])
    else:
        t1, t2 = dict(VOCODER_CARRIER), dict(VOCODER_CARRIER)

    return {
        "label": f"{code} {name}",
        "name_from_syx": bool(raw_name),
        "mode": mode,
        "data_valid": data_valid,
        # Vocoder programs have no engine equivalent; they play as Single.
        "voice_mode": 1.0 if mode == "layer" else 0.0,
        "t1": t1,
        "t2": t2,
        "fx": fx,
    }


def c_float(v):
    return f"{v:.6f}f"


def render_header(presets):
    out = []
    out.append("/* Auto-generated microKORG Factory Presets Header (tools/extracts_presets.py) */\n")
    out.append("#ifndef PRESETS_H\n#define PRESETS_H\n\n")
    out.append("/* All floats are normalized to [0.0, 1.0]. */\n")
    out.append("struct TimbreParams {\n")
    out.append("    float wave1, pulse_width, wave2, detune, sync_ring, osc_mix, sub_level, noise_level, level;\n")
    out.append("    float portamento, transpose, dwgs;\n")
    out.append("    float cutoff, resonance, filter_type, keytrack, env_int, drive;\n")
    out.append("    float attack1, decay1, sustain1, release1, attack2, decay2, sustain2, release2;\n")
    out.append("};\n\n")
    out.append("struct Preset {\n")
    out.append("    const char *label;\n")
    out.append("    int voice_mode; /* 0 = Single, 1 = Layer */\n")
    out.append("    struct TimbreParams t1, t2;\n")
    out.append("    float chorus_mix, delay_time, delay_feedback, delay_mix;\n")
    out.append("};\n\n")
    out.append(f"static const struct Preset FACTORY_PRESETS[{len(presets)}] = {{\n")

    for i, p in enumerate(presets):
        timbres = []
        for key in ("t1", "t2"):
            vals = ", ".join(c_float(p[key][f]) for f in TIMBRE_FIELDS)
            timbres.append("{ " + vals + " }")
        fx = ", ".join(c_float(p["fx"][f]) for f in FX_FIELDS)
        label = p["label"].replace("\\", "\\\\").replace('"', '\\"')
        comma = "," if i < len(presets) - 1 else ""
        out.append(f"    /* [{i:3d}] {p['label']} */\n")
        out.append(f'    {{ "{label}", {int(p["voice_mode"])},\n')
        out.append(f"      {timbres[0]},\n      {timbres[1]},\n      {fx} }}{comma}\n")

    out.append("};\n\n#endif /* PRESETS_H */\n")
    return "".join(out)


def render_json(presets):
    entries = []
    for p in presets:
        e = {
            "label": p["label"],
            "name_from_syx": p["name_from_syx"],
            "mode": p["mode"],
            "data_valid": p["data_valid"],
            "voice_mode": p["voice_mode"],
        }
        for f in TIMBRE_FIELDS:
            e[f] = round(p["t1"][f], 6)
        for f in FX_FIELDS:
            e[f] = round(p["fx"][f], 6)
        e["timbre2"] = {f: round(p["t2"][f], 6) for f in TIMBRE_FIELDS}
        entries.append(e)
    return json.dumps({"presets": entries}, indent=2, ensure_ascii=True)


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"Wrote {path}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    syx_file = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "tools", "FactoryBackUpDoResetAfter.syx")

    print(f"=== Parsing microKORG presets from {syx_file} ===")
    presets = [parse_program(i, prog) for i, prog in enumerate(load_programs(syx_file))]

    # Every float must be a finite value in [0, 1].
    for p in presets:
        for key in ("t1", "t2", "fx"):
            for name, v in p[key].items():
                assert 0.0 <= v <= 1.0, f"{p['label']} {key}.{name} = {v} out of range"

    header = render_header(presets)
    for rel in ("presets.h", "dsp/presets.h", "src/presets.h", "src/dsp/presets.h"):
        write_text(os.path.join(repo_root, rel), header)

    js = render_json(presets)
    for rel in ("presets.json", "src/presets.json"):
        write_text(os.path.join(repo_root, rel), js)

    named = sum(p["name_from_syx"] for p in presets)
    counts = {m: sum(p["mode"] == m for p in presets) for m in ("single", "layer", "vocoder")}
    print(f"\n{len(presets)} presets: {counts}; {named} names read from the dump, "
          f"{len(presets) - named} from FACTORY_LABELS")
    if named == 0:
        print("NOTE: the dump contains no patch names (all 12 name bytes are spaces); "
              "labels come from FACTORY_LABELS.")

    print("\nFirst 8 presets:")
    for i, p in enumerate(presets[:8]):
        print(f"  [{i}] {p['label']:24s} {p['mode']:7s} | T1 cut {p['t1']['cutoff']:.3f} res {p['t1']['resonance']:.3f}"
              f" | T2 cut {p['t2']['cutoff']:.3f} res {p['t2']['resonance']:.3f}")


if __name__ == "__main__":
    main()
