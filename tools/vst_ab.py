#!/usr/bin/env python3
"""A/B the engine against KORG's microKORG plug-in, rendered offline.

    python tools/vst_ab.py params            list the plug-in's parameters and their current values
    python tools/vst_ab.py eq                the EQ: the plug-in's response against the engine's, per setting
    python tools/vst_ab.py delay             the delay: repeat times, gains and sides per type, time and depth
    python tools/vst_ab.py filter            the filter: both responses per type over cutoff x resonance
    python tools/vst_ab.py programs BANK.syx every synth program of a bank through both, ranked by difference

Needs the plug-in (default /Library/Audio/Plug-Ins/VST3/microKORG.vst3, or --vst PATH) and, besides NumPy
and SciPy, the `pedalboard` and `mido` packages (pip install pedalboard mido; a virtual environment is fine).
The plug-in loads without a window and renders about 40x faster than real time. Its parameters carry the
hardware's own units (eq_lowfreq 40..1000, t1_cutoff 0..127, t1_osc1wave "Saw" ...), so a setting can be
written exactly as a program stores it. It is not bit-repeatable from one render to the next (two flat
noise renders differ by about 0.26 dB rms in 5-bin bands over 30 s): compare spectra, not samples.
"""
import argparse
import html
import os
import re
import struct
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calibrate_dsp as cal
from extracts_presets import VOCODER_CARRIER, load_programs, parse_program

SR = cal.SR
DEFAULT_VST = "/Library/Audio/Plug-Ins/VST3/microKORG.vst3"
LOW_HZ = [40, 50, 60, 80, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300,
          320, 340, 360, 380, 400, 420, 440, 460, 480, 500, 600, 700, 800, 900, 1000]
HI_HZ = [1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750, 3000, 3250, 3500, 3750, 4000, 4250, 4500,
         4750, 5000, 5250, 5500, 5750, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000]


# The plug-in's saved state is XML inside JUCE's own base-64 ("<size>.<text>", 6 bits per character, low bit
# first): <PluginState ProgramParameters="<Program> <PARAM id=... value=.../> ..."/>. The PARAM values are the
# hardware's (Timbre1FilterCutoff 0..127, Timbre1Transpose -24..24, VoiceMode ...), and they include what the
# host parameters leave out (VoiceMode, KeyboardOctave), so writing them back loads any program of a dump.
JUCE_B64 = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"


def juce_decode(text):
    size, data = text.split(".", 1)
    bits = "".join(format(JUCE_B64.index(c), "06b")[::-1] for c in data)
    return bytes(int(bits[i:i + 8][::-1], 2) for i in range(0, int(size) * 8, 8))


def juce_encode(blob):
    bits = "".join(format(b, "08b")[::-1] for b in blob)
    bits += "0" * (-len(bits) % 6)
    return "%d.%s" % (len(blob), "".join(JUCE_B64[int(bits[i:i + 6][::-1], 2)] for i in range(0, len(bits), 6)))


def attr_escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")
            .replace("\r", "&#13;").replace("\n", "&#10;"))


def program_state(prog, arp=False):
    """A 254-byte program (KORG's MIDI implementation, TABLE 1 / 2) as the plug-in's PARAM id -> value."""
    s8 = lambda b: b - 256 if b > 127 else b
    mode = (prog[16] >> 4) & 3
    out = {
        "VoiceMode": {0: 0, 2: 1, 3: 2}.get(mode, 0), "KeyboardOctave": s8(prog[37]),
        "DelayTempoSyncSw": prog[19] >> 7, "DelaySyncNote": prog[19] & 15, "DelayTime": prog[20], "DelayDepth": prog[21],
        "DelayType": min(prog[22], 2), "ModFxLFOSpeed": prog[23], "ModFxDepth": prog[24], "ModFxType": min(prog[25], 2),
        "EQHiFreq": min(prog[26], 29), "EQHiGain": prog[27] - 64, "EQLowFreq": min(prog[28], 29), "EQLowGain": prog[29] - 64,
        "ArpSw": (prog[32] >> 7) if arp else 0, "ModulationWheel": 0, "PitchBender": 8192,
    }
    for n, t in ((1, 38), (2, 146)):
        k = "Timbre%d" % n
        out.update({
            k + "AssignMode": min(prog[t + 1] >> 6, 2), k + "AmpEGReset": (prog[t + 1] >> 5) & 1,
            k + "FilterEGReset": (prog[t + 1] >> 4) & 1, k + "TriggerMode": (prog[t + 1] >> 3) & 1,
            k + "UnisonDetune": prog[t + 2], k + "Tune": prog[t + 3] - 64, k + "BendRange": prog[t + 4] - 64,
            k + "Transpose": prog[t + 5] - 64, k + "VibratoInt": prog[t + 6] - 64,
            k + "Osc1Wave": prog[t + 7] & 7, k + "Osc1WaveCtrl1": prog[t + 8], k + "Osc1WaveCtrl2": prog[t + 9],
            k + "Osc1DWGSWave": min(prog[t + 10], 63), k + "Osc2ModSelect": (prog[t + 12] >> 4) & 3,
            k + "Osc2Wave": min(prog[t + 12] & 3, 2), k + "Osc2Semitone": prog[t + 13] - 64, k + "Osc2Tune": prog[t + 14] - 64,
            k + "PortamentoTime": prog[t + 15] & 127, k + "Osc1Level": prog[t + 16], k + "Osc2Level": prog[t + 17],
            k + "NoiseLevel": prog[t + 18], k + "FilterType": prog[t + 19] & 3, k + "FilterCutoff": prog[t + 20],
            k + "FilterResonance": prog[t + 21], k + "FilterEGInt": prog[t + 22] - 64, k + "FilterKbdTrack": prog[t + 24] - 64,
            k + "AmpLevel": prog[t + 25], k + "AmpPanpot": prog[t + 26], k + "AmpDistortion": prog[t + 27] & 1,
            k + "AmpKbdTrack": prog[t + 29] - 64,
            k + "FilterEGAttack": prog[t + 30], k + "FilterEGDecay": prog[t + 31], k + "FilterEGSustain": prog[t + 32],
            k + "FilterEGRelease": prog[t + 33], k + "AmpEGAttack": prog[t + 34], k + "AmpEGDecay": prog[t + 35],
            k + "AmpEGSustain": prog[t + 36], k + "AmpEGRelease": prog[t + 37],
        })
        for i, base in ((1, t + 38), (2, t + 41)):
            out.update({"%sLFO%dWave" % (k, i): prog[base] & 3, "%sLFO%dKeySync" % (k, i): min((prog[base] >> 4) & 3, 2),
                        "%sLFO%dFrequency" % (k, i): prog[base + 1], "%sLFO%dTempoSync" % (k, i): prog[base + 2] >> 7,
                        "%sLFO%dSyncNote" % (k, i): min(prog[base + 2] & 31, 14)})
        for i in range(4):
            route = prog[t + 44 + 2 * i]
            out.update({"%sVPatch%dSource" % (k, i + 1): min(route & 15, 7), "%sVPatch%dDest" % (k, i + 1): min(route >> 4, 7),
                        "%sVPatch%dInt" % (k, i + 1): prog[t + 45 + 2 * i] - 64})
    return out


class Vst:
    def __init__(self, path):
        try:
            from pedalboard import load_plugin
            from mido import Message
        except ImportError:
            sys.exit("vst_ab.py needs the pedalboard and mido packages (pip install pedalboard mido)")
        if not os.path.exists(path):
            sys.exit("plug-in not found: %s" % path)
        self.plugin = load_plugin(path)
        self.Message = Message

    def set(self, **params):
        for k, v in params.items():
            setattr(self.plugin, k, v)

    def load_program(self, prog, arp=False):
        """Loads a 254-byte program by rewriting the PARAM values in the plug-in's state. Returns the ids set."""
        values = program_state(prog, arp)
        raw = self.plugin.raw_state
        outer = raw[8:8 + struct.unpack("<I", raw[4:8])[0]].decode("utf-8")
        comp = re.search(r"<IComponent>([^<]*)</IComponent>", outer)
        blob = juce_decode(comp.group(1))
        n = struct.unpack("<I", blob[4:8])[0]
        xml, tail = blob[8:8 + n].decode("utf-8"), blob[8 + n:]
        attr = re.search(r'ProgramParameters="([^"]*)"', xml)
        inner = html.unescape(attr.group(1))
        if attr_escape(inner) != attr.group(1):
            sys.exit("the plug-in's state is escaped in a way load_program does not reproduce")
        done = set()

        def put(m):
            if m.group(1) not in values:
                return m.group(0)
            done.add(m.group(1))
            return '<PARAM id="%s" value="%.1f"/>' % (m.group(1), float(values[m.group(1)]))
        inner = re.sub(r'<PARAM id="([^"]+)" value="[^"]*"/>', put, inner)
        missing = set(values) - done
        if missing:
            sys.exit("the plug-in's state has no PARAM for: %s" % sorted(missing))
        xml = (xml[:attr.start(1)] + attr_escape(inner) + xml[attr.end(1):]).encode("utf-8")
        blob = b"VC2!" + struct.pack("<I", len(xml)) + xml + tail
        outer = (outer[:comp.start(1)] + juce_encode(blob) + outer[comp.end(1):]).encode("utf-8")
        self.plugin.raw_state = b"VC2!" + struct.pack("<I", len(outer)) + outer + b"\x00"
        return done

    def render(self, note, gate_s, total_s, velocity=100, stereo=False):
        """One note as float64: the mono mix, or frames x 2 with stereo."""
        msgs = [self.Message("note_on", note=note, velocity=velocity),
                self.Message("note_off", note=note, time=gate_s)]
        lr = self.plugin(msgs, duration=total_s, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T
        return lr if stereo else lr.mean(axis=1)


def band_spectrum(x, n=2048):
    seg = x[:len(x) // n * n].reshape(-1, n) * np.hanning(n)
    return (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0)


def cmd_params(vst, _args):
    for name, p in vst.plugin.parameters.items():
        values = getattr(p, "valid_values", None) or []
        span = "%s .. %s (%d)" % (values[0], values[-1], len(values)) if values else ""
        print("%-22s %-14s %s" % (name, getattr(vst.plugin, name), span))


def cmd_eq(vst, args):
    """Noise through both EQs. Each side is compared with its own flat render, so only the EQ is left."""
    seconds = args.seconds
    vst.set(t1_osc1wave="Noise", t1_filtertype="24LPF", t1_cutoff=127.0, t1_resonance=0.0, delay_depth=0.0, modfx_depth=0.0)
    timbre = dict(VOCODER_CARRIER, wave1=1.0, osc1_ctrl1=1.0, osc1_ctrl2=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0, cutoff=1.0,
                  filter_type=0.0, keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0, attack2=0.0,
                  sustain2=1.0, portamento=0.0, level=0.25, amp_level=33.0 / 128.0)
    patch = {"voice_mode": 0.0, "t1": timbre, "t2": dict(timbre), "fx": {f: 0.0 for f in cal.FX_FIELDS}}
    tmp = tempfile.mkdtemp(prefix="tinyk_ab_")
    eng = cal.Engine(cal.build_library(tmp))

    def vst_spec(low_i, low_db, hi_i, hi_db):
        vst.set(eq_lowfreq=float(LOW_HZ[low_i]), eq_lowgain=float(low_db), eq_hifreq=float(HI_HZ[hi_i]), eq_higain=float(hi_db))
        return band_spectrum(vst.render(60, seconds, seconds)[SR:])

    def eng_spec(low_i, low_db, hi_i, hi_db):
        eq = {"hi_freq": hi_i / 29.0, "hi_gain": 0.5 + hi_db / 24.0, "low_freq": low_i / 29.0, "low_gain": 0.5 + low_db / 24.0}
        return band_spectrum(eng.render_patch(0, dict(patch, eq=eq), 60, seconds, seconds)[SR:])

    f = np.fft.rfftfreq(2048, 1.0 / SR)
    band = (f > 40) & (f < 18000)
    smooth = lambda d: np.convolve(d, np.ones(5) / 5, mode="same")
    vst_flat, eng_flat = vst_spec(0, 0, 0, 0), eng_spec(0, 0, 0, 0)
    floor = smooth(10 * np.log10(vst_spec(0, 0, 0, 0) / vst_flat))[band]
    print("method noise (the plug-in, flat against flat): %.2f dB rms" % np.sqrt(np.mean(floor ** 2)))
    points = [50, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 12000, 16000]
    print("%-34s %s   rms" % ("Hz", " ".join("%6d" % x for x in points)))
    worst = 0.0
    for low_i, low_db, hi_i, hi_db in ((9, 12, 0, 0), (9, -12, 0, 0), (0, 0, 12, 12), (0, 0, 12, -12), (29, 6, 0, 0),
                                       (0, 0, 29, 12), (0, 0, 0, -6), (4, 8, 20, -7), (15, -4, 24, 9)):
        v = smooth(10 * np.log10(vst_spec(low_i, low_db, hi_i, hi_db) / vst_flat))
        e = smooth(10 * np.log10(eng_spec(low_i, low_db, hi_i, hi_db) / eng_flat))
        rms = np.sqrt(np.mean((v - e)[band] ** 2))
        worst = max(worst, rms)
        label = "Low %d/%+d  Hi %d/%+d" % (LOW_HZ[low_i], low_db, HI_HZ[hi_i], hi_db)
        print("%-28s VST   %s" % (label, " ".join("%6.1f" % np.interp(x, f, v) for x in points)))
        print("%-28s TinyK %s   %.2f" % ("", " ".join("%6.1f" % np.interp(x, f, e) for x in points), rms))
    print("worst setting: %.2f dB rms, 40 Hz - 18 kHz" % worst)


def echo_train(lr, count=4):
    """A noise burst and its repeats: [(time s, gain on the left, gain on the right)] of the `count` strongest
    repeats, gains relative to the burst itself (cross-correlation with the first 11 ms of the louder side)."""
    from scipy.signal import fftconvolve
    d = int(0.011 * SR)
    src = lr[:, 0] if (lr[:d, 0] ** 2).sum() >= (lr[:d, 1] ** 2).sum() else lr[:, 1]
    dry = src[:d]
    energy = (dry ** 2).sum()
    if energy < 1e-9:
        return []
    c = [fftconvolve(lr[:, ch], dry[::-1], mode="full")[d - 1:] / energy for ch in (0, 1)]
    mag = np.maximum(np.abs(c[0]), np.abs(c[1]))
    mag[:int(0.012 * SR)] = 0   # the burst against itself; the shortest delay is 13.7 ms
    out = []
    for _ in range(count):
        k = int(np.argmax(mag))
        if mag[k] < 2e-3:
            break
        out.append((k / SR, float(c[0][k]), float(c[1][k])))
        mag[max(0, k - d):k + d] = 0
    return sorted(out)


def cmd_delay(vst, _args):
    """A 10 ms noise burst through both delays. The plug-in sometimes returns a render that is not a clean
    echo train, so each setting is rendered up to 5 times and the first train agreeing with another is used."""
    vst.set(modfx_depth=0.0, t1_osc1wave="Noise", t1_filtertype="24LPF", t1_cutoff=127.0, t1_resonance=0.0,
            t1_filteregint=0.0, t1_ampegattack=0.0, t1_ampegdecay=0.0, t1_ampegsustain=0.0, t1_ampegrelease=0.0,
            delay_temposync=False, eq_lowgain=0.0, eq_higain=0.0)
    pans = vst.plugin.parameters["t1_panpot"].valid_values
    timbre = dict(VOCODER_CARRIER, wave1=1.0, osc1_ctrl1=1.0, osc1_ctrl2=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, resonance=0.0, cutoff=1.0,
                  filter_type=0.0, keytrack=0.5, env_int=0.5, drive=0.0, attack1=0.0, sustain1=1.0, attack2=0.0,
                  decay2=0.0, sustain2=0.0, release2=0.0, portamento=0.0, level=0.25, amp_level=33.0 / 128.0)
    tmp = tempfile.mkdtemp(prefix="tinyk_ab_")
    eng = cal.Engine(cal.build_library(tmp))

    def vst_train(kind, time, depth, pan, total):
        vst.set(delay_type=kind, delay_time=float(time), delay_depth=float(depth), t1_panpot=pans[pan])
        vst.render(60, 0.01, 0.3)
        seen = []
        for _ in range(5):
            t = echo_train(vst.render(60, 0.01, total, stereo=True))
            for u in seen:
                if len(u) == len(t) and all(abs(a[0] - b[0]) < 5e-4 and abs(a[1] - b[1]) < 0.02 and abs(a[2] - b[2]) < 0.02
                                            for a, b in zip(t, u)):
                    return t
            seen.append(t)
        return seen[0]

    def eng_train(kind, time, depth, pan, total):
        fx = dict({f: 0.0 for f in cal.FX_FIELDS}, delay_time=time / 127.0, delay_feedback=depth / 127.0, delay_mix=depth / 127.0)
        t = dict(timbre, pan=pan / 126.0)
        patch = {"voice_mode": 0.0, "t1": t, "t2": dict(t), "fx": fx, "delay_type": ["Stereo", "Cross", "L/R"].index(kind) / 2.0}
        return echo_train(eng.render_patch(0, patch, 60, 0.01, total, stereo=True))

    worst_t, worst_g = 0.0, 0.0
    for kind, time, depth, pan in (("Stereo", 40, 90, 63), ("Stereo", 40, 90, 0), ("Cross", 40, 90, 0), ("Cross", 40, 90, 126),
                                   ("L/R", 40, 90, 0), ("L/R", 40, 90, 63), ("Stereo", 0, 64, 63), ("Stereo", 64, 40, 63),
                                   ("Cross", 90, 110, 0), ("L/R", 110, 127, 126), ("Stereo", 118, 100, 63),
                                   ("Stereo", 126, 80, 63), ("Stereo", 127, 80, 63)):
        total = 0.6 + 4.2 * max(0.2, (time / 127.0) ** 2) * 2
        v, e = vst_train(kind, time, depth, pan, total), eng_train(kind, time, depth, pan, total)
        fmt = lambda t: "  ".join("%.3fs L%+.3f R%+.3f" % r for r in t)
        print("%-6s time %3d depth %3d pan %-3s VST   %s" % (kind, time, depth, pans[pan], fmt(v)))
        print("%-33s TinyK %s" % ("", fmt(e)))
        for a in v:   # each of the plug-in's repeats against the engine's nearest in time
            b = min(e, key=lambda r: abs(r[0] - a[0])) if e else (9.0, 9.0, 9.0)
            worst_t = max(worst_t, abs(a[0] - b[0]))
            worst_g = max(worst_g, abs(a[1] - b[1]), abs(a[2] - b[2]))
    print("worst difference: %.1f ms in time, %.3f in gain (of the burst's own level)" % (1000 * worst_t, worst_g))


def cmd_filter(vst, args):
    """Each engine's filter response, read off the harmonics of a 55 Hz saw (they reach 20 kHz there; on lower
    notes the plug-in's saw runs out near 5 kHz) and taken relative to its own open high-pass, so only the
    filter is left. Prints the rms dB between the two curves per setting (and the median offset)."""
    note, f0, n = 33, 55.0, 1 << 17
    win = np.hanning(n)
    harm = np.arange(1, int(19000 / f0))
    bins = np.round(harm * f0 * n / SR).astype(int)
    freq = harm * f0
    kinds = ["24LPF", "12LPF", "12BPF", "12HPF"]

    def harmonics(x):
        spec = np.abs(np.fft.rfft(x[:n] * win))
        return np.array([spec[b - 2:b + 3].max() for b in bins])

    vst.set(modfx_depth=0.0, delay_depth=0.0, eq_lowgain=0.0, eq_higain=0.0, t1_osc1wave="Saw", t1_osc1control1=0.0,
            t1_osc1control2=0.0, t1_osc1level=127.0, t1_osc2level=0.0, amp_level=33.0 / 128.0, t1_noiselevel=0.0, t1_filteregint=0.0,
            t1_filterkbdtrack=0.0, t1_ampegattack=0.0, t1_ampegsustain=127.0, t1_amplevel=127.0, t1_voiceassign="Mono",
            t1_distortion=False, t1_vpatch1int=0.0, t1_vpatch2int=0.0, t1_vpatch3int=0.0, t1_vpatch4int=0.0, t1_vibratoint=0.0)
    tmp = tempfile.mkdtemp(prefix="tinyk_ab_")
    eng = cal.Engine(cal.build_library(tmp))
    timbre = dict(VOCODER_CARRIER, wave1=0.0, osc_mix=0.0, noise_level=0.0, detune=0.5, keytrack=0.5, env_int=0.5, drive=0.0,
                  attack1=0.0, sustain1=1.0, attack2=0.0, sustain2=1.0, portamento=0.0, level=0.05, amp_level=33.0 / 128.0)
    fx = {k: 0.0 for k in cal.FX_FIELDS}

    def vst_raw(kind, cut, res):
        vst.set(t1_filtertype=kind, t1_cutoff=float(cut), t1_resonance=float(res))
        vst.render(note, 0.05, 0.2)
        return harmonics(vst.render(note, 3.6, 3.6)[int(0.5 * SR):])

    def eng_raw(kind, cut, res):
        t = dict(timbre, filter_type=kinds.index(kind) / 3.0, cutoff=cut / 127.0, resonance=res / 127.0)
        return harmonics(eng.render_patch(0, {"voice_mode": 0.0, "t1": t, "t2": dict(t), "fx": fx}, note, 3.6, 3.6)[int(0.5 * SR):])

    vref, eref = vst_raw("12HPF", 0, 0), eng_raw("12HPF", 0, 0)
    cuts, ress = (20, 40, 60, 80, 100, 120), (0, 20, 40, 60, 80, 100)
    every = []
    print("rms dB between the two responses, 80 Hz - 16 kHz (median offset, the plug-in over the engine); columns: resonance %s" % (ress,))
    for kind in kinds:
        print(kind)
        for cut in cuts:
            row = []
            for res in ress:
                a = 20 * np.log10(vst_raw(kind, cut, res) / vref + 1e-12)
                b = 20 * np.log10(eng_raw(kind, cut, res) / eref + 1e-12)
                m = (freq > 80) & (freq < 16000) & ((a > a.max() - 40) | (b > b.max() - 40))
                d = (a - b)[m]
                every.append(np.sqrt(np.mean(d ** 2)))
                row.append("%4.1f(%+5.1f)" % (every[-1], np.median(d)))
            print("  cutoff %3d: %s" % (cut, " ".join(row)))
    print("overall: mean %.2f dB, median %.2f, worst %.1f (resonance above 100 is left out: its peak falls between"
          " harmonics, measure it by ring-down)" % (np.mean(every), np.median(every), np.max(every)))


OSC1_WAVES = ["Saw", "Pulse", "Triangle", "Sine", "Vox", "DWGS", "Noise", "Audio In"]
PATCH_DESTS = ["pitch", "osc2 tune", "ctrl 1", "noise", "cutoff", "amp", "pan", "LFO2 freq"]


def program_tags(prog, dry):
    """What a program uses, as tags: the features to blame its differences on."""
    layer = ((prog[16] >> 4) & 3) == 2
    tags = {"Layer" if layer else "Single"}
    for n, t in enumerate((38, 146)[:2 if layer else 1], 1):
        wave = OSC1_WAVES[prog[t + 7] & 7]
        tags.add("osc1 " + wave)
        if wave not in ("DWGS", "Audio In"):
            if prog[t + 8]:
                tags.add("%s ctrl1 > 0" % wave)
            if prog[t + 9]:
                tags.add("%s ctrl2 > 0" % wave)
        if prog[t + 17]:
            tags.add("osc2 on")
            mod = (prog[t + 12] >> 4) & 3
            if mod:
                tags.add("osc2 " + ["", "ring", "sync", "ring+sync"][mod])
        if prog[t + 18]:
            tags.add("noise level > 0")
        tags.add("filter " + ["LPF24", "LPF12", "BPF12", "HPF12"][prog[t + 19] & 3])
        if prog[t + 21] >= 90:
            tags.add("resonance >= 90")
        if prog[t + 22] != 64:
            tags.add("filter EG int")
        if prog[t + 24] != 64:
            tags.add("filter kbd track")
        if prog[t + 27] & 1:
            tags.add("distortion")
        if prog[t + 26] != 64:
            tags.add("timbre panned")
        tags.add(["Mono", "Poly", "Unison", "Unison"][prog[t + 1] >> 6])
        if prog[t + 15] & 127:
            tags.add("portamento")
        for i in range(4):
            route, amt = prog[t + 44 + 2 * i], prog[t + 45 + 2 * i] - 64
            if amt:
                tags.add("patch -> " + PATCH_DESTS[min(route >> 4, 7)])
                if (route & 15) in (2, 3) and (prog[t + 40 + 3 * ((route & 15) - 2)] >> 7):
                    tags.add("patch from a tempo-synced LFO")
    if not dry:
        if prog[21]:
            tags.add("delay " + ["Stereo", "Cross", "L/R"][min(prog[22], 2)])
            if prog[19] >> 7:
                tags.add("delay tempo sync")
        if prog[24]:
            tags.add("mod fx " + ["Chorus/Flanger", "Ensemble", "Phaser"][min(prog[25], 2)])
        if prog[27] != 64 or prog[29] != 64:
            tags.add("EQ")
    return tags


def compare_renders(a, b, gate_s):
    """Differences between two stereo renders (frames x 2) of the same note, a = the plug-in, b = the engine.
    level: dB, a over b. tone: rms dB between their band spectra (octaves to 400 Hz, third octaves above) over the held note, each scaled to its
    own level. env: rms dB between their 10 ms level contours over the whole render, each relative to its own
    loudest frame. width: dB difference of side over mid."""
    def held(x):
        # from the note's start to its end: spectra and levels are sums of power, so they weigh the loud part,
        # and a pluck that is over in 100 ms is judged on its sound, not on the silence that follows
        return x[:int(gate_s * SR)]

    def level_db(x):
        return 10 * np.log10((held(x) ** 2).sum(axis=1).mean() + 1e-12)

    def thirds(x):
        m = held(x).mean(axis=1)
        n = 8192
        seg = m[:len(m) // n * n].reshape(-1, n) * np.hanning(n)
        p = (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0)
        f = np.fft.rfftfreq(n, 1.0 / SR)
        # octave bands up to 400 Hz (a low note has one harmonic per octave there, and a third-octave band
        # with none in it would compare noise floors), third-octave bands from there to 18 kHz
        edges = [50.0, 100.0, 200.0, 400.0] + list(400.0 * 2 ** (np.arange(1, 18) / 3.0))
        bands = np.array([p[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])])
        return 10 * np.log10(bands / bands.sum() + 1e-12)

    def contour(x):
        w = SR // 100
        e = (x[:len(x) // w * w] ** 2).sum(axis=1).reshape(-1, w).mean(axis=1)
        return np.maximum(10 * np.log10(e / e.max() + 1e-12), -50.0)   # relative to its own loudest 10 ms

    def width(x):
        h = held(x)
        mid, side = ((h[:, 0] + h[:, 1]) ** 2).mean(), ((h[:, 0] - h[:, 1]) ** 2).mean()
        return float(np.clip(10 * np.log10((side + 1e-12) / (mid + 1e-12)), -40.0, 10.0))

    ta, tb = thirds(a), thirds(b)
    heard = (ta > ta.max() - 40) | (tb > tb.max() - 40)
    ca, cb = contour(a), contour(b)
    n = min(len(ca), len(cb))
    live = (ca[:n] > -40) | (cb[:n] > -40)
    return {"level": level_db(a) - level_db(b),
            "tone": float(np.sqrt(np.mean((np.maximum(ta, ta.max() - 50) - np.maximum(tb, tb.max() - 50))[heard] ** 2))),
            "env": float(np.sqrt(np.mean((ca[:n] - cb[:n])[live] ** 2))) if live.any() else 0.0,
            "width": width(a) - width(b)}


def cmd_programs(vst, args):
    """Every synth program of a dump, one held note through both. FX off with --dry (the synth alone)."""
    import json
    progs = load_programs(args.bank)
    tmp = tempfile.mkdtemp(prefix="tinyk_ab_")
    eng = cal.Engine(cal.build_library(tmp))
    gate, total = args.gate, args.gate + 1.0
    rows = []
    for idx, prog in enumerate(progs):
        if ((prog[16] >> 4) & 3) == 3:
            continue   # vocoder
        prog = bytearray(prog)
        if args.dry:
            prog[21] = prog[24] = 0
            prog[27] = prog[29] = 64
        prog = bytes(prog)
        patch = parse_program(idx, prog)
        vst.load_program(prog)
        vst.render(args.note, 0.05, 0.3)
        b = eng.render_patch(0, patch, args.note, gate, total, stereo=True)
        # the plug-in starts its oscillators at random phases and lets its LFOs run on, so one program can read
        # several dB differently from one render to the next: the figures are the median over --takes renders
        takes = [compare_renders(vst.render(args.note, gate, total, stereo=True), b, gate) for _ in range(args.takes)]
        d = {k: float(np.median([t[k] for t in takes])) for k in takes[0]}
        rows.append(dict(d, index=idx, label=patch["label"], tags=sorted(program_tags(prog, args.dry))))
    offset = float(np.median([r["level"] for r in rows]))
    for r in rows:
        r["level"] -= offset
    print("%d programs, note %d held %.1f s%s. The plug-in is %+.1f dB against the engine (median), removed below."
          % (len(rows), args.note, gate, ", FX off" if args.dry else "", offset))
    print("\nBy program, worst tone first (rms dB over third-octave bands; env = level contour; level and width in dB)")
    print("%-22s %6s %6s %6s %6s" % ("", "tone", "env", "level", "width"))
    for r in sorted(rows, key=lambda r: -r["tone"])[:args.top]:
        print("%-22s %6.1f %6.1f %+6.1f %+6.1f  %s" % (r["label"][:22], r["tone"], r["env"], r["level"], r["width"],
                                                     ", ".join(t for t in r["tags"] if t not in ("Single", "Poly"))))
    for key, title in (("tone", "tone"), ("env", "level contour"), ("level", "level, as |dB|"), ("width", "stereo width, as |dB|")):
        val = (lambda r: abs(r[key])) if key in ("level", "width") else (lambda r: r[key])
        stats = []
        for tag in sorted({t for r in rows for t in r["tags"]}):
            w = [val(r) for r in rows if tag in r["tags"]]
            wo = [val(r) for r in rows if tag not in r["tags"]]
            if len(w) >= 3 and len(wo) >= 3:
                stats.append((np.mean(w) - np.mean(wo), tag, len(w), np.mean(w), np.mean(wo)))
        print("\nWhat goes with a worse %s (mean with the feature, mean without, programs)   all: mean %.1f, median %.1f"
              % (title, np.mean([val(r) for r in rows]), np.median([val(r) for r in rows])))
        for diff, tag, n, w, wo in sorted(stats, reverse=True)[:10]:
            print("  %-32s %5.1f vs %5.1f  (%d)" % (tag, w, wo, n))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump({"offset": offset, "note": args.note, "dry": args.dry, "rows": rows}, f, indent=1)
        print("\nsaved %s" % args.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--vst", default=DEFAULT_VST)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("params")
    eq = sub.add_parser("eq")
    eq.add_argument("--seconds", type=float, default=30.0)
    sub.add_parser("delay")
    sub.add_parser("filter")
    pr = sub.add_parser("programs")
    pr.add_argument("bank")
    pr.add_argument("--dry", action="store_true", help="delay, Mod FX and EQ off in both: the synth alone")
    pr.add_argument("--note", type=int, default=48)
    pr.add_argument("--takes", type=int, default=1, help="plug-in renders per program; each figure is their median")
    pr.add_argument("--gate", type=float, default=2.0)
    pr.add_argument("--top", type=int, default=25)
    pr.add_argument("--out", help="save every program's figures as JSON")
    args = ap.parse_args()
    {"params": cmd_params, "eq": cmd_eq, "delay": cmd_delay, "filter": cmd_filter, "programs": cmd_programs}[args.cmd](Vst(args.vst), args)


if __name__ == "__main__":
    main()
