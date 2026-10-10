"""Shared harness for the measurement scripts in this folder: KORG's microKORG plug-in and the engine set to the
same plain patch (one oscillator, open high-pass, no modulation, FX off). The scripts need what tools/vst_ab.py
needs; those that read a bank take it from $TINYK_BANK (default banks/microKORG_KorgUSA.syx)."""
import os, sys, tempfile
import numpy as np
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import calibrate_dsp as cal
import vst_ab
from extracts_presets import VOCODER_CARRIER, load_programs, parse_program
SR = vst_ab.SR
BANK = os.environ.get("TINYK_BANK", os.path.join(ROOT, "banks", "microKORG_KorgUSA.syx"))   # the reference bank
W1 = ["Saw", "Pulse", "Triangle", "Sine", "Vox", "DWGS", "Noise"]

def make_vst():
    v = vst_ab.Vst(vst_ab.DEFAULT_VST)
    v.set(modfx_depth=0.0, delay_depth=0.0, eq_lowgain=0.0, eq_higain=0.0, t1_osc1control1=0.0, t1_osc1control2=0.0,
          t1_osc1level=127.0, t1_osc2level=0.0, t1_noiselevel=0.0, t1_filtertype="12HPF", t1_cutoff=0.0, t1_resonance=0.0,
          t1_filteregint=0.0, t1_filterkbdtrack=0.0, t1_voiceassign="Mono", t1_vibratoint=0.0, t1_ampegattack=0.0,
          t1_ampegdecay=64.0, t1_ampegsustain=127.0, t1_ampegrelease=0.0, t1_distortion=False, t1_vpatch1int=0.0, t1_vpatch2int=0.0, t1_vpatch3int=0.0,
          t1_vpatch4int=0.0, t1_panpot="CNT", t1_osc1wave="Saw", t1_transpose=0.0, t1_tune=0.0, t1_portamento=0.0,
          t1_osc2modselect="Off", t1_osc2semitone=0.0, t1_osc2tune=0.0, t1_amplevel=127.0, t1_ampkbdtrack=0.0,
          t1_bendrange=2.0, pitch_bender=8192.0, modulation_wheel=0.0, arp_sw=False, t1_triggermode="Single")
    return v

def make_eng(extra_flags=()):
    tmp = tempfile.mkdtemp(prefix="tinyk_h_")
    if extra_flags:
        import subprocess
        out = os.path.join(tmp, "tinyk_cal.so")
        cmd = ["cc", "-O2", "-shared", "-fPIC", "-DTINYK_LIB", "-DTINYK_TUNING", "-DTINYK_OUTPUT_HEADROOM=1.0f", *extra_flags,
               "-I", os.path.join(ROOT, "src", "dsp"), os.path.join(ROOT, "tools", "test_render.c"),
               os.path.join(ROOT, "src", "dsp", "dsp.c"), "-lm", "-o", out]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        if r.returncode: sys.exit(r.stderr)
        return cal.Engine(out)
    return cal.Engine(cal.build_library(tmp))

def timbre(**kw):
    t = dict(VOCODER_CARRIER, wave1=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, keytrack=0.5, env_int=0.5, drive=0.0,
             attack1=0.0, sustain1=1.0, attack2=0.0, sustain2=1.0, release2=0.0, portamento=0.0, level=0.05, amp_level=33.0 / 128.0,
             filter_type=1.0, cutoff=0.0, resonance=0.0, osc1_ctrl1=0.0, osc1_ctrl2=0.0, assign=0.0,
             osc1_level=1.0, osc2_level=1.0 / 128.0)
    t.update(kw)
    return t

def patch(**kw):
    t = timbre(**kw)
    return {"voice_mode": 0.0, "t1": t, "t2": dict(t), "fx": {k: 0.0 for k in cal.FX_FIELDS}}

def hz(note): return 440.0 * 2 ** ((note - 69) / 12.0)

def spectrum(x, n=1 << 16):
    x = x[:n]
    w = np.blackman(len(x))
    return np.abs(np.fft.rfft(x * w, n)) / (w.sum() / 2.0), np.fft.rfftfreq(n, 1.0 / SR)

def harmonics(x, f0, top=20000.0, n=1 << 16):
    """(harmonic amplitudes, off-harmonic rms, harmonic rms) of a steady tone"""
    s, f = spectrum(x, n)
    k = np.arange(1, int(top / f0) + 1)
    bins = np.round(k * f0 * n / SR).astype(int)
    amp = np.array([s[max(b - 3, 0):b + 4].max() for b in bins])
    mask = np.ones(len(s), bool)
    for b in bins: mask[max(b - 8, 0):b + 9] = False
    mask[:int(30 * n / SR)] = False
    mask[f > 20000] = False
    # blackman: sum of power over mainlobe ~ ; use power sums
    hp = sum((s[max(b - 8, 0):b + 9] ** 2).sum() for b in bins)
    op = (s[mask] ** 2).sum()
    return amp, op, hp
