"""EG reset, bend range and vibrato int: the engine against the plug-in, the same MIDI through both."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
def vplay(events, total):
    msgs = []
    for t, st, a, b in events:
        if st == 0x90: msgs.append(vst.Message("note_on", note=a, velocity=b, time=t))
        elif st == 0x80: msgs.append(vst.Message("note_off", note=a, time=t))
        elif st == 0xB0: msgs.append(vst.Message("control_change", control=a, value=b, time=t))
        elif st == 0xE0: msgs.append(vst.Message("pitchwheel", pitch=((b << 7) | a) - 8192, time=t))
    return vst.plugin(msgs, duration=total, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T.mean(axis=1)
def env(x, t0, t1, step):
    w = int(step * SR); a = int(t0 * SR)
    e = np.array([np.sqrt((x[a + i * w:a + (i + 1) * w] ** 2).mean()) for i in range(int((t1 - t0) / step))])
    return 20 * np.log10(e / (np.sqrt((x ** 2).reshape(-1)[:len(x) // 441 * 441].reshape(-1, 441).mean(axis=1)).max()) + 1e-6)
def pitch(x, t0=0.3):
    x = x[int(t0 * SR):]
    i = np.nonzero((x[:-1] < 0) & (x[1:] >= 0))[0]
    z = i + x[i] / (x[i] - x[i + 1])
    return 12 * np.log2(SR / np.diff(z) / hz(69))
print("EG RESET: Mono sine, amp attack A, release 70; key 81 on, off at 1.0 s, on again at 1.3 s. Level per 20 ms from 1.30 s, dB re the loudest")
ev = [(0, 0x90, 81, 100), (1.0, 0x80, 81, 0), (1.3, 0x90, 81, 100), (2.4, 0x80, 81, 0)]
for atk in (64, 20, 0):
    for reset in (1, 0):
        vst.set(t1_osc1wave="Sine", t1_voiceassign="Mono", t1_ampegattack=float(atk), t1_ampegdecay=64.0, t1_ampegsustain=127.0, t1_ampegrelease=70.0, t1_ampegreset=bool(reset))
        vplay([(0, 0x90, 60, 100), (0.05, 0x80, 60, 0)], 0.3)
        a = env(vplay(ev, 2.6), 1.30, 1.72, 0.02)
        p = patch(wave1=0.5, attack2=atk / 127.0, decay2=64 / 127.0, sustain2=1.0, release2=70 / 127.0, assign=0.0, eg2_reset=1.0 if reset else 0.5)
        b = env(eng.render_midi(0, p, ev, 2.6), 1.30, 1.72, 0.02)
        print("  attack %2d reset %d  plug-in %s\n                     engine  %s" % (atk, reset, " ".join("%4.0f" % v for v in a), " ".join("%4.0f" % v for v in b)))
        if atk == 64 and reset:
            a = env(vplay(ev, 2.6), 1.300, 1.316, 0.001); b = env(eng.render_midi(0, p, ev, 2.6), 1.300, 1.316, 0.001)
            print("     per ms          plug-in %s\n                     engine  %s" % (" ".join("%4.0f" % v for v in a), " ".join("%4.0f" % v for v in b)))
vst.set(t1_ampegattack=0.0, t1_ampegrelease=0.0, t1_ampegreset=True)
print("BEND RANGE: semitones at the wheel's ends and half way (plug-in | engine)")
for rng in (2, 12, -12, 9, 0, -3):
    row = []
    for bend in (0, 4096, 8192, 12288, 16383):
        e2 = [(0, 0xE0, bend & 127, bend >> 7), (0, 0x90, 69, 100)]
        vst.set(t1_bendrange=float(rng))
        a = np.median(pitch(vplay(e2, 1.2)))
        b = np.median(pitch(eng.render_midi(0, patch(wave1=0.5, bend_range=(rng + 13) / 25.0), e2, 1.2)))
        row.append("%+6.2f|%+6.2f" % (a, b))
    print("  range %+3d: %s" % (rng, "  ".join(row)))
vst.set(t1_bendrange=2.0)
print("VIBRATO INT: LFO2 (sine, knob 70) swing in semitones, high / low (plug-in | engine)")
vst.set(t1_lfo2wave="Sine", t1_lfo2frequency=70.0, t1_lfo2temposync=False, t1_lfo2keysync="Off")
for wheel in (0, 64, 127):
    for vi in (5, 25, 50, 63, -25):
        e3 = [(0, 0xB0, 1, wheel), (0, 0x90, 69, 100)]
        vst.set(t1_vibratoint=float(vi))
        a = pitch(vplay(e3, 4.0)); b = pitch(eng.render_midi(0, patch(wave1=0.5, lfo2_wave=2 / 3.0, lfo2_rate=70 / 127.0, vibrato_int=(vi + 64) / 127.0), e3, 4.0))
        print("  wheel %3d int %+3d: %+7.3f / %+7.3f  |  %+7.3f / %+7.3f" % (wheel, vi, np.percentile(a, 99.5), np.percentile(a, 0.5), np.percentile(b, 99.5), np.percentile(b, 0.5)))
