#!/usr/bin/env python3
"""
A.21 Timbre 2 kick / hat traces, for fitting the Sine cross-mod (src/dsp/dsp.c, sine_xmod_depth / sine_xmod_ratio).

    python tools/a21_kick_trace.py [wav ...]

With no arguments it reads the VST takes in tools/reference/ that exist:
    ref_a21_timbre2_drum_c3.wav        the program as stored (Osc 1 Ctrl 1 / 2 = 1 / 13, Osc 2 semitone -24)
    ref_a21_take1_ctrl2_zero.wav       Ctrl 2 = 0: no LFO1 on the cross-mod depth
    ref_a21_take2_osc2_semi_zero.wav   Ctrl 2 = 13, Osc 2 semitone 0: the synced square runs a full cycle per sine cycle

Each take is Timbre 2 alone, C3 at 120 BPM, dry. Per beat (500 ms from the first onset) it prints the kick's pitch
every 15 ms over 0..240 ms (strongest spectral peak below 400 Hz in a 40 ms window), the kick and hat levels
(0..235 / 255..490 ms) and the hat's decay (255..330 vs 410..490 ms). TinyK renders compare the same way:

    zig cc -O2 -DTINYK_TUNING -Isrc/dsp tools/test_render.c src/dsp/dsp.c -lm -o build/test_render_tune
    build/test_render_tune A.21 out.wav 60 2.2 0.3 --param timbre_balance=1 --param delay_mix=0 --set xmod_semitones=24
"""

import os
import sys
import wave

import numpy as np

REF_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "reference")
DEFAULT_TAKES = ["ref_a21_timbre2_drum_c3.wav", "ref_a21_take1_ctrl2_zero.wav", "ref_a21_take2_osc2_semi_zero.wav"]
BEAT_S = 0.5
TRACE_TIMES = np.arange(0.0, 0.241, 0.015)


def load_mono(path):
    with wave.open(path) as w:
        sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        x = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(x >= 1 << 23, x - (1 << 24), x) / float(1 << 23)
    else:
        x = np.frombuffer(raw, {2: np.int16, 4: np.int32}[sw]) / float(1 << (8 * sw - 1))
    return sr, x.reshape(-1, ch).mean(axis=1)


def onset(m):
    return int(np.argmax(np.abs(m) > 0.01 * np.abs(m).max()))


def rms_db(v):
    return 10.0 * np.log10(np.mean(v ** 2) + 1e-20)


def peak_hz(seg, sr):
    """Strongest spectral peak below 400 Hz (Hann window, 16x zero padding)."""
    seg = seg - seg.mean()
    n = len(seg)
    spec = np.abs(np.fft.rfft(seg * np.hanning(n), 16 * n))
    f = np.fft.rfftfreq(16 * n, 1.0 / sr)
    spec[f > 400.0] = 0.0
    return f[np.argmax(spec)]


def analyze(path, beats=4):
    sr, m = load_mono(path)
    on = onset(m)
    win = int(0.04 * sr)
    rows = []
    for b in range(beats):
        s = on + int(b * BEAT_S * sr)
        if s + int(BEAT_S * sr) > len(m):
            break
        kick = m[s + int(0.005 * sr):s + int(0.235 * sr)]
        hat = m[s + int(0.255 * sr):s + int(0.49 * sr)]
        decay = rms_db(m[s + int(0.255 * sr):s + int(0.33 * sr)]) - rms_db(m[s + int(0.41 * sr):s + int(0.49 * sr)])
        trace = [peak_hz(m[s + int(t * sr):s + int(t * sr) + win], sr) for t in TRACE_TIMES]
        rows.append((rms_db(kick), rms_db(hat), decay, trace))
    return rows


def main():
    paths = sys.argv[1:] or [os.path.join(REF_DIR, f) for f in DEFAULT_TAKES if os.path.exists(os.path.join(REF_DIR, f))]
    if not paths:
        sys.exit("no takes found")
    print("kick pitch (Hz) at " + " ".join(f"{t * 1000:4.0f}" for t in TRACE_TIMES) + " ms")
    for path in paths:
        print(f"\n{os.path.basename(path)}")
        for b, (k, h, d, trace) in enumerate(analyze(path)):
            print(f"  beat {b}: " + " ".join(f"{v:4.0f}" for v in trace)
                  + f"   kick {k:6.1f} dB, hat {h - k:+5.1f} dB vs kick, hat decay {d:4.1f} dB")


if __name__ == "__main__":
    main()
