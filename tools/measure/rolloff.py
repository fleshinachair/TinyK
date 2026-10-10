"""How the plug-in's oscillators lose their top as the note rises: each harmonic of a saw against the same
frequency on a 55 Hz saw (whose harmonics are 55 Hz apart), per note; and a sine's level against the note."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
import json
vst, eng = make_vst(), make_eng()
N = 1 << 16
def vtake(note, **kw):
    vst.set(**kw); vst.render(note, 0.05, 0.2); return vst.render(note, 2.2, 2.2)[int(0.4 * SR):]
def etake(note, **kw):
    return eng.render_patch(0, patch(**kw), note, 2.2, 2.2)[int(0.4 * SR):]
def harm(x, f0, top=21500):
    s, f = spectrum(x, N)
    k = np.arange(1, int(top / f0) + 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    return np.array([s[b - 3:b + 4].max() for b in bins])
out = {}
print("sine level against the note, dB re note 45: plug-in | engine")
vs, es = harm(vtake(45, t1_osc1wave="Sine"), 110.0)[0], harm(etake(45, wave1=0.5), 110.0)[0]
out["sine"] = []
for note in range(60, 124, 4):
    a = 20 * np.log10(harm(vtake(note, t1_osc1wave="Sine"), hz(note), hz(note) * 1.5)[0] / vs)
    b = 20 * np.log10(harm(etake(note, wave1=0.5), hz(note), hz(note) * 1.5)[0] / es)
    out["sine"].append((note, a, b))
    print("  note %3d %6.0f Hz  %6.2f | %6.2f" % (note, hz(note), a, b))
for name, wave in (("Saw", 0), ("Square", 1)):
    refs = []
    for rn in (33, 35, 37):
        ra = harm(vtake(rn, t1_osc1wave=name), hz(rn))
        k = np.arange(1, len(ra) + 1)
        if wave == 1: ra, k = ra[::2], k[::2]
        refs.append((k * hz(rn), ra * k))
    rf = np.concatenate([r[0] for r in refs]); rv = np.concatenate([r[1] for r in refs])
    o = np.argsort(rf); rf, rv = rf[o], rv[o]
    rv = np.exp(np.convolve(np.log(rv), np.ones(9) / 9, mode="same"))
    out[name] = {}
    print(name, ": each harmonic x k, against the same frequency on low notes, dB")
    for note in range(60, 112, 2):
        f0 = hz(note)
        a = harm(vtake(note, t1_osc1wave=name), f0)
        k = np.arange(1, len(a) + 1)
        if wave == 1: a, k = a[::2], k[::2]
        d = 20 * np.log10(a * k / np.interp(k * f0, rf, rv))
        out[name][note] = [(float(kk * f0), float(dd)) for kk, dd in zip(k, d)]
        print("  note %3d %6.0f Hz  " % (note, f0) + " ".join("%5.1f" % v for v in d[:14]))
json.dump(out, open(__import__("os").path.join(__import__("os").path.dirname(__import__("os").path.abspath(__file__)), "rolloff.json"), "w"))
