"""EG reset, close up: the level per millisecond around a retrigger (Mono, the key pressed again 0.3 s into its
release), for the amp EG and for the filter EG (a saw through the 24LPF, EG int +63: the level follows the corner)."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst = make_vst()
def play(events, total):
    msgs = [vst.Message("note_on" if on else "note_off", note=n, velocity=100, time=t) for t, n, on in events]
    return vst.plugin(msgs, duration=total, sample_rate=SR, num_channels=2, reset=True).astype(np.float64).T.mean(axis=1)
def env(x, t0, t1, step):
    w = int(step * SR); a = int(t0 * SR)
    n = int((t1 - t0) / step)
    e = np.array([np.sqrt((x[a + i * w:a + (i + 1) * w] ** 2).mean()) for i in range(n)])
    return 20 * np.log10(e / np.abs(x).max() * np.sqrt(2) + 1e-6)
ev = [(0, 81, 1), (1.0, 81, 0), (1.3, 81, 1), (2.4, 81, 0)]
vst.set(t1_osc1wave="Sine", t1_voiceassign="Mono", t1_ampegdecay=64.0, t1_ampegsustain=127.0, t1_ampegrelease=70.0)
for atk in (64.0, 20.0, 0.0):
    for reset in (True, False):
        vst.set(t1_ampegattack=atk, t1_ampegreset=reset)
        play([(0, 60, 1), (0.05, 60, 0)], 0.3)
        x = play(ev, 2.6)
        print("amp attack %3d reset %d, per ms from 1.297 s: %s" % (atk, reset, " ".join("%4.0f" % v for v in env(x, 1.297, 1.325, 0.001))))
        print("%30s per 20 ms from 1.30 s: %s" % ("", " ".join("%4.0f" % v for v in env(x, 1.30, 1.9, 0.02))))
# the filter EG: amp EG instant and reset off (so the level is the filter's doing)
vst.set(t1_osc1wave="Saw", t1_filtertype="24LPF", t1_cutoff=20.0, t1_resonance=0.0, t1_filteregint=63.0, t1_ampegattack=0.0, t1_ampegreset=False,
        t1_ampegrelease=90.0, t1_filteregdecay=64.0, t1_filteregsustain=127.0, t1_filteregrelease=70.0)
def centroid(x, t0, t1, step):
    w = int(step * SR); a = int(t0 * SR); out = []
    for i in range(int((t1 - t0) / step)):
        s = np.abs(np.fft.rfft(x[a + i * w:a + (i + 1) * w] * np.hanning(w))); f = np.fft.rfftfreq(w, 1 / SR)
        out.append((s * f).sum() / (s.sum() + 1e-12))
    return out
ev = [(0, 45, 1), (1.0, 45, 0), (1.3, 45, 1), (2.4, 45, 0)]
for atk in (64.0, 0.0):
    for reset in (True, False):
        vst.set(t1_filteregattack=atk, t1_filteregreset=reset)
        play([(0, 60, 1), (0.05, 60, 0)], 0.3)
        x = play(ev, 2.6)
        print("filter attack %3d reset %d, spectral centroid Hz per 40 ms from 0.9 s: %s" % (atk, reset, " ".join("%5.0f" % v for v in centroid(x, 0.9, 2.0, 0.04))))
