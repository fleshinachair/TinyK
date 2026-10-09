#!/usr/bin/env python3
"""
Fit the engine's filter cutoff curve and filter-EG intensity mapping from a recorded microKORG sweep.

    python tools/fit_filter_sweep.py [--dir tools/reference/sweep] [--note 48] [--eg-cutoff 32]
    python tools/fit_filter_sweep.py --selftest     # render a sweep with the engine itself and recover its constants

Recordings (44.1 kHz WAV, one held note each, in --dir):
    cut_000.wav cut_016.wav cut_032.wav cut_048.wav cut_064.wav cut_080.wav cut_096.wav cut_112.wav cut_127.wav
        Init program, Single, Osc1 Saw only (osc2/noise off), LPF24, resonance 0, EG1 intensity 0,
        key track 0, no FX/arp; filter cutoff set to the number in the name.
    eg_p10.wav eg_p30.wav eg_p63.wav   (optional)
        The same, at cutoff --eg-cutoff, with EG1 intensity +10 / +30 / +63 and EG1 = attack 0, sustain 127
        (so the filter sits at the full EG offset while the note is held).
    Hold each note >= 1 s; all files must be played on the same key (--note) at the same recording level.

Method
  * Mono, aligned at the first 2 ms frame above -40 dBFS; the steady part (0.3 s after onset until the
    level drops at note-off) is analysed with a long Hann FFT.
  * The level of every harmonic of the played note is measured and divided by the same harmonic in
    cut_127.wav, which cancels the oscillator's own spectrum and the recording gain (cut_127 is treated
    as unfiltered over the harmonics where it is above its noise floor).
  * Each step's cutoff is the fc for which an LPF24 of two 2-pole sections (Q --stage-q, default 0.707,
    which fits the microKORG VST at res 0 and is what the engine's LPF24 uses too;
    --selftest checks the fit against the engine itself) best explains those relative levels (least squares in dB).
  * A step is reliable only when its fc lies inside the measured harmonic range and the model fits it
    within 3 dB rms; others are reported but not used in the curve fit. Harmonics within 10 dB of the
    noise floor, or more than 60 dB below the file's strongest, only bound the fit (export 24-bit or
    float WAVs if you can: undithered 16-bit adds distortion exactly on the harmonics).
  * Curve: log2(fc) = log2(cutoff_base_hz) + cutoff/127 * cutoff_octaves (linear regression), with the
    residuals and a quadratic term reported so curvature in the hardware mapping is visible.
  * EG: octave shift = log2(fc_eg / fc at --eg-cutoff), compared with intensity/63 to give env_octaves
    (linear) and a power-law exponent (shift = A * (int/63)^p) if the three points demand one.
"""
import argparse
import os
import shutil
import sys
import tempfile

import numpy as np
from scipy import optimize
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import calibrate_dsp as cal  # noqa: E402
from extracts_presets import VOCODER_CARRIER  # noqa: E402

SR = cal.SR
CUT_STEPS = [0, 16, 32, 48, 64, 80, 96, 112, 127]
EG_STEPS = [10, 30, 63]
STEADY_DB = 6.0          # frames within this of the loudest count as the held note
SNR_DB = 10.0            # a harmonic counts as measured only this far above the noise floor next to it
MAX_FIT_ERR_DB = 3.0     # a step whose levels the LPF24 model misses by more than this (rms) is not used
DYN_DB = 60.0            # ...and within this of the file's strongest harmonic (quantisation distortion lands ON harmonics)
VEL_FACTOR = 1.0 - 0.3 + 0.3 * 100 / 127   # engine EG depth scaling at default vel_sens 0.3, velocity 100


# --------------------------------------------------------------------------------------------
# analysis
# --------------------------------------------------------------------------------------------
def steady_segment(path):
    """The held part of the note: the longest run of 50 ms frames within STEADY_DB of the file's loudest
    frame, trimmed by 0.15 s at each end. Thresholds are relative to the file, so quiet exports work.
    Returns (segment, or None if shorter than 0.4 s; loudest frame level in dBFS)."""
    x = cal.load_mono(path)
    n = int(0.05 * SR)
    frames = x[: len(x) // n * n].reshape(-1, n)
    db = 20 * np.log10(np.sqrt((frames ** 2).mean(axis=1)) + 1e-12)
    sounding = db[db > -200]
    if not len(sounding):
        return None, -240.0
    # relative to the typical loud level, not the single loudest frame (a click at the strike would win)
    active = db > np.percentile(sounding, 80) - STEADY_DB
    best, start = (0, 0), None
    for i, a in enumerate(np.append(active, False)):
        if a and start is None:
            start = i
        elif not a and start is not None:
            best = max(best, (i - start, start))
            start = None
    length, first = best
    i0, i1 = (first + 3) * n, (first + length - 3) * n
    return (x[i0:i1] if i1 - i0 >= int(0.4 * SR) else None), float(db.max())


def harmonic_levels(seg, f0):
    """dB level of each harmonic below 18 kHz (peak within +-3 % of n * f0), and the noise floor next
    to it (median of the bins between this harmonic and the next)."""
    spec = 20 * np.log10(np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) + 1e-12)
    freqs = np.fft.rfftfreq(len(seg), 1 / SR)
    levels, floors = [], []
    for n in range(1, int(18000 / f0) + 1):
        m = (freqs > n * f0 * 0.97) & (freqs < n * f0 * 1.03)
        gap = (freqs > (n + 0.3) * f0) & (freqs < (n + 0.7) * f0)
        levels.append(spec[m].max())
        floors.append(np.median(spec[gap]))
    return np.arange(1, len(levels) + 1) * f0, np.array(levels), np.array(floors)


def refine_f0(seg, note):
    """Played fundamental near the nominal note (the hardware may be slightly detuned)."""
    nominal = 440.0 * 2 ** ((note - 69) / 12)
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1 / SR)
    m = (freqs > nominal * 0.94) & (freqs < nominal * 1.06)
    return float(freqs[m][np.argmax(spec[m])])


STAGE_Q = 0.7071  # Q of each of the two 2-pole sections (set by --stage-q; the engine at res 0 is 0.5)


def lp24_db(f, fc):
    """Two identical 2-pole low-pass sections of Q = STAGE_Q."""
    w = f / fc
    return -20.0 * np.log10((1.0 - w * w) ** 2 + (w / STAGE_Q) ** 2)


def fit_ref_fc(freqs, ref_db, usable):
    """cut_127 against an ideal saw (-20 log10 n) with free gain: its own LPF24 cutoff, or inf if it is
    open over the whole measured range."""
    f, r = freqs[usable], ref_db[usable]
    saw = -20 * np.log10(f / freqs[0])

    def err(log_fc):
        return float(np.var(r - saw - lp24_db(f, 2.0 ** log_fc)))

    grid = np.linspace(np.log2(200.0), np.log2(200000.0), 400)
    best = grid[np.argmin([err(g) for g in grid])]
    fc = 2.0 ** optimize.minimize_scalar(err, bounds=(best - 0.1, best + 0.1), method="bounded").x
    return fc if fc < 4 * f.max() else np.inf


def fit_fc(f, rel_db, measured, bound_db, ref_fc=np.inf):
    """fc (Hz) of the engine LPF24 that best explains rel_db (levels relative to cut_127, whose own
    cutoff is ref_fc). Harmonics not `measured` (within SNR_DB of the noise floor) only bound the model
    from above at bound_db. Returns (fc, rms error dB over measured harmonics, reliable)."""
    ref_resp = lp24_db(f, ref_fc) if np.isfinite(ref_fc) else 0.0

    def err(log_fc):
        model = lp24_db(f, 2.0 ** log_fc) - ref_resp
        e = np.where(measured, rel_db - model, np.maximum(0.0, model - bound_db))
        return float(np.mean(e ** 2))

    grid = np.linspace(np.log2(10.0), np.log2(40000.0), 400)
    best = grid[np.argmin([err(g) for g in grid])]
    res = optimize.minimize_scalar(err, bounds=(best - 0.1, best + 0.1), method="bounded")
    fc = 2.0 ** res.x
    model = lp24_db(f, fc) - ref_resp
    rms = float(np.sqrt(np.mean((rel_db - model)[measured] ** 2))) if measured.any() else float("nan")
    # reliable only if the cutoff is bracketed by measured harmonics (enough of them to see the slope)
    reliable = bool(measured.sum() >= 3 and f[measured].min() * 0.5 <= fc <= f[measured].max() / 1.5 and rms <= MAX_FIT_ERR_DB)
    return fc, rms, reliable


def measure(paths, note, ref_key):
    ref_seg, ref_peak = steady_segment(paths[ref_key])
    if ref_seg is None:
        sys.exit(f"{paths[ref_key]}: no held note found in the reference (most open) recording")
    f0 = refine_f0(ref_seg, note)
    freqs, ref_db, ref_floor = harmonic_levels(ref_seg, f0)
    usable = ref_db > np.maximum(ref_floor + SNR_DB, ref_db.max() - DYN_DB)
    usable &= np.cumprod(usable).astype(bool) | (np.arange(len(usable)) < 3)  # stop at the first harmonic lost in noise
    f = freqs[usable]
    ref_fc = fit_ref_fc(freqs, ref_db, usable)
    print(f"Played fundamental {f0:.1f} Hz (note {note}); {usable.sum()} harmonics up to "
          f"{f.max():.0f} Hz usable from {os.path.basename(paths[ref_key])}, whose own cutoff fits "
          + ("above the measured range (treated as open)" if not np.isfinite(ref_fc) else f"{ref_fc:.0f} Hz"))
    results = {}
    for key, path in paths.items():
        if key == ref_key:
            continue
        seg, peak = steady_segment(path)
        if seg is None:
            # no held tone: the filter pushed the note below the recording/VST floor. All that is known is
            # that the fundamental lost more than (reference level - this file's loudest frame).
            lost = ref_peak - peak if peak > -200 else 60.0
            results[key] = (f0 / 2 ** (max(lost, 20.0) / 24.0), float("nan"), False, "silent: fc below")
            continue
        _, lv, floor = harmonic_levels(seg, f0)
        rel = (lv - ref_db)[usable]
        limit = np.maximum(floor + SNR_DB, lv.max() - DYN_DB)
        measured = (lv > limit)[usable]
        bound = (limit - ref_db)[usable]
        drift = max(0.0, rel[:3].max())  # a step can't be louder than unfiltered: absorb small level drift
        results[key] = fit_fc(f, rel - drift, measured, bound - drift, ref_fc) + ("",)
    results[ref_key] = (ref_fc, 0.0, bool(np.isfinite(ref_fc) and ref_fc <= f.max() / 1.5), "reference")
    return results, f, f0


# --------------------------------------------------------------------------------------------
# fitting and report
# --------------------------------------------------------------------------------------------
def fit_curve(cut_fc):
    pts = sorted((c, v[0]) for c, v in cut_fc.items() if v[2])
    if len(pts) < 2:
        sys.exit("Fewer than 2 cutoff steps have a cutoff inside the measured range; cannot fit the curve.")
    if len(pts) == 2:
        print("WARNING: only 2 usable cutoff steps: the line goes exactly through them, nothing checks it.")
    x = np.array([c / 127.0 for c, _ in pts])
    y = np.log2([fc for _, fc in pts])
    slope, icept = np.polyfit(x, y, 1)
    resid = y - (icept + slope * x)
    quad = np.polyfit(x, y, 2) if len(pts) >= 4 else None
    return 2.0 ** icept, slope, resid, quad, pts


def fit_eg(eg_fc, base_fc):
    pts = sorted((i, np.log2(v[0] / base_fc)) for i, v in eg_fc.items() if v[2])
    if not pts:
        return None
    x = np.array([i / 63.0 for i, _ in pts])
    s = np.array([v for _, v in pts])
    linear = float((x * s).sum() / (x * x).sum())  # shift = A * x through the origin
    power = None
    if len(pts) >= 2 and np.all(s > 0):
        p, logA = np.polyfit(np.log(x), np.log(s), 1)
        power = (float(np.exp(logA)), float(p))
    return linear, power, pts


def report(cut_fc, eg_fc, eg_cutoff, current):
    print("\nCutoff steps (engine-equivalent LPF24 cutoff):")
    print(f"  {'knob':>4s} {'fc Hz':>9s} {'fit err dB':>10s}  used   engine now Hz")
    for c in sorted(cut_fc):
        fc, e, ok, note = cut_fc[c]
        now = current["cutoff_base_hz"] * 2 ** (c / 127 * current["cutoff_octaves"])
        shown = f"<{fc:8.0f}" if note.startswith("silent") else f"{fc:9.0f}"
        print(f"  {c:4d} {shown} {e:10.1f}  {'yes ' if ok else 'no  '}  {now:9.0f}   {note}")

    base, octs, resid, quad, pts = fit_curve(cut_fc)
    print(f"\nExponential fit over {len(pts)} steps: fc = {base:.2f} Hz * 2^(cutoff/127 * {octs:.3f})")
    for c, (fc, _, _, note) in sorted(cut_fc.items()):
        if note.startswith("silent"):
            pred = base * 2 ** (c / 127 * octs)
            print(f"  bound check: knob {c} is silent (fc < {fc:.0f} Hz); the fit predicts {pred:.0f} Hz "
                  f"-> {'consistent' if pred <= fc * 1.2 else 'INCONSISTENT'}")
    print("  residuals (octaves): " + " ".join(f"{c}:{r:+.2f}" for (c, _), r in zip(pts, resid)))
    if quad is not None:
        print(f"  quadratic term {quad[0]:+.2f} octaves at full scale "
              f"({'curved: a single exponential does not describe the knob' if np.abs(resid).max() > 0.25 else 'residuals < 0.25 octave: exponential is adequate'})")

    out = {"cutoff_base_hz": base, "cutoff_octaves": octs}
    if eg_fc:
        measured_base = eg_cutoff in cut_fc and cut_fc[eg_cutoff][2]
        ref_fc = cut_fc[eg_cutoff][0] if measured_base else base * 2 ** (eg_cutoff / 127 * octs)
        print(f"\nEG intensity (base cutoff {eg_cutoff} = {ref_fc:.0f} Hz, "
              f"{'measured' if measured_base else 'EXTRAPOLATED from the fitted curve'}):")
        for i in sorted(eg_fc):
            fc, e, ok, _ = eg_fc[i]
            print(f"  +{i:2d}: fc {fc:7.0f} Hz  shift {np.log2(fc / ref_fc):+.2f} octaves  fit err {e:.1f} dB"
                  f"{'' if ok else '  (outside measured range: not used)'}")
        eg = fit_eg(eg_fc, ref_fc)
        if eg:
            linear, power, _ = eg
            print(f"  linear: {linear:.2f} octaves at intensity 63")
            if power:
                print(f"  power law: shift = {power[0]:.2f} * (int/63)^{power[1]:.2f}"
                      f"{'  (strongly curved: linear env_octaves will be wrong at small intensities)' if power[1] < 0.7 else ''}")
            # the engine scales EG depth by its velocity response; compensate so a velocity-100 note matches
            out["env_octaves"] = linear / VEL_FACTOR

    print("\n/* ---- fitted filter constants (tinyk_tuning_t field = value) ---- */")
    for k, v in out.items():
        print(f"  {k:16s} = {cal.c_literal(v)}   (now {cal.c_literal(current[k])})")
    print("/* Paste into TINYK_TUNING_DEFAULTS in src/dsp/dsp.c (field order: "
          + ", ".join(cal.TUNING_ORDER) + "):")
    print(cal.tuning_defaults_line(out)[1] + " */")
    return out


# --------------------------------------------------------------------------------------------
# self-test: render the sweep with the engine and check the fit recovers its constants
# --------------------------------------------------------------------------------------------
def selftest(note, eg_cutoff):
    global STAGE_Q
    STAGE_Q = 0.7071  # the engine's LPF24 at resonance 0: two Butterworth stages (k = sqrt 2), like the VST
    truth =dict(cal.DEFAULTS, cutoff_base_hz=20.0, cutoff_octaves=9.5, env_octaves=5.0, cutoff_ceil_hz=13000.0)
    tmp = tempfile.mkdtemp(prefix="tinyk_sweep_")
    try:
        eng = cal.Engine(cal.build_library(tmp))
        eng.set_tuning(truth)
        eng.set_tuning({"cutoff_ceil_hz": 20000.0})  # let the top steps open fully, like an analog filter
        t = dict(VOCODER_CARRIER, wave1=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0,
                 filter_type=0.0, keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0,
                 attack2=0.0, sustain2=1.0, portamento=0.0, level=0.25, amp_level=33.0 / 128.0)  # low level: keep the filter's tanh input linear
        fx = {"chorus_mix": 0.0, "delay_time": 0.0, "delay_feedback": 0.0, "delay_mix": 0.0}

        def write(name, **over):
            patch = {"voice_mode": 0.0, "t1": dict(t, **over), "t2": dict(t), "fx": fx}
            x = eng.render_patch(0, patch, note, 1.6, 2.0)
            wavfile.write(os.path.join(tmp, name), SR, x.astype(np.float32))  # float: no quantisation distortion

        for c in CUT_STEPS:
            write(f"cut_{c:03d}.wav", cutoff=c / 127.0)
        for i in EG_STEPS:
            write(f"eg_p{i}.wav", cutoff=eg_cutoff / 127.0, env_int=0.5 + i / 126.0)
        print(f"Self-test: engine renders with cutoff_base_hz {truth['cutoff_base_hz']}, cutoff_octaves "
              f"{truth['cutoff_octaves']}, env_octaves {truth['env_octaves']}\n")
        got = run(tmp, note, eg_cutoff, truth)
        ok = (abs(np.log2(got["cutoff_base_hz"] / truth["cutoff_base_hz"])) < 0.25
              and abs(got["cutoff_octaves"] - truth["cutoff_octaves"]) < 0.4
              and abs(got.get("env_octaves", 0) - truth["env_octaves"]) < 0.6)
        print("\nSELFTEST " + ("PASSED" if ok else "FAILED"))
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(sweep_dir, note, eg_cutoff, current, cut_files=None, eg_files=None):
    """cut_files {knob: path} / eg_files {intensity: path}; default: cut_NNN.wav / eg_pNN.wav in sweep_dir."""
    paths = cut_files or {c: os.path.join(sweep_dir, f"cut_{c:03d}.wav") for c in CUT_STEPS}
    if eg_files is None:
        eg_files = {i: os.path.join(sweep_dir, f"eg_p{i}.wav") for i in EG_STEPS}
        eg_files = {i: p for i, p in eg_files.items() if os.path.exists(p)}
    missing = [p for p in list(paths.values()) + list(eg_files.values()) if not os.path.exists(p)]
    if missing:
        sys.exit("missing sweep recordings:\n  " + "\n  ".join(missing))
    ref_key = max(paths)
    all_paths = dict(paths)
    all_paths.update({("eg", i): p for i, p in eg_files.items()})
    res, _, _ = measure(all_paths, note, ref_key)
    cut_fc = {c: res[c] for c in paths}
    eg_fc = {i: res[("eg", i)] for i in eg_files}
    return report(cut_fc, eg_fc, eg_cutoff, current)


def parse_map(items, base_dir):
    out = {}
    for item in items:
        k, path = item.split("=", 1)
        out[int(k)] = path if os.path.isabs(path) or os.path.exists(path) else os.path.join(base_dir, path)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=os.path.join(HERE, "reference", "sweep"))
    ap.add_argument("--note", type=int, default=48, help="MIDI note that sounded (default 48; low notes give more harmonics)")
    ap.add_argument("--eg-cutoff", type=int, default=32, help="cutoff knob value used for the eg_p*.wav files (default 32)")
    ap.add_argument("--cut", action="append", default=[], metavar="KNOB=FILE",
                    help="cutoff step recording (repeat; relative to --dir); the highest knob is the reference")
    ap.add_argument("--eg", action="append", default=[], metavar="INT=FILE", help="EG intensity recording (repeat)")
    ap.add_argument("--stage-q", type=float, default=0.7071,
                    help="Q of each 2-pole section of the model LPF24 (default 0.7071: fits the microKORG VST at res 0)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    global STAGE_Q
    STAGE_Q = args.stage_q
    if args.selftest:
        return selftest(args.note, args.eg_cutoff)
    run(args.dir, args.note, args.eg_cutoff, cal.DEFAULTS,
        parse_map(args.cut, args.dir) or None, parse_map(args.eg, args.dir) if args.cut else None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
