#!/usr/bin/env python3
"""Capture the 64 DWGS waveforms from KORG's microKORG plug-in and write src/dsp/dwgs_waves.h.

    python tools/capture_dwgs.py [--vst PATH] [--check]

Needs what tools/vst_ab.py needs (the plug-in, `pedalboard`, `mido`). --check only reports how the header on
disk compares with the plug-in, wave by wave.

Method: each wave is played at 110 Hz through the open high-pass (the flat path) and fitted, by least squares,
as harmonics of note / periods, amplitude and phase. `periods` is the smallest loop (1..8 periods of the note)
that leaves less than -28 dB unexplained: most waves repeat every period, a few only every 2 or 3 (they hold
partials between the note's harmonics). A saw through the same path gives the path's own gain per harmonic
(the plug-in's oscillators are brighter than a 1/n saw; the engine adds that tilt itself, after its filter),
and every wave is divided by it and scaled against the plug-in's full-level sine (the engine's unit of level):
the waves keep the plug-in's relative levels, which differ by up to 12 dB.

tools/trace_dwgs.py, the earlier source, digitised the same waves from scope plots of the hardware. The two
agree to about 1 dB on the plain waves (E.Piano1, Bass2, Organ2 ...) and on which waves are multi-period, but
the plots could not give the loop of Bell4 or the true levels, and several dense waves traced poorly.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vst_ab

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "dsp", "dwgs_waves.h")
SR = vst_ab.SR
NOTE, F0 = 45, 110.0
HARMONICS = 96            # per period of the note
MAX_PERIODS = 12


N_FFT = 1 << 16   # 1.49 s
_WIN = np.blackman(N_FFT)


def fit(x, periods, f0):
    """The partials on multiples of f0 / periods, read off one long windowed FFT (the window is centred on
    time 0 by rotating the frame, so every phase refers to the same instant). Returns (complex c with
    x ~ Re sum c_k e^{j k w t}, HARMONICS per period, and the share of the energy below 19 kHz that those
    partials do NOT explain, as an amplitude ratio)."""
    frame = np.fft.fftshift(x[:N_FFT] * _WIN)
    spec = np.fft.rfft(frame) / (_WIN.sum() / 2.0)
    power = np.abs(spec) ** 2
    hz = SR / N_FFT
    k = np.arange(1, int(19000.0 * periods / f0) + 1)
    bins = np.round(k * f0 / periods / hz).astype(int)
    c = np.zeros(k.size, complex)
    taken = np.zeros(power.size, bool)
    for i, b in enumerate(bins):
        j = b - 2 + int(np.argmax(power[b - 2:b + 3]))
        c[i] = spec[j]
        taken[j - 3:j + 4] = True                      # the Blackman main lobe
    below = np.arange(power.size) * hz < 19000.0
    resid = np.sqrt(power[below & ~taken].sum() / power[below].sum())
    out = np.zeros(HARMONICS * periods, complex)
    m = min(out.size, c.size)
    out[:m] = c[:m]
    return out, resid


def tune(x):
    """The note's exact frequency (the plug-in's pitch is a float), from the strongest low partials."""
    spec = np.abs(np.fft.rfft(x[:N_FFT] * _WIN))
    hz = SR / N_FFT
    est = []
    for h in range(1, 9):
        b = int(round(h * F0 / hz))
        j = b - 3 + int(np.argmax(spec[b - 3:b + 4]))
        a0, a1, a2 = np.log(spec[j - 1:j + 2] + 1e-12)
        est.append(((j + 0.5 * (a0 - a2) / (a0 - 2 * a1 + a2)) * hz / h, spec[j]))
    return float(np.average([e[0] for e in est], weights=[e[1] for e in est]))


def capture(vst):
    vst.set(modfx_depth=0.0, delay_depth=0.0, eq_lowgain=0.0, eq_higain=0.0, t1_osc1control1=0.0, t1_osc1control2=0.0,
            t1_osc1level=127.0, t1_osc2level=0.0, t1_noiselevel=0.0, t1_filtertype="12HPF", t1_cutoff=0.0, t1_resonance=0.0,
            t1_filteregint=0.0, t1_filterkbdtrack=0.0, t1_voiceassign="Mono", t1_vibratoint=0.0, t1_ampegattack=0.0,
            t1_ampegsustain=127.0, t1_distortion=False, t1_vpatch1int=0.0, t1_vpatch2int=0.0, t1_vpatch3int=0.0,
            t1_vpatch4int=0.0, t1_panpot="CNT")

    def take():
        vst.render(NOTE, 0.05, 0.2)
        return vst.render(NOTE, 2.0, 2.0)[int(0.3 * SR):]

    vst.set(t1_osc1wave="Saw")
    saw = take()
    f0 = tune(saw)
    saw_c, _ = fit(saw, 1, f0)
    path = np.abs(saw_c) * np.arange(1, HARMONICS + 1)       # a 1/n saw: what is left is the path
    path /= path[0]
    vst.set(t1_osc1wave="Sine")
    sine_c, _ = fit(take(), 1, f0)
    unit = 1.0 / np.abs(sine_c[0])                            # the engine's unit: a full-level sine's amplitude
    vst.set(t1_osc1wave="DWGS")
    waves = []
    for label in vst.plugin.parameters["t1_osc1dwgswave"].valid_values:
        vst.set(t1_osc1dwgswave=label)
        x = take()
        # the shortest loop that leaves under -28 dB unexplained (the plain waves leave -31 .. -57 dB at one
        # period, the multi-period ones -2 .. -16 dB until the right loop is reached); a wave no loop explains
        # that well (Bell4: its partials are not harmonics of anything this short) takes the best there is
        fits = [fit(x, p, f0) for p in range(1, MAX_PERIODS + 1)]
        good = [p for p, (_, r) in enumerate(fits, 1) if r <= 0.04]
        periods = good[0] if good else min(range(1, MAX_PERIODS + 1), key=lambda p: fits[p - 1][1])
        c, resid = fits[periods - 1]
        gain = np.interp(np.arange(1, HARMONICS * periods + 1) / periods, np.arange(1, HARMONICS + 1), path)
        c = c / gain * unit
        c *= np.exp(-1j * np.angle(c[np.argmax(np.abs(c))]) * np.arange(1, c.size + 1) / (np.argmax(np.abs(c)) + 1))
        waves.append((label.split(":", 1)[1], periods, c, resid))
    return waves


def read_header():
    import test_dwgs
    names, coefs, periods = test_dwgs.load_header()
    return [(n, p, c[:, 0] - 1j * c[:, 1]) for n, c, p in zip(names, coefs, periods)]


def compare(waves, old):
    rows = []
    for (name, periods, c, _), (_, op, oc) in zip(waves, old):
        if periods != op:
            rows.append((99.0, "%s: %d periods on disk, %d in the plug-in" % (name, op, periods)))
            continue
        a, b = 20 * np.log10(np.abs(c) + 1e-9), 20 * np.log10(np.abs(oc) + 1e-9)
        m = (a > a.max() - 40) | (b > b.max() - 40)
        d = np.maximum(a, a.max() - 50) - np.maximum(b, b.max() - 50)
        rows.append((float(np.sqrt(np.mean(d[m] ** 2))), "%s %.1f" % (name, np.sqrt(np.mean(d[m] ** 2)))))
    errs = [r[0] for r in rows]
    print("header against the plug-in, rms dB per wave: median %.2f, worst: %s"
          % (np.median(errs), ", ".join(r[1] for r in sorted(rows, reverse=True)[:6])))


def write_header(waves):
    q = 32767.0 / max(max(np.abs(c.real).max(), np.abs(c.imag).max()) for _, _, c, _ in waves)
    out = ["/* Generated by tools/capture_dwgs.py: the microKORG's 64 DWGS waveforms as Fourier series, captured from\n",
           " * KORG's microKORG plug-in (see the tool for the method). A table spans `periods` periods of the note and\n",
           " * holds harmonics 1..count of that span, as (cos, sin) int16 pairs; value * DWGS_COEF_SCALE is the\n",
           " * amplitude, on the scale where a full-level sine is 1.0 (the waves keep their relative levels). */\n",
           "#ifndef TINYK_DWGS_WAVES_H\n#define TINYK_DWGS_WAVES_H\n\n",
           "#define DWGS_WAVE_COUNT 64\n",
           "#define DWGS_HARMONICS %d      /* per period of the note */\n" % HARMONICS,
           "#define DWGS_MAX_PERIODS %d\n" % max(p for _, p, _, _ in waves),
           "#define DWGS_TOTAL_PERIODS %d\n" % sum(p for _, p, _, _ in waves),
           "#define DWGS_COEF_SCALE %.9ef\n\n" % (1.0 / q)]
    out.append("static const char *const DWGS_NAMES[DWGS_WAVE_COUNT] = {\n")
    for i in range(0, 64, 8):
        out.append("    " + ", ".join('"%s"' % w[0] for w in waves[i:i + 8]) + ",\n")
    out.append("};\n\n")
    for i, (name, periods, c, _) in enumerate(waves):
        vals = np.round(np.column_stack([c.real, -c.imag]).ravel() * q).astype(int)   # x = a cos + b sin
        out.append("static const short DWGS_COEF_%02d[%d] = { /* %s */\n" % (i + 1, vals.size, name))
        for j in range(0, vals.size, 24):
            out.append("    " + ",".join(str(v) for v in vals[j:j + 24]) + ",\n")
        out.append("};\n")
    out.append("\nstatic const struct { const short *coef; short periods, count; } DWGS_WAVES[DWGS_WAVE_COUNT] = {\n")
    for i, (_, periods, c, _) in enumerate(waves):
        out.append("    { DWGS_COEF_%02d, %d, %d },\n" % (i + 1, periods, c.size))
    out.append("};\n\n#endif\n")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vst", default=vst_ab.DEFAULT_VST)
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    waves = capture(vst_ab.Vst(args.vst))
    for i, (name, periods, c, resid) in enumerate(waves, 1):
        t = np.arange(4096) / 4096.0
        peak = np.abs(np.real(np.exp(2j * np.pi * np.outer(t * periods, np.arange(1, c.size + 1) / periods)) @ c)).max()
        print("%2d %-9s periods %d  unexplained %5.1f dB  rms %.3f  peak %.2f" % (
            i, name, periods, 20 * np.log10(resid + 1e-9), np.sqrt((np.abs(c) ** 2).sum() / 2), peak))
    compare(waves, read_header())
    if not args.check:
        write_header(waves)
        print("wrote %s" % os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
