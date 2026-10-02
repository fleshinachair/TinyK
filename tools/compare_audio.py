#!/usr/bin/env python3
"""
Compare TinyK renders against hardware reference audio.

  compare_audio.py slice   # cut tools/reference/korg_ref.wav into the reference slices
  compare_audio.py compare # print metrics + % difference for each patch

Metrics (computed from the first onset, over a fixed analysis window):
  centroid  - mean spectral centroid in Hz (brightness / filter opening)
  decay     - slope of the RMS envelope in dB/s (more negative = more percussive)
  sustain   - level late in the window relative to the peak, in dB
  peak      - peak level in dBFS
  sub200    - fraction of energy below 200 Hz
  f0        - estimated pitch, so centroid differences can be read against pitch differences
"""
import os
import sys

import numpy as np
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
REF_DIR = os.path.join(HERE, "reference")
# "matched_note" is the MIDI note nearest the reference's strongest spectral peak,
# rendered as rendered_<key>_matched.wav next to the plain note-60 render.
SLICES = {
    "a11": {"start": 2.0, "end": 4.0, "label": "A.11 Saw Lead", "matched_note": "59+66"},
    "a12": {"start": 10.0, "end": 12.0, "label": "A.12 Trance Bass", "matched_note": 33},
}
WINDOW_S = 1.5       # analysis window after the onset
FFT_N, HOP = 4096, 512


def read_mono(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    x /= 32768.0 if x.dtype != np.float32 else 1.0
    return sr, (x.mean(axis=1) if x.ndim == 2 else x), x


def slice_reference():
    sr, _, stereo = read_mono(os.path.join(REF_DIR, "korg_ref.wav"))
    raw = wavfile.read(os.path.join(REF_DIR, "korg_ref.wav"))[1]
    for key, s in SLICES.items():
        seg = raw[int(s["start"] * sr):int(s["end"] * sr)]
        out = os.path.join(REF_DIR, f"ref_{key}.wav")
        wavfile.write(out, sr, seg)
        print(f"wrote {out} ({s['start']}-{s['end']}s, {s['label']})")


def onset_index(x, sr):
    """First 10 ms frame within 25 dB of the slice's loudest frame."""
    n = int(0.01 * sr)
    env = np.array([np.sqrt(np.mean(x[i:i + n] ** 2)) for i in range(0, len(x) - n, n)])
    thresh = env.max() * 10 ** (-25 / 20)
    return int(np.argmax(env > thresh)) * n


def estimate_f0(x, sr):
    """Strongest spectral peak between 30 Hz and 1 kHz (the dominant pitch, not necessarily the lowest)."""
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    freqs = np.fft.rfftfreq(len(x), 1 / sr)
    band = (freqs >= 30) & (freqs <= 1000)
    return float(freqs[band][np.argmax(spec[band])])


def metrics(path):
    sr, x, _ = read_mono(path)
    peak_db = 20 * np.log10(np.abs(x).max() + 1e-12)
    x = x[onset_index(x, sr):]
    x = x[: int(WINDOW_S * sr)]

    # Spectral centroid, power-weighted, over frames within 40 dB of the loudest frame.
    win = np.hanning(FFT_N)
    cents, powers = [], []
    for i in range(0, len(x) - FFT_N, HOP):
        spec = np.abs(np.fft.rfft(x[i:i + FFT_N] * win)) ** 2
        p = spec.sum()
        if p > 0:
            cents.append((np.fft.rfftfreq(FFT_N, 1 / sr) * spec).sum() / p)
            powers.append(p)
    cents, powers = np.array(cents), np.array(powers)
    keep = powers > powers.max() * 1e-4
    centroid = float(np.average(cents[keep], weights=powers[keep]))

    # RMS envelope in 20 ms frames
    n = int(0.02 * sr)
    rms = np.array([np.sqrt(np.mean(x[i:i + n] ** 2)) for i in range(0, len(x) - n, n)])
    env_db = 20 * np.log10(rms + 1e-9)
    t = np.arange(len(env_db)) * 0.02
    pk = int(np.argmax(env_db))
    seg = slice(pk, min(len(env_db), pk + int(1.2 / 0.02)))
    decay = float(np.polyfit(t[seg], env_db[seg], 1)[0])
    late = env_db[min(len(env_db) - 1, pk + int(1.0 / 0.02)):][:25]
    sustain = float(late.mean() - env_db[pk]) if len(late) else float("nan")

    full = np.abs(np.fft.rfft(x)) ** 2
    freqs = np.fft.rfftfreq(len(x), 1 / sr)
    sub = float(full[freqs < 200].sum() / full.sum())

    return {"centroid": centroid, "decay": decay, "sustain": sustain,
            "peak": float(peak_db), "sub200": sub, "f0": estimate_f0(x, sr)}


def pct(a, b):
    return (a - b) / abs(b) * 100.0 if b else float("nan")


def compare():
    rows = []
    variants = [(k, s, "") for k, s in SLICES.items()] + [(k, s, "_matched") for k, s in SLICES.items()]
    for key, s, suffix in variants:
        ref_path = os.path.join(REF_DIR, f"ref_{key}.wav")
        ren_path = os.path.join(HERE, f"rendered_{key}{suffix}.wav")
        if not (os.path.exists(ref_path) and os.path.exists(ren_path)):
            print(f"skip {key}: missing {ref_path} or {ren_path}")
            continue
        ref, ren = metrics(ref_path), metrics(ren_path)
        note = s["matched_note"] if suffix else 60
        print(f"\n=== {s['label']} (render: MIDI note {note}) ===")
        print(f"{'metric':10s} {'reference':>12s} {'render':>12s} {'diff':>10s}")
        for m, unit in (("centroid", "Hz"), ("decay", "dB/s"), ("sustain", "dB"),
                        ("peak", "dBFS"), ("sub200", "frac"), ("f0", "Hz")):
            print(f"{m:10s} {ref[m]:12.2f} {ren[m]:12.2f} {pct(ren[m], ref[m]):+9.1f}%  {unit}")
        rows.append((key + suffix, pct(ren["centroid"], ref["centroid"]), ref["f0"], ren["f0"]))
    print("\nCentroid error: " + ", ".join(f"{k}={e:+.1f}%" for k, e, _, _ in rows))
    for k, _, rf, nf in rows:
        if abs(np.log2(nf / rf)) > 0.5:
            print(f"WARNING {k}: pitch differs by {np.log2(nf / rf) * 12:+.1f} semitones "
                  f"(ref {rf:.0f} Hz vs render {nf:.0f} Hz) - centroid is not comparable")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "compare"
    {"slice": slice_reference, "compare": compare}[cmd]()
