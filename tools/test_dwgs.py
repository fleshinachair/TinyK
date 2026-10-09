#!/usr/bin/env python3
"""Checks the DWGS oscillator and its picker against src/dsp/dwgs_waves.h.

    python tools/test_dwgs.py        (exit code 0 = all pass)

Spectra: every wave is rendered through a neutral patch (Osc 1 only, the high-pass at cutoff 0, no drive / FX, low
level so the chain stays linear). The chain then scales each harmonic by one gain curve G(f) that is the
same for every wave, so |rendered| / |stored| must agree between waves: a wrong table, a wrong level or a
wrong loop length shows as a wave that departs from the others.
Also: the multi-period waves really play their sub-harmonics, a high note does not alias, and the picker
(dwgs_wave, dwgs_pick, dwgs_list, the Osc page's second knob) behaves over the host API.
"""
import ctypes
import json
import os
import re
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
from extracts_presets import VOCODER_CARRIER

ROOT = cal.ROOT
SR = cal.SR
failures = 0


def check(ok, what):
    global failures
    print("  [%s] %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        failures += 1


def load_header():
    s = open(os.path.join(ROOT, "src", "dsp", "dwgs_waves.h"), encoding="utf-8").read()
    scale = float(re.search(r"DWGS_COEF_SCALE ([0-9.e+-]+)f", s).group(1))
    names = re.findall(r'"([^"]+)"', s[s.index("DWGS_NAMES"):s.index("};", s.index("DWGS_NAMES"))])
    coefs = [np.array(m.split(","), dtype=float).reshape(-1, 2) * scale
             for m in re.findall(r"DWGS_COEF_\d+\[\d+\] = \{[^\n]*\n([^}]*?),?\s*\};", s)]
    periods = [int(p) for p in re.findall(r"\{ DWGS_COEF_\d+, (\d+), \d+ \}", s)]
    return names, coefs, periods


def harmonic_mags(x, f0, count):
    """Least-squares magnitudes of count sinusoids on multiples of f0 (refined: the engine's pitch is a float),
    over a whole number of periods. Returns (harmonic numbers, magnitudes, residual rms, signal rms)."""
    def solve(f):
        n = int(np.floor(len(x) * f / SR) * SR / f)
        t = np.arange(n) / SR
        k = np.arange(1, count + 1)
        k = k[k * f < 0.48 * SR]
        ph = 2 * np.pi * np.outer(t, k * f)
        A = np.hstack([np.cos(ph), np.sin(ph)])
        c = np.linalg.lstsq(A, x[:n], rcond=None)[0]
        return k, np.hypot(c[:k.size], c[k.size:]), np.sqrt(np.mean((x[:n] - A @ c) ** 2)), np.sqrt(np.mean(x[:n] ** 2))
    best = min((f0 * (1 + d) for d in np.linspace(-2e-5, 2e-5, 9)), key=lambda f: solve(f)[2])
    return solve(best)


def partial_mags(x, f0, count):
    """Partials on multiples of f0, read off one long Blackman-windowed FFT (a loop of 11 periods has a
    thousand of them, 20 Hz apart: too many for a least-squares fit). Returns (numbers, magnitudes, the share
    of the energy below 19 kHz that is NOT in those partials, as an amplitude ratio)."""
    n = 1 << 16
    win = np.blackman(n)
    spec = np.abs(np.fft.rfft(x[:n] * win)) / (win.sum() / 2.0)
    hz = SR / n
    k = np.arange(1, count + 1)
    k = k[k * f0 < 19000.0]
    mags, taken = np.zeros(k.size), np.zeros(spec.size, bool)
    for i, b in enumerate(np.round(k * f0 / hz).astype(int)):
        j = b - 2 + int(np.argmax(spec[b - 2:b + 3]))
        mags[i] = spec[j]
        taken[j - 3:j + 4] = True
    below = np.arange(spec.size) * hz < 19000.0
    return k, mags, np.sqrt((spec[below & ~taken] ** 2).sum() / (spec[below] ** 2).sum())


def test_spectra(eng, names, coefs, periods):
    print("Spectra (A3, every wave against the stored series)")
    timbre = dict(VOCODER_CARRIER, wave1=5.0 / 6.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0,
                  cutoff=0.0, filter_type=1.0,   # 12HPF at cutoff 0: the flat path
                  keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0,
                  attack2=0.0, sustain2=1.0, portamento=0.0, level=0.25, amp_level=100.0 / 128.0)
    fx = {f: 0.0 for f in cal.FX_FIELDS}
    note, f_note = 57, 220.0
    curves, sub, worst_resid = {}, {}, -200.0
    for w in range(64):
        patch = {"voice_mode": 0.0, "t1": dict(timbre, dwgs=w / 63.0), "t2": dict(timbre), "fx": fx}
        x = eng.render_patch(0, patch, note, 1.9, 2.0)[int(0.3 * SR):]
        p = periods[w]
        k, mag, resid = partial_mags(x, f_note / p, 96 * p)
        worst_resid = max(worst_resid, 20 * np.log10(resid + 1e-12))
        stored = np.hypot(coefs[w][:, 0], coefs[w][:, 1])[k - 1]
        strong = stored > 0.02 * stored.max()
        curves[w] = (k[strong] * f_note / p, 20 * np.log10(mag[strong] / stored[strong]))
        off = (k % p) != 0
        sub[w] = (mag[off] ** 2).sum() / (mag ** 2).sum()
    # the chain's gain curve: the median over the single-period waves, per harmonic of the note
    freqs = np.arange(1, 97) * f_note
    per_h = [[] for _ in freqs]
    for w in range(64):
        if periods[w] == 1:
            for f, g in zip(*curves[w]):
                per_h[int(round(f / f_note)) - 1].append(g)
    known = np.array([len(v) >= 5 for v in per_h])
    gain = np.array([np.median(v) if len(v) >= 5 else np.nan for v in per_h])
    worst = (0.0, "")
    for w in range(64):
        f, g = curves[w]
        ok = (f >= freqs[known][0]) & (f <= freqs[known][-1])
        dev = np.sqrt(np.mean((g[ok] - np.interp(f[ok], freqs[known], gain[known])) ** 2))
        if dev > worst[0]:
            worst = (dev, names[w])
    check(worst[0] < 0.8, "all 64 waves follow one gain curve: worst %.2f dB rms (%s)" % worst)
    check(worst_resid < -40, "nothing but the wave's own partials: worst residual %.1f dB" % worst_resid)
    multi = [w for w in range(64) if periods[w] > 1]
    check(all(sub[w] > 0.005 for w in multi) and all(sub[w] < 1e-6 for w in range(64) if periods[w] == 1),
          "multi-period waves play partials between the note's harmonics: "
          + ", ".join("%s %.0f%%" % (names[w], 100 * sub[w]) for w in multi))

    print("Aliasing (C7, 2093 Hz)")
    worst = (-200.0, "")
    for w in (0, 14, 18, 20, 27, 34, 59, 63):
        patch = {"voice_mode": 0.0, "t1": dict(timbre, dwgs=w / 63.0), "t2": dict(timbre), "fx": fx}
        x = eng.render_patch(0, patch, 96, 0.9, 1.0)[int(0.3 * SR):int(0.7 * SR)]
        p = periods[w]
        _, _, resid, rms = harmonic_mags(x, 440.0 * 2 ** ((96 - 69) / 12.0) / p, 96 * p)
        db = 20 * np.log10(resid / rms + 1e-12)
        if db > worst[0]:
            worst = (db, names[w])
    saw = {"voice_mode": 0.0, "t1": dict(timbre, wave1=0.0), "t2": dict(timbre), "fx": fx}
    x = eng.render_patch(0, saw, 96, 0.9, 1.0)[int(0.3 * SR):int(0.7 * SR)]
    _, _, resid, rms = harmonic_mags(x, 440.0 * 2 ** ((96 - 69) / 12.0), 96)
    check(worst[0] < -40, "off-harmonic energy at C7: worst %.1f dB (%s); the engine's saw: %.1f dB"
          % (worst + (20 * np.log10(resid / rms + 1e-12),)))


class Host:
    def __init__(self, lib_path, module_dir):
        self.lib = ctypes.CDLL(lib_path)
        self.lib.tinyk_dsp_v2_create.argtypes = [ctypes.c_char_p]
        self.lib.tinyk_dsp_v2_set.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib.tinyk_dsp_v2_get.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        if not self.lib.tinyk_dsp_v2_create(module_dir.encode()):
            sys.exit("create_instance failed")

    def put(self, key, val):
        self.lib.tinyk_dsp_v2_set(key.encode(), str(val).encode())

    def get(self, key):
        buf = ctypes.create_string_buffer(65536)
        n = self.lib.tinyk_dsp_v2_get(key.encode(), buf, len(buf))
        return None if n < 0 else buf.value.decode()

    def osc_knobs(self):
        return json.loads(self.get("ui_hierarchy"))["levels"]["osc"]["knobs"]

    def settle(self):
        """The host's poll: is_loading reads 1 once after a label change, then 0."""
        first = self.get("is_loading")
        return first, self.get("is_loading")


def test_picker(lib_path, names):
    print("Picker (host API)")
    tmp = tempfile.mkdtemp(prefix="tinyk_dwgs_")
    try:
        h = Host(lib_path, tmp)
        manifest = json.load(open(os.path.join(ROOT, "src", "module.json"), encoding="utf-8"))
        levels = manifest["capabilities"]["ui_hierarchy"]["levels"]
        h.put("voice_mode", "Single")
        h.put("wave1", "Saw")
        h.settle()
        check(h.osc_knobs() == levels["osc"]["knobs"] and h.osc_knobs()[1] == "osc1_ctrl1",
              "Wave 1 = Saw: the Osc page's second knob is Control 1")
        h.put("wave1", "DWGS")
        flags = h.settle()
        check(flags == ("1", "0") and h.osc_knobs()[1] == "dwgs_wave" and h.osc_knobs()[0] == "wave1" and h.osc_knobs()[2] == "osc1_ctrl2"
              and h.osc_knobs()[3:] == levels["osc"]["knobs"][3:],
              "Wave 1 = DWGS: is_loading pulses and the second knob becomes dwgs_wave (%s, %s)" % (flags, h.osc_knobs()))
        meta = {e["key"]: e for e in json.loads(h.get("chain_params"))}["dwgs_wave"]
        check(meta["type"] == "enum" and len(meta["options"]) == 64 and len(meta["short_options"]) == 64
              and meta["options"][41] == "42 Organ3" and meta["short_options"][41] == "42",
              "chain_params: dwgs_wave is an enum of 64 (%s / %s)" % (meta["options"][41], meta["short_options"][41]))
        got = []
        for val in ("41", "Organ3", "42 Organ3"):
            h.put("dwgs_wave", "0")
            h.put("dwgs_wave", val)
            got.append(h.get("dwgs_wave"))
        h.put("dwgs_wave", "99")
        got.append(h.get("dwgs_wave"))
        check(got == ["41", "41", "41", "63"], "dwgs_wave takes an index, a name or a list label, clamped: %s" % got)

        items = json.loads(h.get("dwgs_list"))
        # the host's renderer: label = item.label || item.name || `Item ${item.index}`; a pick writes String(item.index)
        check(len(items) == 64 and all(isinstance(it, dict) and it["index"] == i and it["label"] == "%d %s" % (i + 1, names[i])
                                       for i, it in enumerate(items)),
              "dwgs_list: 64 {index, label} objects, numbered 1..64 (%s ... %s)" % (items[0]["label"], items[-1]["label"]))
        h.put("wave1", "Saw")
        h.settle()
        h.put("dwgs_pick", str(items[56]["index"]))
        check(h.get("dwgs_wave") == "56" and h.get("wave1") == "5" and h.osc_knobs()[1] == "dwgs_wave",
              "a pick from the list selects the wave and turns Wave 1 to DWGS")
        check("dwgs_list" not in levels and any(p.get("key") == "dwgs_wave" for p in levels["osc"]["params"]),
              "module.json has the dwgs_wave knob on Osc and no separate DWGS list page")

        # per layer, and the knob follows the edited layer
        h.put("voice_mode", "Layer")
        h.put("timbre_edit", "1")
        h.put("wave1", "Square")
        h.put("dwgs_wave", "7")
        h.settle()
        l2 = (h.get("dwgs_wave"), h.osc_knobs()[1])
        h.put("timbre_edit", "0")
        h.settle()
        l1 = (h.get("dwgs_wave"), h.osc_knobs()[1])
        check(l2 == ("7", "osc1_ctrl1") and l1 == ("56", "dwgs_wave"), "each layer keeps its own wave and knob: L1 %s, L2 %s" % (l1, l2))
        meta = {e["key"]: e for e in json.loads(h.get("chain_params"))}["dwgs_wave"]
        check(meta.get("short_name") == "L1.DWGS", "Layer mode names the cell %s" % meta.get("short_name"))

        # slot state
        state = h.get("state")
        h.put("dwgs_wave", "3")
        h.put("wave1", "Saw")
        h.put("state", state)
        check(h.get("dwgs_wave") == "56" and h.get("wave1") == "5", "the slot state restores the wave")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    names, coefs, periods = load_header()
    assert len(names) == len(coefs) == len(periods) == 64
    tmp = tempfile.mkdtemp(prefix="tinyk_dwgs_lib_")
    try:
        lib = cal.build_library(tmp)
        test_spectra(cal.Engine(lib), names, coefs, periods)
        test_picker(lib, names)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%s" % ("ALL PASS" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
