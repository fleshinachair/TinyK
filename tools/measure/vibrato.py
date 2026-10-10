"""Vibrato Int, Bend Range and a patch into a tempo-synced LFO2's frequency, on the plug-in: the pitch of a sine
tracked by its zero crossings."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst = make_vst()
def play(events, total, extra=()):
    msgs = [vst.Message("note_on" if on else "note_off", note=n, velocity=100, time=t) for t, n, on in events] + list(extra)
    msgs.sort(key=lambda m: m.time)
    return vst.plugin(msgs, duration=total, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T.mean(axis=1)
def pitch_track(x, t0=0.3):
    """(times, semitones re the median) from rising zero crossings"""
    x = x[int(t0 * SR):]
    i = np.nonzero((x[:-1] < 0) & (x[1:] >= 0))[0]
    z = i + x[i] / (x[i] - x[i + 1])
    f = SR / np.diff(z)
    return (z[1:] + z[:-1]) / 2 / SR + t0, f
def swing(x, base):
    t, f = pitch_track(x)
    st = 12 * np.log2(f / base)
    # LFO rate from the pitch track's own spectrum
    u = np.interp(np.arange(t[0], t[-1], 0.001), t, st)
    s = np.abs(np.fft.rfft((u - u.mean()) * np.hanning(len(u)), 1 << 16)); fr = np.fft.rfftfreq(1 << 16, 0.001)
    return np.percentile(st, 99.5), np.percentile(st, 0.5), fr[np.argmax(s[5:]) + 5]
vst.set(t1_osc1wave="Sine", t1_lfo2wave="Sine", t1_lfo2frequency=70.0, t1_lfo2temposync=False, t1_lfo2keysync="Off", t1_lfo1temposync=False)
base = hz(69)
cc = lambda v: [vst.Message("control_change", control=1, value=int(v), time=0.0)]
pb = lambda v: [vst.Message("pitchwheel", pitch=int(v), time=0.0)]
print("VIBRATO INT (LFO2 sine, rate knob 70; the wheel sent as CC1): pitch swing in semitones, high / low, and the rate")
for wheel in (0, 32, 64, 96, 127):
    row = []
    for vi in (2.0, 5.0, 14.0, 25.0, 50.0, 63.0, -25.0):
        vst.set(t1_vibratoint=vi)
        hi, lo, rate = swing(play([(0, 69, 1)], 4.0, cc(wheel)), base)
        row.append("%+3d: %+6.3f/%+6.3f" % (vi, hi, lo))
    print("  wheel %3d  %s" % (wheel, "  ".join(row)))
for wave in ("Saw", "Square2", "Sample&Hold"):
    try:
        vst.set(t1_vibratoint=25.0, t1_lfo2wave=wave)
        hi, lo, rate = swing(play([(0, 69, 1)], 4.0, cc(127)), base)
        print("  LFO2 %s, int +25, wheel 127: %+6.3f / %+6.3f at %.2f Hz" % (wave, hi, lo, rate))
    except Exception as e: print("  ", wave, e)
vst.set(t1_vibratoint=0.0, t1_lfo2wave="Sine")
print("BEND RANGE: semitones at pitch wheel -8192, -4096, 0, +4096, +8191")
for rng in (2.0, 12.0, -12.0, 9.0, 0.0, 1.0):
    row = []
    for bend in (-8192, -4096, 0, 4096, 8191):
        vst.set(t1_bendrange=rng)
        t, f = pitch_track(play([(0, 69, 1)], 1.2, pb(bend)))
        row.append("%+7.3f" % (12 * np.log2(np.median(f) / base)))
    print("  range %+3d: %s" % (rng, " ".join(row)))
vst.set(t1_bendrange=2.0)
print("PATCH -> LFO2 FREQ with LFO2 tempo-synced (arp tempo 120): LFO2 -> pitch +24, source Mod Wheel at CC1 = 127")
vst.set(arp_tempo=120.0, t1_lfo2temposync=True, t1_vpatch1source="LFO 2", t1_vpatch1dest="Pitch", t1_vpatch1int=24.0,
        t1_vpatch2source="Mod. WH", t1_vpatch2dest="LFO2 Freq")
for note in ("1/1", "1/4", "1/8", "1/16"):
    row = []
    for amt in (0.0, 19.0, 40.0, 63.0, -40.0):
        vst.set(t1_lfo2syncnote=note, t1_vpatch2int=amt)
        hi, lo, rate = swing(play([(0, 69, 1)], 6.0, cc(127)), base)
        row.append("%+3d: %6.3f Hz" % (amt, rate))
    print("  sync %-5s %s" % (note, "   ".join(row)))
vst.set(t1_lfo2temposync=False, t1_lfo2frequency=40.0)
row = []
for amt in (0.0, 19.0, 40.0):
    vst.set(t1_vpatch2int=amt)
    row.append("%+3d: %6.3f Hz" % (amt, swing(play([(0, 69, 1)], 6.0, cc(127)), base)[2]))
print("  free, knob 40: %s" % "   ".join(row))
