#!/usr/bin/env python3
"""One program through KORG's microKORG plug-in and through the engine, the same MIDI into both: WAV files to
listen to, and the differences in numbers.

    python tools/ab_program.py BANK.syx A.11 [--out DIR] [--dry] [--arp]

Plays a short phrase (C3, C4, a C major chord, C5, then a retriggered C3). Writes <DIR>/<code>_plugin.wav,
<code>_tinyk.wav (the engine, brought to the plug-in's level) and <code>_ab.wav (each phrase from the plug-in,
then from the engine). The program's own arpeggiator is off unless --arp. Needs what tools/vst_ab.py needs."""
import argparse
import os
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
import vst_ab
from extracts_presets import load_programs, parse_program

SR = vst_ab.SR
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (start, length, notes)
PHRASES = [("C3", 0.0, 2.0, [48]), ("C4", 3.5, 2.0, [60]), ("chord", 7.0, 3.0, [48, 52, 55, 60]), ("C5", 11.5, 2.0, [72]),
           ("C3 again", 15.0, 0.4, [48]), ("C3 again", 15.6, 0.4, [48]), ("C3 again", 16.2, 1.5, [48])]
TOTAL = 20.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bank")
    ap.add_argument("code", help="A.11 .. B.88")
    ap.add_argument("--out", default=os.path.join(ROOT, "output", "ab"))
    ap.add_argument("--dry", action="store_true", help="delay, Mod FX and EQ off in both")
    ap.add_argument("--arp", action="store_true", help="leave the program's arpeggiator as stored")
    ap.add_argument("--vst", default=vst_ab.DEFAULT_VST)
    args = ap.parse_args()
    c = args.code.upper().replace(".", "")
    idx = (0 if c[0] == "A" else 64) + (int(c[1]) - 1) * 8 + int(c[2]) - 1
    prog = bytearray(load_programs(args.bank)[idx])
    if args.dry:
        prog[21] = prog[24] = 0
        prog[27] = prog[29] = 64
    if not args.arp:
        prog[32] &= 0x7F
    prog = bytes(prog)
    patch = parse_program(idx, prog)
    print("%s, %s%s" % (patch["label"], patch["mode"], ", FX off" if args.dry else ""))

    vst = vst_ab.Vst(args.vst)
    vst.load_program(prog, arp=args.arp)
    vst.render(60, 0.05, 0.5)
    msgs, events = [], []
    for _, t, n, notes in PHRASES:
        for note in notes:
            msgs += [vst.Message("note_on", note=note, velocity=100, time=t), vst.Message("note_off", note=note, time=t + n)]
            events += [(t, 0x90, note, 100), (t + n, 0x80, note, 0)]
    msgs.sort(key=lambda m: m.time)
    a = vst.plugin(msgs, duration=TOTAL, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T
    eng = cal.Engine(cal.build_library(tempfile.mkdtemp(prefix="tinyk_ab_")))
    b = eng.render_midi(0, patch, events, TOTAL, stereo=True)
    gain = np.sqrt((a ** 2).mean() / ((b ** 2).mean() + 1e-20))
    print("the plug-in is %+.1f dB against the engine at unity headroom; the engine's file is brought to the plug-in's level" % (20 * np.log10(gain)))
    b = b * gain
    peak = max(np.abs(a).max(), np.abs(b).max(), 1e-9)
    norm = min(1.0, 0.89 / peak) if peak > 0.89 else 10 ** (-1 / 20) / peak
    os.makedirs(args.out, exist_ok=True)
    tag = c[0] + c[1:] + ("_dry" if args.dry else "")
    save = lambda name, x: wavfile.write(os.path.join(args.out, "%s_%s.wav" % (tag, name)), SR, np.clip(x * norm * 32767.0, -32768, 32767).astype(np.int16))
    save("plugin", a)
    save("tinyk", b)
    parts, seen = [], []
    spans = {}
    for name, t, n, _ in PHRASES:
        lo, hi = spans.get(name, (t, t + n))
        spans[name] = (min(lo, t), max(hi, t + n))
        if name not in seen: seen.append(name)
    for name in seen:
        lo, hi = spans[name]
        i0, i1 = int(lo * SR), int(min(TOTAL, hi + 1.3) * SR)
        parts += [a[i0:i1], np.zeros((int(0.25 * SR), 2)), b[i0:i1], np.zeros((int(0.6 * SR), 2))]
    save("ab", np.concatenate(parts))
    print("\n%-10s %6s %6s %6s %6s   (tone: rms dB over third-octave bands; env: level contour; level, width: dB)" % ("", "tone", "env", "level", "width"))
    for name in seen:
        lo, hi = spans[name]
        i0, i1 = int(lo * SR), int(min(TOTAL, hi + 1.0) * SR)
        d = vst_ab.compare_renders(a[i0:i1], b[i0:i1], hi - lo)
        print("%-10s %6.1f %6.1f %+6.1f %+6.1f" % (name, d["tone"], d["env"], d["level"], d["width"]))
    print("\nwrote %s_{plugin,tinyk,ab}.wav in %s" % (tag, os.path.relpath(args.out, ROOT)))


if __name__ == "__main__":
    main()
