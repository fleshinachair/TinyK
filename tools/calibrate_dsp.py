#!/usr/bin/env python3
"""
Calibrate the TinyK engine's global transfer-function constants against recordings of a real
microKORG (tools/reference/ref_<A11|A12|A14|A21|A28|B11>_C3.wav).

    python tools/calibrate_dsp.py baseline            # score the engine as it is now
    python tools/calibrate_dsp.py identify            # does each reference's dump slot actually reproduce it?
    python tools/calibrate_dsp.py tune                # fit, validate leave-one-out, report
    python tools/calibrate_dsp.py tune --apply        # ...and write accepted constants into dsp.c
    options: --note KEY=N (played note), --exclude KEY (leave a reference out of the fit), --gate KEY=S

How it works
  * The engine is built as a shared library with -DTINYK_TUNING (tools/test_render.c) and called
    through ctypes, so a few hundred renders cost seconds. The shipped build is unchanged: the
    constants are compile-time there.
  * Reference and render are mixed to mono and aligned so t = 0 is the first 2 ms frame above
    -40 dBFS.
  * Each reference's gate (note-off time) is estimated from its own envelope: some are hard-cut
    ~1.2 s after the strike, so a fixed "sustain 1.0-1.8 s" window would only see the release.
  * Loss per patch = spectral + envelope + brightness (all roughly in dB):
      spectral    RMS dB difference of the mean 2048-point STFT power in 1/3-octave bands (63 Hz-16 kHz)
                  over the sustain window (bands > 55 dB below the loudest are ignored)
      envelope    mean |dB| difference of 20 ms RMS frames, averaged over attack / body / release
      brightness  6 dB per octave of spectral centroid and 85% rolloff error
  * The played octave is unknown (C3 is MIDI 48 or 60 depending on convention), so each reference's
    note is chosen once from {48, 60, 72} at the default constants and then frozen.
  * `identify` ranks every dump program against each reference. A reference whose assigned slot does not
    rank near the top is not evidence about the engine (the dump holds a different sound there), and
    should be excluded from fitting.
  * Recording gain is unknown, so ONE global level offset (median across patches) is estimated, not
    tuned. Relative loudness between patches still counts.
  * With 6 recordings and ~10 constants, overfitting is the main risk. A weak prior keeps constants
    near their defaults, and `tune` runs leave-one-out validation (fit on 5, score the 6th). Constants
    are only accepted if held-out loss improves.
"""
import argparse
import ctypes
import glob
import multiprocessing as mp
import os
import re
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from scipy import optimize
from scipy.io import wavfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF_DIR = os.path.join(HERE, "reference")
SR = 44100
NOTE = 60  # default played note; see NOTE_CANDIDATES
ONSET_DB = -40.0
STFT_N, STFT_HOP = 2048, 512
ENV_FRAME = int(0.020 * SR)
ENV_FLOOR_DB = -70.0
PRIOR_WEIGHT = 0.5

# reference file key -> preset index (A.11=0, A.12=1, A.14=3, A.21=8, A.28=15, B.11=64)
REFS = [("A11", 0), ("A12", 1), ("A14", 3), ("A21", 8), ("A28", 15), ("B11", 64)]
DEFAULT_GATE_S = 1.2

# tunable constants: name, lower bound, upper bound
PARAMS = [
    ("cutoff_base_hz", 6.0, 60.0),
    ("cutoff_octaves", 9.0, 12.0),
    ("env_depth_hz", 2000.0, 20000.0),
    ("env_octaves", 0.0, 8.0),  # linear: 0 = off
    ("res_damping_range", 1.2, 1.98),
    ("lp24_res_scale", 0.3, 1.0),
    ("drive_gain", 0.5, 8.0),
    ("attack_scale", 0.5, 2.0),
    ("decay_scale", 0.5, 2.0),
    ("release_scale", 0.5, 2.0),
    ("delay_send_scale", 0.1, 1.0),
]
LINEAR_PARAMS = {"env_octaves"}
NOTE_CANDIDATES = (48, 60, 72)
DEFAULTS = {"cutoff_base_hz": 15.0, "cutoff_octaves": 10.3, "env_depth_hz": 8000.0, "env_octaves": 0.0,
            "res_damping_range": 1.92,
            "lp24_res_scale": 0.7, "drive_gain": 3.5, "attack_scale": 1.0, "decay_scale": 1.0,
            "release_scale": 1.0, "delay_send_scale": 0.4}

BAND_CENTERS = 1000.0 * 2.0 ** (np.arange(-12, 13) / 3.0)  # ~63 Hz .. 16 kHz
BAND_EDGES = np.append(BAND_CENTERS / 2 ** (1 / 6), BAND_CENTERS[-1] * 2 ** (1 / 6))


# --------------------------------------------------------------------------------------------
# engine access
# --------------------------------------------------------------------------------------------
def find_compiler():
    for cc in ("gcc", "cc", "clang"):
        if shutil.which(cc):
            return [cc]
    for zig in glob.glob(os.path.join(ROOT, ".toolchain", "zig-*", "zig.exe")) + \
            glob.glob(os.path.join(ROOT, ".toolchain", "zig-*", "zig")):
        return [zig, "cc"]
    sys.exit("No C compiler found (gcc, cc, clang, or .toolchain/zig-*/zig).")


def build_library(out_dir):
    ext = ".dll" if os.name == "nt" else ".so"
    out = os.path.join(out_dir, "tinyk_cal" + ext)
    cmd = find_compiler() + ["-O2", "-shared", "-fPIC", "-DTINYK_LIB", "-DTINYK_TUNING",
                             "-I", os.path.join(ROOT, "src", "dsp"), os.path.join(HERE, "test_render.c"),
                             os.path.join(ROOT, "src", "dsp", "dsp.c"), "-lm", "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit("Library build failed:\n" + r.stderr)
    return out


class Engine:
    def __init__(self, lib_path):
        self.lib = ctypes.CDLL(lib_path)
        self.lib.tinyk_render.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_double,
                                          ctypes.POINTER(ctypes.c_float), ctypes.c_int]
        self.lib.tinyk_render.restype = ctypes.c_int
        self.lib.tinyk_set_tuning.argtypes = [ctypes.c_char_p, ctypes.c_double]
        self.lib.tinyk_set_tuning.restype = ctypes.c_int

    def set_tuning(self, params):
        for k, v in params.items():
            if not self.lib.tinyk_set_tuning(k.encode(), float(v)):
                raise KeyError(k)

    def render(self, preset, note, gate_s, total_s):
        frames = int(total_s * SR)
        buf = np.zeros(frames * 2, dtype=np.float32)
        n = self.lib.tinyk_render(preset, note, gate_s, total_s,
                                  buf.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), frames)
        if n < 0:
            raise RuntimeError("render failed")
        return buf.reshape(-1, 2).astype(np.float64).mean(axis=1)


# --------------------------------------------------------------------------------------------
# signal analysis
# --------------------------------------------------------------------------------------------
def load_mono(path):
    sr, x = wavfile.read(path)
    if sr != SR:
        sys.exit(f"{path}: expected {SR} Hz, got {sr}")
    x = x.astype(np.float64)
    if np.abs(x).max() > 2.0:
        x /= 32768.0
    return x.mean(axis=1) if x.ndim == 2 else x


def onset_index(x):
    """Start of the first 2 ms frame whose RMS is above ONSET_DB dBFS."""
    n = int(0.002 * SR)
    frames = x[: len(x) // n * n].reshape(-1, n)
    db = 20 * np.log10(np.sqrt((frames ** 2).mean(axis=1)) + 1e-12)
    hit = np.nonzero(db > ONSET_DB)[0]
    return int(hit[0]) * n if len(hit) else 0


def env_db(x):
    n = len(x) // ENV_FRAME
    frames = x[: n * ENV_FRAME].reshape(n, ENV_FRAME)
    return np.maximum(20 * np.log10(np.sqrt((frames ** 2).mean(axis=1)) + 1e-12), ENV_FLOOR_DB)


def estimate_gate(ref):
    """Note-off time from the reference's own envelope; default for percussive patches."""
    n = int(0.05 * SR)
    frames = ref[: len(ref) // n * n].reshape(-1, n)
    db = 20 * np.log10(np.sqrt((frames ** 2).mean(axis=1)) + 1e-12)
    mid = np.median(db[6:12])  # 0.3-0.6 s
    if mid < db.max() - 10.0:
        return DEFAULT_GATE_S, False  # decays from the strike: no sustained section to read a gate from
    below = np.nonzero(db[12:] < mid - 6.0)[0]
    return (12 + int(below[0])) * 0.05 if len(below) else DEFAULT_GATE_S, True


def avg_power(x, i0, i1):
    seg = x[i0:i1]
    if len(seg) < STFT_N:
        seg = np.pad(seg, (0, STFT_N - len(seg)))
    win = np.hanning(STFT_N)
    acc = np.zeros(STFT_N // 2 + 1)
    count = 0
    for i in range(0, len(seg) - STFT_N + 1, STFT_HOP):
        acc += np.abs(np.fft.rfft(seg[i:i + STFT_N] * win)) ** 2
        count += 1
    return acc / max(count, 1)


def band_db(power):
    freqs = np.fft.rfftfreq(STFT_N, 1 / SR)
    idx = np.digitize(freqs, BAND_EDGES) - 1
    out = np.full(len(BAND_CENTERS), -200.0)
    for b in range(len(BAND_CENTERS)):
        s = power[idx == b].sum()
        out[b] = 10 * np.log10(s + 1e-20)
    return out


def brightness(power):
    freqs = np.fft.rfftfreq(STFT_N, 1 / SR)
    m = (freqs >= 40) & (freqs <= 16000)
    f, p = freqs[m], power[m]
    centroid = float((f * p).sum() / (p.sum() + 1e-20))
    cum = np.cumsum(p)
    rolloff = float(f[np.searchsorted(cum, 0.85 * cum[-1])])
    return centroid, rolloff


def dominant_hz(x, i0, i1):
    seg = x[i0:i1]
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freqs = np.fft.rfftfreq(len(seg), 1 / SR)
    m = (freqs > 30) & (freqs < 2000)
    return float(freqs[m][np.argmax(spec[m])])


# --------------------------------------------------------------------------------------------
# reference preparation and loss
# --------------------------------------------------------------------------------------------
def prepare_patches(gate_overrides, note_overrides=None):
    patches = []
    for key, preset in REFS:
        path = os.path.join(REF_DIR, f"ref_{key}_C3.wav")
        if not os.path.exists(path):
            sys.exit(f"missing reference: {path}")
        x = load_mono(path)
        ref = x[onset_index(x):]
        gate, sustained = estimate_gate(ref)
        gate = gate_overrides.get(key, gate)
        i0, i1 = int(0.15 * SR), int(max(gate - 0.05, 0.45) * SR)
        i1 = min(i1, len(ref))
        pw = avg_power(ref, i0, i1)
        patches.append(dict(key=key, preset=preset, note=(note_overrides or {}).get(key, NOTE),
                            ref=ref, gate=gate, sustained=sustained,
                            win=(i0, i1), ref_band=band_db(pw), ref_bright=brightness(pw),
                            ref_env=env_db(ref), ref_hz=dominant_hz(ref, i0, i1),
                            ref_level=10 * np.log10(pw.sum() + 1e-20)))
    return patches


def render_patch(engine, p):
    tail = max(0.5, len(p["ref"]) / SR - p["gate"] + 0.25)
    sim_raw = engine.render(p["preset"], p["note"], p["gate"], p["gate"] + tail)
    return sim_raw[onset_index(sim_raw):]


def patch_loss(p, sim, gain_db):
    i0, i1 = p["win"]
    i1 = min(i1, len(sim))
    pw = avg_power(sim, i0, i1)
    sim_band = band_db(pw) + gain_db
    ref_band = p["ref_band"]
    floor = ref_band.max() - 55.0
    spectral = float(np.sqrt(np.mean((np.maximum(ref_band, floor) - np.maximum(sim_band, floor)) ** 2)))

    sc, sr_ = brightness(pw)
    rc, rr = p["ref_bright"]
    bright = 6.0 * (abs(np.log2(sc / rc)) + abs(np.log2(sr_ / rr))) / 2.0

    ref_env = p["ref_env"]
    sim_env = env_db(sim) + gain_db
    sim_env = np.maximum(sim_env, ENV_FLOOR_DB)
    if len(sim_env) < len(ref_env):
        sim_env = np.pad(sim_env, (0, len(ref_env) - len(sim_env)), constant_values=ENV_FLOOR_DB)
    d = np.abs(ref_env - sim_env[: len(ref_env)])
    fr = 0.020
    a_end, g_end = int(0.2 / fr), int(p["gate"] / fr)
    parts = [d[:a_end], d[a_end:g_end], d[g_end:]]
    envelope = float(np.mean([part.mean() for part in parts if len(part)]))

    return dict(spectral=spectral, envelope=envelope, bright=bright, total=spectral + envelope + bright,
                centroid=(rc, sc), rolloff=(rr, sr_), sim_hz=dominant_hz(sim, i0, i1),
                sim_level=10 * np.log10(pw.sum() + 1e-20))


def evaluate(engine, patches, params, fit_keys=None, score_keys=None):
    """Loss of `score_keys` patches with the level offset estimated on `fit_keys` patches."""
    engine.set_tuning(params)
    fit_keys = fit_keys or [p["key"] for p in patches]
    score_keys = score_keys or [p["key"] for p in patches]
    sims = {p["key"]: render_patch(engine, p) for p in patches if p["key"] in set(fit_keys) | set(score_keys)}

    offsets = []
    for p in patches:
        if p["key"] in fit_keys:
            i0, i1 = p["win"]
            lvl = 10 * np.log10(avg_power(sims[p["key"]], i0, min(i1, len(sims[p["key"]]))).sum() + 1e-20)
            offsets.append(p["ref_level"] - lvl)
    gain_db = float(np.median(offsets))

    results = {p["key"]: patch_loss(p, sims[p["key"]], gain_db) for p in patches if p["key"] in score_keys}
    return results, gain_db


def choose_notes(engine, patches, keep=()):
    """Pick each reference's played note from NOTE_CANDIDATES at the default constants (then frozen)."""
    engine.set_tuning(DEFAULTS)
    for p in patches:
        if p["key"] in keep:
            continue
        best = None
        for note in NOTE_CANDIDATES:
            p["note"] = note
            sim = render_patch(engine, p)
            i0, i1 = p["win"]
            lvl = 10 * np.log10(avg_power(sim, i0, min(i1, len(sim))).sum() + 1e-20)
            total = patch_loss(p, sim, p["ref_level"] - lvl)["total"]
            if best is None or total < best[0]:
                best = (total, note)
        p["note"] = best[1]


def identify(engine, patches):
    """Rank all 128 dump programs against each reference; returns {key: rank of the assigned slot}."""
    engine.set_tuning(DEFAULTS)
    ranks = {}
    print("\nIdentification: where does the dump program assigned to each reference rank among all 128?")
    for p in patches:
        scores = []
        for idx in range(128):
            best = None
            for note in NOTE_CANDIDATES:
                q = dict(p, preset=idx, note=note)
                sim = render_patch(engine, q)
                i0, i1 = p["win"]
                lvl = 10 * np.log10(avg_power(sim, i0, min(i1, len(sim))).sum() + 1e-20)
                total = patch_loss(p, sim, p["ref_level"] - lvl)["total"]
                if best is None or total < best[0]:
                    best = (total, note)
            scores.append((best[0], idx, best[1]))
        order = sorted(scores)
        rank = [i for _, i, _ in order].index(p["preset"]) + 1
        ranks[p["key"]] = rank
        top = ", ".join(f"[{i}]={sc:.1f}@{n}" for sc, i, n in order[:3])
        print(f"  {p['key']:4s} assigned slot {p['preset']:3d}: loss {scores[p['preset']][0]:5.1f} (note {scores[p['preset']][2]}) "
              f"rank {rank:3d}/128   best: {top}   median {np.median([x[0] for x in scores]):.1f}")
    return ranks


def prior_penalty(params):
    total = 0.0
    for n in DEFAULTS:
        total += (params[n] / 4.0) ** 2 if n in LINEAR_PARAMS else np.log(params[n] / DEFAULTS[n]) ** 2
    return PRIOR_WEIGHT * total


def vec_to_params(x):
    return {name: float(v if name in LINEAR_PARAMS else np.exp(v)) for (name, _, _), v in zip(PARAMS, x)}


def params_to_vec(params):
    return np.array([params[n] if n in LINEAR_PARAMS else np.log(params[n]) for n, _, _ in PARAMS])


def fit(engine, patches, fit_keys, maxfev):
    x0 = params_to_vec(DEFAULTS)
    bounds = optimize.Bounds(params_to_vec({n: lo for n, lo, _ in PARAMS}), params_to_vec({n: hi for n, _, hi in PARAMS}))

    def objective(x):
        params = vec_to_params(x)
        res, _ = evaluate(engine, patches, params, fit_keys, fit_keys)
        return sum(r["total"] for r in res.values()) + prior_penalty(params)

    out = optimize.minimize(objective, x0, method="Powell", bounds=bounds,
                            options={"maxfev": maxfev, "xtol": 1e-2, "ftol": 1e-3})
    return vec_to_params(out.x), out.nfev


# --------------------------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------------------------
def print_table(title, patches, results, gain_db):
    print(f"\n{title}   (global level offset {gain_db:+.1f} dB, estimated)")
    print(f"{'patch':6s} {'note':>4s} {'gate':>5s} {'spectral':>9s} {'envelope':>9s} {'bright':>7s} {'total':>7s}   "
          f"{'centroid ref/sim Hz':>20s}  {'rolloff ref/sim Hz':>19s}  {'f0 ref/sim':>11s}")
    tot = 0.0
    for p in patches:
        r = results[p["key"]]
        tot += r["total"]
        flag = "" if p["sustained"] else "*"
        print(f"{p['key']:6s} {p['note']:4d} {p['gate']:4.2f}{flag:1s} {r['spectral']:9.2f} {r['envelope']:9.2f} {r['bright']:7.2f} "
              f"{r['total']:7.2f}   {r['centroid'][0]:9.0f} /{r['centroid'][1]:8.0f}  "
              f"{r['rolloff'][0]:8.0f} /{r['rolloff'][1]:8.0f}  {p['ref_hz']:5.0f}/{r['sim_hz']:5.0f}")
    print(f"{'TOTAL':11s} {'':5s} {'':9s} {'':9s} {'':7s} {tot:7.2f}")
    return tot


def apply_to_source(params):
    path = os.path.join(ROOT, "src", "dsp", "dsp.c")
    src = open(path, encoding="utf-8", newline="").read()
    order = ["cutoff_base_hz", "cutoff_octaves", None, None, "env_depth_hz", "env_octaves", "res_damping_range", "lp24_res_scale",
             "drive_gain", "attack_scale", "decay_scale", "release_scale", None, "delay_send_scale"]
    m = re.search(r"#define TINYK_TUNING_DEFAULTS \{([^}]*)\}", src)
    if not m:
        sys.exit("TINYK_TUNING_DEFAULTS not found in dsp.c")
    old = [t.strip() for t in m.group(1).split(",")]
    new = [(f"{params[n]:.4g}" + ("f" if "." in f"{params[n]:.4g}" or "e" in f"{params[n]:.4g}" else ".0f")) if n else o
           for n, o in zip(order, old)]
    src = src.replace(m.group(0), "#define TINYK_TUNING_DEFAULTS { " + ", ".join(new) + " }")
    open(path, "w", encoding="utf-8", newline="").write(src)
    print("Updated TINYK_TUNING_DEFAULTS in src/dsp/dsp.c:", ", ".join(new))


# --------------------------------------------------------------------------------------------
# leave-one-out worker (separate process, own engine copy)
# --------------------------------------------------------------------------------------------
_W = {}


def _init_worker(lib_path, gate_overrides, notes):
    _W["engine"] = Engine(lib_path)
    _W["patches"] = prepare_patches(gate_overrides, notes)


def _loo_job(args):
    held, maxfev, fit_keys = args
    engine, patches = _W["engine"], _W["patches"]
    train = [k for k in fit_keys if k != held]
    params, nfev = fit(engine, patches, train, maxfev)
    base, _ = evaluate(engine, patches, DEFAULTS, train, [held])
    tuned, _ = evaluate(engine, patches, params, train, [held])
    return held, base[held]["total"], tuned[held]["total"], params, nfev


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["baseline", "identify", "tune"])
    ap.add_argument("--maxfev", type=int, default=500, help="max renders-sets per fit (default 500)")
    ap.add_argument("--apply", action="store_true", help="write accepted constants into src/dsp/dsp.c")
    ap.add_argument("--gate", action="append", default=[], metavar="KEY=SECONDS", help="override a reference's gate")
    ap.add_argument("--note", action="append", default=[], metavar="KEY=MIDI", help="force a reference's played note")
    ap.add_argument("--exclude", action="append", default=[], metavar="KEY", help="leave a reference out of fitting")
    args = ap.parse_args()
    gate_overrides = {k: float(v) for k, v in (g.split("=") for g in args.gate)}
    note_overrides = {k: int(v) for k, v in (g.split("=") for g in args.note)}

    tmp = tempfile.mkdtemp(prefix="tinyk_cal_")
    try:
        lib = build_library(tmp)
        engine = Engine(lib)
        patches = prepare_patches(gate_overrides, note_overrides)
        choose_notes(engine, patches, keep=note_overrides)
        print("Played notes:", ", ".join(f"{p['key']}={p['note']}" for p in patches))
        print("Reference gates (s):", ", ".join(f"{p['key']}={p['gate']:.2f}{'' if p['sustained'] else '*'}" for p in patches),
              " (* = decays from the strike, default gate used)")

        res, gain = evaluate(engine, patches, DEFAULTS)
        base_total = print_table("BASELINE (current engine constants)", patches, res, gain)
        if args.command == "baseline":
            return 0
        if args.command == "identify":
            identify(engine, patches)
            return 0

        fit_keys = [p["key"] for p in patches if p["key"] not in args.exclude]
        if len(fit_keys) < 3:
            sys.exit("Need at least 3 references to fit and validate.")
        print(f"\nFitting {len(PARAMS)} constants on {len(fit_keys)} references {fit_keys} (Powell, <= {args.maxfev} evals)...")
        params, nfev = fit(engine, patches, fit_keys, args.maxfev)
        res_t, gain_t = evaluate(engine, patches, params, fit_keys)
        tuned_total = print_table(f"TUNED ({nfev} evals)", patches, res_t, gain_t)
        print("\nconstant               default      tuned")
        for n, _, _ in PARAMS:
            print(f"  {n:20s} {DEFAULTS[n]:9.4g}  {params[n]:9.4g}")

        print("\nLeave-one-out validation (fit on all but one reference, score the held-out one):")
        with mp.Pool(min(6, os.cpu_count() or 2), initializer=_init_worker, initargs=(lib, gate_overrides, {p["key"]: p["note"] for p in patches})) as pool:
            loo = pool.map(_loo_job, [(k, args.maxfev, fit_keys) for k in fit_keys])
        print(f"  {'held-out':9s} {'default loss':>13s} {'tuned loss':>11s} {'change':>8s}")
        b_sum = t_sum = 0.0
        for held, b, t, _, _ in loo:
            print(f"  {held:9s} {b:13.2f} {t:11.2f} {t - b:+8.2f}")
            b_sum += b
            t_sum += t
        improved = sum(1 for _, b, t, _, _ in loo if t < b)
        print(f"  {'sum':9s} {b_sum:13.2f} {t_sum:11.2f} {t_sum - b_sum:+8.2f}   (better on {improved}/{len(loo)} held-out patches)")

        accept = (t_sum < b_sum * 0.97) and improved >= 4
        print(f"\nTraining loss {base_total:.2f} -> {tuned_total:.2f}; held-out loss {b_sum:.2f} -> {t_sum:.2f}.")
        print("ACCEPT: tuned constants generalize to unseen references." if accept else
              "REJECT: held-out loss does not improve convincingly; the fit is memorizing the recordings.")
        if args.apply:
            if accept:
                apply_to_source(params)
            else:
                print("--apply ignored: not writing rejected constants.")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    mp.freeze_support()
    sys.exit(main())
