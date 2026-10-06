#!/usr/bin/env python3
"""
Render demo WAVs of the reference presets with a filter-cutoff sweep, for auditioning without hardware.

    python tools/render_demo_sweeps.py                    # A11 A12 A14 A28 B11, 8th-note sequence, FX on
    python tools/render_demo_sweeps.py --held --dry       # one held note, chorus/delay off
    python tools/render_demo_sweeps.py --keys B11 --seconds 8 --bits 24

Each clip plays the patch decoded from tools/reference/mk_<key>.syx (with its own chorus/delay unless
--dry) while the cutoff knob of every playing timbre sweeps high -> low -> high on a raised-cosine
curve (--high -> --low -> --high). --high defaults to 1.0, or to 0.45 when a playing timbre is a
band-pass: the patch's own EG and LFO modulation add ~3 octaves on top of the knob (B11), so above
~0.5 the band sits at the 19 kHz ceiling and passes only the top partials. The knob is swept, not Hz, so the sweep is exponential in frequency
like turning the hardware knob. Layered patches sweep both timbres to the same value, which overrides
their individual cutoffs for the duration of the clip.

Output: output/demos/<key>_<name>_sweep.wav, 44.1 kHz stereo, 16- or 24-bit PCM. The engine is built
from src/dsp/dsp.c with the same compile flags as tools/calibrate_dsp.py.
"""
import argparse
import ctypes
import os
import shutil
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import calibrate_dsp as cal  # noqa: E402
from extracts_presets import FX_FIELDS, TIMBRE_FIELDS, parse_program  # noqa: E402

SR = cal.SR
OUT_DIR = os.path.join(cal.ROOT, "output", "demos")
PATTERN = [0, 0, 12, 0, 7, 0, 10, 12]  # semitones above the root, one per 8th note
BPF_HIGH = 0.45  # default sweep ceiling for band-pass patches (B11: 4.5 kHz centroid; 0.5 -> 6.7 kHz, 0.7 -> 18.6 kHz)
BPF = 2.0 / 3.0  # filter_type value of BPF12


def load_patch(key, slot, dry):
    _, prog = cal.load_program_syx(os.path.join(cal.REF_DIR, f"mk_{key.lower()}.syx"))
    patch = parse_program(slot, prog)
    if dry:
        patch["fx"] = {k: 0.0 for k in patch["fx"]}
    return patch


def note_events(root, seconds, held, bpm):
    """[(frame, note, velocity)] with velocity 0 = note off; the last 1 s is left for the release tail."""
    play = max(seconds - 1.0, 0.5)
    if held:
        return [(0, root, 100), (int(play * SR), root, 0)]
    step = 60.0 / bpm / 2.0  # 8th notes
    ev = []
    for i in range(int(play / step)):
        note = root + PATTERN[i % len(PATTERN)]
        ev.append((int(i * step * SR), note, 100))
        ev.append((int((i + 0.8) * step * SR), note, 0))  # 80% gate
    return sorted(ev, key=lambda e: (e[0], e[2]))  # offs before ons at the same frame


def uses_bandpass(patch):
    playing = ("t1", "t2") if patch["voice_mode"] >= 0.5 else ("t1",)
    return any(abs(patch[t]["filter_type"] - BPF) < 0.1 for t in playing)


def sweep_curve(seconds, low, high=1.0, points_per_s=200):
    """Cutoff knob: high -> low -> high over the played part (raised cosine), held at high in the tail."""
    n = int(seconds * points_per_s)
    t = np.linspace(0.0, 1.0, n)
    play = max(seconds - 1.0, 0.5) / seconds
    phase = np.clip(t / play, 0.0, 1.0)
    return (low + (high - low) * 0.5 * (1.0 + np.cos(2.0 * np.pi * phase))).astype(np.float32)


def render(lib, patch, slot, events, curve, seconds):
    frames = int(seconds * SR)
    out = np.zeros(frames * 2, dtype=np.float32)
    t1 = np.array([patch["t1"][f] for f in TIMBRE_FIELDS], dtype=np.float32)
    t2 = np.array([patch["t2"][f] for f in TIMBRE_FIELDS], dtype=np.float32)
    fx = np.array([patch["fx"][f] for f in FX_FIELDS], dtype=np.float32)
    ef = np.array([e[0] for e in events], dtype=np.int32)
    en = np.array([e[1] for e in events], dtype=np.int32)
    ev = np.array([e[2] for e in events], dtype=np.int32)
    fp = lambda a: a.ctypes.data_as(ctypes.POINTER(ctypes.c_float))
    ip = lambda a: a.ctypes.data_as(ctypes.POINTER(ctypes.c_int))
    n = lib.tinyk_render_patch_events(slot, int(patch["voice_mode"] >= 0.5), fp(t1), fp(t2), fp(fx),
                                      ip(ef), ip(en), ip(ev), len(events), fp(curve), len(curve), frames, fp(out))
    if n < 0:
        raise RuntimeError("render failed")
    return out.reshape(-1, 2)


def write_wav(path, x, bits):
    x = np.clip(x, -1.0, 1.0)
    if bits == 16:
        wavfile.write(path, SR, (x * 32767.0).astype(np.int16))
    else:  # 24-bit samples left-justified in int32: scipy writes these as a 32-bit PCM container
        wavfile.write(path, SR, (x * 8388607.0).astype(np.int32) << 8)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keys", nargs="+", default=[k for k, _ in cal.REFS], help="reference keys (default: all)")
    ap.add_argument("--seconds", type=float, default=6.0, help="clip length incl. 1 s release tail (default 6)")
    ap.add_argument("--held", action="store_true", help="one held note instead of the 8th-note sequence")
    ap.add_argument("--bpm", type=float, default=120.0, help="sequence tempo (default 120)")
    ap.add_argument("--low", type=float, default=0.15, help="cutoff knob at the bottom of the sweep (default 0.15)")
    ap.add_argument("--high", type=float, default=None,
                    help=f"cutoff knob at the top of the sweep (default 1.0, or {BPF_HIGH} for band-pass patches)")
    ap.add_argument("--dry", action="store_true", help="chorus and delay off")
    ap.add_argument("--bits", type=int, choices=(16, 24), default=16)
    args = ap.parse_args()

    slots = dict(cal.REFS)
    unknown = [k for k in args.keys if k not in slots]
    if unknown:
        sys.exit(f"unknown key(s) {unknown}; choose from {sorted(slots)}")
    os.makedirs(OUT_DIR, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="tinyk_demo_")
    try:
        lib = ctypes.CDLL(cal.build_library(tmp))
        lib.tinyk_render_patch_events.restype = ctypes.c_int
        if lib.tinyk_timbre_floats() != len(TIMBRE_FIELDS):
            sys.exit("presets.h / extractor field mismatch; rebuild presets.h")
        for key in args.keys:
            patch = load_patch(key, slots[key], args.dry)
            root = cal.NOTES.get(key, 48)
            events = note_events(root, args.seconds, args.held, args.bpm)
            high = args.high if args.high is not None else (BPF_HIGH if uses_bandpass(patch) else 1.0)
            x = render(lib, patch, slots[key], events, sweep_curve(args.seconds, args.low, high), args.seconds)
            name = patch["label"].split(" ")[-1]  # the export's name already carries the program code
            path = os.path.join(OUT_DIR, f"{key}_{name}_sweep.wav")
            write_wav(path, x, args.bits)
            peak = 20 * np.log10(np.abs(x).max() + 1e-12)
            print(f"{path}  ({'Layer' if patch['voice_mode'] >= 0.5 else 'Single'}, root note {root}, "
                  f"{'held' if args.held else 'sequence'}, sweep {high:.2f}->{args.low:.2f}->{high:.2f}, "
                  f"{'dry' if args.dry else 'FX on'}, peak {peak:.1f} dBFS)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
