#!/usr/bin/env python3
"""Checks the program EQ (program bytes 26-29): both decoders, the filter and the slot state.

    python tools/test_eq.py        (exit code 0 = all pass)

Needs no bank file: it writes a 128-program dump of its own with every EQ setting in it.
"""
import ctypes
import json
import os
import random
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
from extracts_presets import EQ_FIELDS, VOCODER_CARRIER, load_programs, parse_program

SR = cal.SR
LOW_HZ = [40, 50, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300,
          320, 340, 360, 380, 400, 420, 440, 460, 480, 500, 600, 700, 800, 900, 1000]
HI_HZ = [1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000, 3250, 3500, 3750, 4000, 4250, 4500,
         4750, 5000, 5250, 5500, 5750, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000]
failures = 0


def check(ok, what):
    global failures
    print("  [%s] %s" % ("PASS" if ok else "FAIL", what))
    if not ok:
        failures += 1


def pack_8to7(data):
    """Korg's packing: 7 data bytes become [their top bits][7 low-7-bit bytes]."""
    out = bytearray()
    for i in range(0, len(data), 7):
        group = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(group)))
        out.extend(b & 0x7F for b in group)
    return bytes(out)


def write_bank(path, rng):
    """128 Single programs: every Hi / Low frequency and gain appears, plus out-of-range bytes."""
    progs = []
    for i in range(128):
        p = bytearray(254)
        p[0:12] = ("EQ test %03d " % i).encode("ascii")
        for t in (38, 146):
            p[t + 3] = p[t + 4] = p[t + 5] = p[t + 6] = p[t + 13] = p[t + 14] = 64
            p[t + 16] = p[t + 25] = 127
            p[t + 20] = 100
            p[t + 22] = p[t + 23] = p[t + 24] = p[t + 26] = p[t + 28] = p[t + 29] = 64
            p[t + 36] = 127
            for n in range(4):
                p[t + 45 + 2 * n] = 64
        p[22] = i % 3 if i < 120 else 9   # delay type; 9 is out of range and clamps to L/R
        p[26], p[28] = i % 30, (i * 7) % 30
        p[27], p[29] = 64 + (i % 25) - 12, 64 + ((i * 11) % 25) - 12
        if i >= 124:   # out of range: clamped by both decoders
            p[26], p[27], p[28], p[29] = 40 + i, rng.choice([0, 127]), 200, rng.choice([30, 99])
        progs.append(bytes(p))
    body = pack_8to7(b"".join(progs))
    with open(path, "wb") as f:
        f.write(bytes([0xF0, 0x42, 0x30, 0x58, 0x4C]) + body + bytes([0xF7]))
    return progs


def shelf_db(f, f0, gain_db, high):
    """The engine's shelf evaluated at f: first order, the corner at f0 (bilinear, pre-warped); a cut mirrors the boost."""
    if gain_db == 0:
        return np.zeros_like(f)
    G, K = 10 ** (abs(gain_db) / 20.0), 1.0 / np.tan(np.pi * f0 / SR)
    n1, n0 = (G, 1.0) if high else (1.0, G)
    z = np.exp(-1j * 2 * np.pi * f / SR)
    boost = 20 * np.log10(np.abs(((n1 * K + n0) + (n0 - n1 * K) * z) / ((K + 1) + (1 - K) * z)))
    return boost if gain_db > 0 else -boost


def test_decoders(eng, tmp):
    print("Decoders (a synthesised bank)")
    rng = random.Random(7)
    path = os.path.join(tmp, "banks", "EqTest.syx")
    os.makedirs(os.path.dirname(path))
    write_bank(path, rng)
    ref = [parse_program(i, p) for i, p in enumerate(load_programs(path))]
    lib = eng.lib
    lib.tinyk_bank_scan.argtypes = [ctypes.c_char_p]
    lib.tinyk_dsp_bank_eq.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_float)]
    check(lib.tinyk_bank_scan(os.path.dirname(path).encode()) == 1, "the C loader accepts the bank")
    worst, seen = 0.0, set()
    for i in range(128):
        eq = np.zeros(5, np.float32)
        lib.tinyk_dsp_bank_eq(1, i, eq.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        want = np.array([ref[i]["eq"][f] for f in EQ_FIELDS] + [ref[i]["delay_type"]])
        worst = max(worst, float(np.abs(eq - want).max()))
        seen.add(tuple(np.round(want, 4)))
    check(worst < 1e-6, "syx_bank.h and extracts_presets.py agree on EQ and delay type for all 128 programs (max diff %.1e)" % worst)
    lo, hi = min(r["eq"]["hi_gain"] for r in ref), max(r["eq"]["hi_gain"] for r in ref)
    check((lo, hi) == (0.0, 1.0) and all(0.0 <= r["eq"][f] <= 1.0 for r in ref for f in EQ_FIELDS) and len(seen) > 100,
          "values stay in 0..1, out-of-range bytes clamp, %d distinct settings" % len(seen))
    eq = np.zeros(5, np.float32)
    flat = True
    for i in range(128):
        lib.tinyk_dsp_bank_eq(0, i, eq.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
        flat = flat and eq[1] == 0.5 and eq[3] == 0.5
    check(flat, "the built-in bank's EQ is flat (presets.h has no EQ data yet)")


def test_filter(eng):
    print("Filter (noise through the EQ against the designed shelves)")
    timbre = dict(VOCODER_CARRIER, wave1=1.0, osc1_ctrl1=1.0, osc1_ctrl2=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0, cutoff=1.0,
                  filter_type=0.0, keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0, attack2=0.0,
                  sustain2=1.0, portamento=0.0, level=0.1, amp_level=33.0 / 128.0)   # low: +12 dB of EQ must stay under the limiter's knee
    patch = {"voice_mode": 0.0, "t1": timbre, "t2": dict(timbre), "fx": {f: 0.0 for f in cal.FX_FIELDS}}

    def render(low_i, low_db, hi_i, hi_db):
        eq = {"hi_freq": hi_i / 29.0, "hi_gain": 0.5 + hi_db / 24.0, "low_freq": low_i / 29.0, "low_gain": 0.5 + low_db / 24.0}
        return eng.render_patch(0, dict(patch, eq=eq), 60, 3.9, 4.0)[int(0.2 * SR):int(3.8 * SR)]

    def spectrum(x):
        n = 4096
        seg = x[:len(x) // n * n].reshape(-1, n) * np.hanning(n)
        return (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0)

    base = render(0, 0, 0, 0)
    f = np.fft.rfftfreq(4096, 1.0 / SR)
    band = (f > 30) & (f < 18000)
    worst = (0.0, "")
    for low_i, low_db, hi_i, hi_db in ((9, 12, 0, 0), (9, -12, 0, 0), (0, 0, 12, 12), (0, 0, 12, -12),
                                       (0, 6, 29, -6), (29, -9, 0, 9), (14, 3, 20, 12)):
        got = 10 * np.log10(spectrum(render(low_i, low_db, hi_i, hi_db)) / spectrum(base))
        want = shelf_db(f, LOW_HZ[low_i], low_db, False) + shelf_db(f, HI_HZ[hi_i], hi_db, True)
        # the noise is the same sequence in every render, so the ratio is the EQ alone; smooth over 5 bins
        dev = np.convolve(got - want, np.ones(5) / 5, mode="same")[band]
        err = float(np.abs(dev).max())
        if err > worst[0]:
            worst = (err, "Low %d Hz %+d dB, Hi %d Hz %+d dB" % (LOW_HZ[low_i], low_db, HI_HZ[hi_i], hi_db))
    check(worst[0] < 0.5, "seven settings within %.2f dB of the design, 30 Hz - 18 kHz (worst: %s)" % worst)
    again = render(0, 0, 0, 0)
    off = render(7, 0, 22, 0)   # frequencies set, gains 0
    check(np.array_equal(base, again) and np.array_equal(base, off), "0 dB on both bands leaves the output bit-identical")
    hot = render(0, 12, 0, 12)
    check(np.isfinite(hot).all() and np.abs(hot).max() <= 1.0, "+12 dB on both bands stays finite and within full scale")


def test_host(lib_path, tmp):
    print("Host API (bank program, params, slot state)")
    lib = ctypes.CDLL(lib_path)
    lib.tinyk_dsp_v2_create.argtypes = [ctypes.c_char_p]
    lib.tinyk_dsp_v2_set.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lib.tinyk_dsp_v2_get.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    if not lib.tinyk_dsp_v2_create(tmp.encode()):
        sys.exit("create_instance failed")

    def put(k, v):
        lib.tinyk_dsp_v2_set(k.encode(), str(v).encode())

    def get(k):
        buf = ctypes.create_string_buffer(65536)
        return buf.value.decode() if lib.tinyk_dsp_v2_get(k.encode(), buf, len(buf)) >= 0 else None

    def eq():
        return [get(k) for k in ("eq_low_freq", "eq_low_gain", "eq_hi_freq", "eq_hi_gain")]

    check(eq() == ["0", "0", "0", "0"], "a built-in program loads a flat EQ: %s" % eq())
    put("bank_file", "1")
    put("preset", "40")   # program 40 of the test bank: Hi 10 / +3 dB, Low (40 * 7) % 30 = 10 / (440 % 25) - 12 = +3 dB
    check(eq() == ["10", "3", "10", "3"], "a bank program loads its EQ: %s" % eq())
    put("eq_hi_gain", "-7")
    put("eq_low_freq", "99")
    check(eq() == ["29", "3", "10", "-7"], "the EQ keys set and clamp: %s" % eq())
    state = get("state")
    put("preset", "0")
    put("state", state)
    check(eq() == ["29", "3", "10", "-7"] and all(k in json.loads(state) for k in ("eq_low_freq", "eq_hi_gain")),
          "the slot state restores an edited EQ: %s" % eq())
    old = json.loads(state)
    for k in ("eq_low_freq", "eq_low_gain", "eq_hi_freq", "eq_hi_gain"):
        del old[k]
    put("state", json.dumps(old))
    check(eq() == ["10", "3", "10", "3"], "a state saved before the EQ existed keeps the program's EQ: %s" % eq())


def main():
    tmp = tempfile.mkdtemp(prefix="tinyk_eq_")
    try:
        lib = cal.build_library(tmp)
        eng = cal.Engine(lib)
        test_filter(eng)
        test_decoders(eng, tmp)
        test_host(lib, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("\n%s" % ("ALL PASS" if not failures else "%d FAILED" % failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
