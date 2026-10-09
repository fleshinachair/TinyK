#!/usr/bin/env python3
"""
Render every factory preset (MIDI note 60, 2 s) with tools/test_render.c and flag
patches that are silent, faint, click-only, or unstable.

    python tools/audit_all_presets.py            # audit all 128, exit 1 on any failure
    python tools/audit_all_presets.py 8 9 120    # audit selected preset indices

Categories:
  SILENT      peak < -45 dBFS (before the -6.9 dB output headroom)
  VERY QUIET  peak between -45 and -20 dBFS (same)
  CLICK ONLY  the signal stays within 30 dB of its peak for < 15 ms
  UNSTABLE    non-finite values inside the engine, DC offset > 0.05, or int16 rail hits
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NOTE, HOLD_S, TOTAL_S = 60, 1.5, 2.0
# The engine ships with TINYK_OUTPUT_HEADROOM 0.45 (-6.9 dB, src/dsp/dsp.c): the level thresholds below are
# stated at unity and moved by the same amount, so they keep meaning what they did.
HEADROOM_DB = -10.0   # TINYK_MIX_GAIN 0.35 x TINYK_OUTPUT_HEADROOM 0.9
SILENT_DB, QUIET_DB = -45.0 + HEADROOM_DB, -20.0 + HEADROOM_DB
CLICK_MS = 15.0
DC_LIMIT = 0.05


def find_compiler():
    """gcc/cc/clang if present, otherwise the repo-local zig toolchain."""
    for cc in ("gcc", "cc", "clang"):
        if shutil.which(cc):
            return [cc]
    for zig in glob.glob(os.path.join(ROOT, ".toolchain", "zig-*", "zig*")):
        if os.path.basename(zig).startswith("zig") and os.path.isfile(zig) and not zig.endswith((".md", ".txt")):
            return [zig, "cc"]
    sys.exit("No C compiler found (gcc, cc, clang, or .toolchain/zig-*/zig).")


def build_renderer(out_exe):
    cmd = find_compiler() + ["-O2", "-DTINYK_DIAG", "-I", os.path.join(ROOT, "src", "dsp"),
                              os.path.join(HERE, "test_render.c"), os.path.join(ROOT, "src", "dsp", "dsp.c"),
                              "-lm", "-o", out_exe]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit("Renderer build failed:\n" + r.stderr)


def analyze(path, nonfinite):
    sr, raw = wavfile.read(path)
    x = raw.astype(np.float64) / 32768.0
    mono = x.mean(axis=1)
    peak = np.abs(x).max()
    peak_db = 20 * np.log10(peak + 1e-12)
    problems = []

    if nonfinite:
        problems.append(f"UNSTABLE (nonfinite={nonfinite})")
    dc = np.abs(x.mean(axis=0)).max()
    if dc > DC_LIMIT:
        problems.append(f"UNSTABLE (DC {dc:.3f})")
    if (np.abs(raw) >= 32767).sum() > 4:
        problems.append("UNSTABLE (clipped to int16 rails)")

    if peak_db < SILENT_DB:
        problems.append("SILENT")
    else:
        if peak_db < QUIET_DB:
            problems.append("VERY QUIET")
        # time the 1 ms-smoothed envelope stays within 30 dB of its peak
        n = max(1, int(0.001 * sr))
        env = np.sqrt(np.convolve(mono ** 2, np.ones(n) / n, mode="same"))
        above = (env > env.max() * 10 ** (-30 / 20)).sum() / sr * 1000.0
        if above < CLICK_MS:
            problems.append(f"CLICK ONLY ({above:.1f} ms)")
    return peak_db, problems


def main():
    with open(os.path.join(ROOT, "presets.json"), encoding="utf-8") as f:
        labels = [p["label"] for p in json.load(f)["presets"]]
    indices = [int(a) for a in sys.argv[1:]] or list(range(128))

    tmp = tempfile.mkdtemp(prefix="tinyk_audit_")
    try:
        exe = os.path.join(tmp, "test_render" + (".exe" if os.name == "nt" else ""))
        build_renderer(exe)
        failures = []
        for i in indices:
            wav = os.path.join(tmp, f"p{i}.wav")
            r = subprocess.run([exe, str(i), wav, str(NOTE), str(HOLD_S), str(TOTAL_S - HOLD_S)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                failures.append((i, labels[i], float("nan"), [f"RENDER FAILED: {r.stderr.strip()}"]))
                continue
            nonfinite = int(r.stdout.rsplit("nonfinite=", 1)[1])
            peak_db, problems = analyze(wav, nonfinite)
            if problems:
                failures.append((i, labels[i], peak_db, problems))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"Audited {len(indices)} presets (note {NOTE}, {HOLD_S}s held of {TOTAL_S}s)")
    if not failures:
        print("PASS: all presets audible and stable")
        return 0
    print(f"FAIL: {len(failures)} presets\n")
    print(f"{'idx':>3}  {'label':28s} {'peak dBFS':>9}  problems")
    for i, label, pk, probs in failures:
        print(f"{i:3d}  {label:28s} {pk:9.1f}  {', '.join(probs)}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
