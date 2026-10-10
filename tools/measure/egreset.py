"""EG reset on the plug-in: what a second note-on does to a running envelope. Mono voice, a sine through the open
high-pass (amp EG), or a saw through the low-pass with EG int +63 (filter EG, read from the level)."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst = make_vst()
def play(events, total):
    msgs = [vst.Message("note_on" if on else "note_off", note=n, velocity=100, time=t) for t, n, on in events]
    return vst.plugin(msgs, duration=total, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T.mean(axis=1)
def contour(x, step=0.05):
    w = int(step * SR)
    e = np.sqrt((x[:len(x) // w * w] ** 2).reshape(-1, w).mean(axis=1))
    return 20 * np.log10(e / e.max() + 1e-6)
def row(tag, x): print("%-46s %s" % (tag, " ".join("%4.0f" % v for v in contour(x))))
vst.set(t1_osc1wave="Sine", t1_ampegattack=64.0, t1_ampegdecay=64.0, t1_ampegsustain=127.0, t1_ampegrelease=70.0)
print("amp EG, attack 64 (0.79 s), release 70; level per 50 ms, dB re the loudest")
for assign in ("Mono", "Poly"):
    for trig in ("Single", "Multi"):
        for reset in (True, False):
            vst.set(t1_voiceassign=assign, t1_triggermode=trig, t1_ampegreset=reset)
            play([(0, 60, 1), (0.05, 60, 0)], 0.3)
            # legato second key at 1.2 s
            row("%s %s reset %d: 60 held, 64 at 1.2 s" % (assign, trig, reset), play([(0, 60, 1), (1.2, 64, 1), (2.4, 60, 0), (2.4, 64, 0)], 2.6))
            # same key again during its release
            row("%s %s reset %d: 60 off 1.0, on again 1.3" % (assign, trig, reset), play([(0, 60, 1), (1.0, 60, 0), (1.3, 60, 1), (2.4, 60, 0)], 2.6))
            # another key during the release
            row("%s %s reset %d: 60 off 1.0, 64 on 1.3" % (assign, trig, reset), play([(0, 60, 1), (1.0, 60, 0), (1.3, 64, 1), (2.4, 64, 0)], 2.6))
