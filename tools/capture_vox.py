#!/usr/bin/env python3
"""Capture the Vox wave from KORG's microKORG plug-in and write src/dsp/vox_pulse.h.

    python tools/capture_vox.py [--vst PATH]

Needs what tools/vst_ab.py needs. What the plug-in's Vox wave is (measured): one fixed pulse per period of the
note, the same pulse at every pitch (its peak is the same size at 27, 55 and 110 Hz; a higher note only brings
the pulses closer, so they overlap and the harmonics trace the pulse's own spectrum: a formant that stays put).
Control 1 shortens the pulse, about an octave of formant per 32 steps (275 Hz at 0, 1.2 kHz at 64, above 5 kHz at
127), and changes its shape a little.

Method: at each of nine Control 1 settings the wave is played at 27.5 Hz through the open high-pass, one period
is averaged over every period of a 1.7 s render, and its spectrum is divided by what the ENGINE's path does to a
sine of each frequency (its brightness tilt and the high-pass's low corner, read off engine renders), so that the
engine, playing the table through the same path, gives the plug-in's output. Each pulse is stored on its own time
scale d (the time from its negative peak to the positive peak after it): 32 samples per d, starting 0.75 d before
the peak, 32 d long, in units of a full-level sine.
"""
import argparse
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
import vst_ab
from extracts_presets import VOCODER_CARRIER

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "dsp", "vox_pulse.h")
SR = vst_ab.SR
C1 = [0, 16, 32, 48, 64, 80, 96, 112, 127]
NOTE = 21                 # 27.5 Hz: a 36 ms period, longer than the longest pulse
BINS = 8192               # of one period
PER_D, PRE_D, SPAN_D = 32, 0.75, 32     # table samples per d, lead-in and length in d
N_FFT = 1 << 16


def peak_level(x, hz):
    spec = np.abs(np.fft.rfft(x[:N_FFT] * np.blackman(N_FFT)))
    b = int(round(hz * N_FFT / SR))
    return float(spec[max(1, b - 4):b + 5].max())


def exact_f0(x, nominal):
    spec = np.abs(np.fft.rfft(x[:N_FFT] * np.blackman(N_FFT)))
    hz = SR / N_FFT
    est = []
    for h in range(1, 13):
        b = int(round(h * nominal / hz))
        j = b - 2 + int(np.argmax(spec[b - 2:b + 3]))
        a0, a1, a2 = np.log(spec[j - 1:j + 2] + 1e-12)
        est.append(((j + 0.5 * (a0 - a2) / (a0 - 2 * a1 + a2)) * hz / h, spec[j]))
    return float(np.average([e[0] for e in est], weights=[e[1] for e in est]))


def engine_path():
    """(frequencies, gain) of the engine's open path for a sine, relative to 110 Hz"""
    eng = cal.Engine(cal.build_library(tempfile.mkdtemp(prefix="tinyk_vox_")))
    t = dict(VOCODER_CARRIER, wave1=3.0 / 6.0, osc_mix=0.0, noise_level=0.0, detune=0.5, keytrack=0.5, env_int=0.5, drive=0.0,
             attack1=0.0, sustain1=1.0, attack2=0.0, sustain2=1.0, portamento=0.0, level=0.05, amp_level=33.0 / 128.0,
             filter_type=1.0, cutoff=0.0, resonance=0.0, osc1_ctrl1=0.0, osc1_ctrl2=0.0)
    patch = {"voice_mode": 0.0, "t1": t, "t2": dict(t), "fx": {k: 0.0 for k in cal.FX_FIELDS}}
    notes = list(range(9, 128, 3)) + [127]
    hz = [440.0 * 2 ** ((n - 69) / 12.0) for n in notes]
    lev = [peak_level(eng.render_patch(0, patch, n, 2.0, 2.0)[int(0.4 * SR):], f) for n, f in zip(notes, hz)]
    ref = np.interp(np.log(110.0), np.log(hz), np.log(lev))
    return np.array(hz), np.exp(np.log(lev) - ref)


def capture(vst):
    vst.set(modfx_depth=0.0, delay_depth=0.0, eq_lowgain=0.0, eq_higain=0.0, t1_osc1control1=0.0, t1_osc1control2=0.0,
            t1_osc1level=127.0, t1_osc2level=0.0, t1_noiselevel=0.0, t1_filtertype="12HPF", t1_cutoff=0.0, t1_resonance=0.0,
            t1_filteregint=0.0, t1_filterkbdtrack=0.0, t1_voiceassign="Mono", t1_vibratoint=0.0, t1_ampegattack=0.0,
            t1_ampegsustain=127.0, t1_distortion=False, t1_vpatch1int=0.0, t1_vpatch2int=0.0, t1_vpatch3int=0.0,
            t1_vpatch4int=0.0, t1_panpot="CNT", t1_osc1wave="Sine")

    def take(note):
        vst.render(note, 0.05, 0.2)
        return vst.render(note, 2.0, 2.0)[int(0.3 * SR):]

    unit = peak_level(take(45), 110.0) / (np.blackman(N_FFT).sum() / 2.0)      # a full-level sine's amplitude
    path_hz, path_gain = engine_path()
    vst.set(t1_osc1wave="Vox")
    nominal = 440.0 * 2 ** ((NOTE - 69) / 12.0)
    pulses = []
    for c1 in C1:
        vst.set(t1_osc1control1=float(c1))
        x = take(NOTE)
        f0 = exact_f0(x, nominal)
        idx = (((np.arange(x.size) * f0 / SR) % 1.0) * BINS).astype(int)
        period = np.bincount(idx, weights=x, minlength=BINS) / np.maximum(np.bincount(idx, minlength=BINS), 1) / unit
        spec = np.fft.rfft(period)
        f = np.arange(spec.size) * f0
        gain = np.exp(np.interp(np.log(np.maximum(f, 1.0)), np.log(path_hz), np.log(path_gain)))
        gain = np.maximum(gain, 0.25)                     # never lift a band by more than 12 dB
        spec[1:] /= gain[1:]
        spec[f > 19000.0] = 0.0
        spec[0] = 0.0
        w = np.fft.irfft(spec, BINS)
        k = int(np.argmin(w))
        w = np.roll(w, -k)                                # the negative peak at 0
        dt = 1.0 / (f0 * BINS)
        d = int(np.argmax(w[:BINS // 8])) * dt            # to the positive peak after it
        w = w - np.median(w[BINS // 2:BINS * 7 // 8])     # the pulse sits on zero (its mean was removed)
        u = np.arange(SPAN_D * PER_D) / PER_D - PRE_D
        table = np.interp(u * d / dt, np.arange(-BINS // 4, BINS * 3 // 4), np.roll(w, BINS // 4))
        table[u > SPAN_D - PRE_D - 2] *= np.linspace(1.0, 0.0, int((u > SPAN_D - PRE_D - 2).sum()))   # fade the last 2 d
        table[:4] *= np.linspace(0.0, 1.0, 4)
        pulses.append((c1, d, table))
        print("c1 %3d: d %.4f ms (formant near %.0f Hz), peak %+.2f / %+.2f, area %.3f d" % (
            c1, 1000 * d, 0.5 / d, table.min(), table.max(), table.sum() / PER_D))
    return pulses


def write_header(pulses):
    q = 32767.0 / max(np.abs(t).max() for _, _, t in pulses)
    out = ["/* Generated by tools/capture_vox.py: the microKORG's Vox wave, captured from KORG's microKORG plug-in (see the\n",
           " * tool). One pulse per period of the note; VOX_PULSE[i] is the pulse at Control 1 = VOX_C1[i], sampled\n",
           " * VOX_PER_D times per VOX_D_S[i] seconds (its own time scale), starting VOX_PRE_D of them before its\n",
           " * negative peak; value * VOX_SCALE is the amplitude with a full-level sine at 1.0. VOX_AREA[i] is its\n",
           " * integral in units of d (what the pulse train's mean is made of). */\n",
           "#ifndef TINYK_VOX_PULSE_H\n#define TINYK_VOX_PULSE_H\n\n",
           "#define VOX_POINTS %d\n#define VOX_PER_D %d\n#define VOX_LEN %d\n#define VOX_SCALE %.9ef\n\n" % (
               len(pulses), PER_D, SPAN_D * PER_D, 1.0 / q),
           "static const float VOX_C1[VOX_POINTS] = { %s };\n" % ", ".join("%d" % c for c, _, _ in pulses),
           "static const float VOX_D_S[VOX_POINTS] = { %s };\n" % ", ".join("%.6ef" % d for _, d, _ in pulses),
           "static const float VOX_AREA[VOX_POINTS] = { %s };\n\n" % ", ".join("%.6ef" % (t.sum() / PER_D) for _, _, t in pulses),
           "static const short VOX_PULSE[VOX_POINTS][VOX_LEN] = {\n"]
    for c1, _, t in pulses:
        vals = np.round(t * q).astype(int)
        out.append("    { /* Control 1 = %d */\n" % c1)
        for j in range(0, vals.size, 24):
            out.append("        " + ",".join(str(v) for v in vals[j:j + 24]) + ",\n")
        out.append("    },\n")
    out.append("};\n\n#endif\n")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vst", default=vst_ab.DEFAULT_VST)
    args = ap.parse_args()
    write_header(capture(vst_ab.Vst(args.vst)))
    print("wrote %s" % os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
