"""Ring and ring+sync with each Osc 1 wave: the engine against the plug-in, level and band spectrum, Osc 2 at an
inharmonic interval. Osc 1's own level at 0 leaves the ring product alone."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
W2 = ["Saw", "Square", "Triangle"]
def level(x): return 10 * np.log10((x ** 2).mean() + 1e-12)
def bands(x):
    n = 8192
    seg = x[:len(x) // n * n].reshape(-1, n) * np.hanning(n)
    p = (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0); f = np.fft.rfftfreq(n, 1.0 / SR)
    edges = [50.0, 100.0, 200.0, 400.0] + list(400.0 * 2 ** (np.arange(1, 17) / 3.0))
    b = np.array([p[(f >= lo) & (f < hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])])
    return 10 * np.log10(b / b.sum() + 1e-12)
def both(w1, w2, mod, semi, lvl1):
    vst.set(t1_osc1wave=["Saw", "Square", "Triangle", "Sine"][w1], t1_osc2wave=W2[w2], t1_osc2modselect=["Off", "Ring", "Sync", "RingSync"][mod],
            t1_osc2semitone=float(semi), t1_osc1level=float(lvl1), t1_osc2level=127.0)
    vst.render(45, 0.05, 0.2)
    a = [vst.render(45, 2.2, 2.2)[int(0.4 * SR):] for _ in range(3)]
    p = patch(wave1=w1 / 6.0, wave2=w2 / 2.0, sync_ring={0: 0, 1: 2, 2: 1, 3: 3}[mod] / 3.0, detune=0.5 + semi / 48.0,
              osc1_level=(lvl1 + 1) / 128.0, osc2_level=1.0)
    b = eng.render_patch(0, p, 45, 2.2, 2.2)[int(0.4 * SR):]
    return a, b
# each side's reference: plain Osc 2 saw alone
ra, rb = both(3, 0, 0, 7, 0)
ref = np.median([level(x) for x in ra]) - level(rb)
print("level = plug-in minus engine, dB, after removing the same difference for a plain Osc 2 saw; tone = rms dB over bands within 40 dB of the loudest")
for w1, n1 in ((3, "Sine"), (0, "Saw"), (2, "Triangle")):
    for mod, nm in ((1, "ring"), (3, "ring+sync")):
        row = []
        for w2 in range(3):
            for semi in (7, 19):
                for lvl1 in (0, 127):
                    a, b = both(w1, w2, mod, semi, lvl1)
                    lv = np.median([level(x) for x in a]) - level(b) - ref
                    tb = bands(b); t = []
                    for x in a:
                        ta = bands(x); m = (ta > ta.max() - 40) | (tb > tb.max() - 40)
                        t.append(np.sqrt(np.mean((np.maximum(ta, ta.max() - 50) - np.maximum(tb, tb.max() - 50))[m] ** 2)))
                    row.append("%+5.1f/%4.1f" % (lv, np.median(t)))
        print("%-8s %-9s %s" % (n1, nm, "  ".join(row)))
print("columns: Osc 2 saw, square, triangle; each at +7 and +19 semitones; each with Osc 1 level 0 and 127")
