"""The brightness tilt against the plug-in's top end: a saw and a square through the open high-pass on both, each
harmonic x k, levelled at 300-1500 Hz. Prints the difference for the shipped tilt and fits tilt_db / tilt_hz."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
from scipy.optimize import minimize
vst, eng = make_vst(), make_eng()
N = 1 << 16
def vtake(note, **kw):
    vst.set(**kw); vst.render(note, 0.05, 0.2); return vst.render(note, 2.2, 2.2)[int(0.4 * SR):]
def etake(note, **kw):
    return eng.render_patch(0, patch(**kw), note, 2.2, 2.2)[int(0.4 * SR):]
def curve(x, f0, odd):
    s, f = spectrum(x, N)
    k = np.arange(2 if not odd else 3, int(19500 / f0) + 1, 2 if odd else 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    a = np.array([s[b - 3:b + 4].max() for b in bins]) * k
    fr = k * f0
    d = 20 * np.log10(a)
    return fr, d - d[(fr > 300) & (fr < 1500)].mean()
CASES = [(33, "Saw", 0), (45, "Saw", 0), (40, "Square", 1), (52, "Square", 1)]
ref = [curve(vtake(n, t1_osc1wave=w), hz(n), i) for n, w, i in CASES]
grid = np.array([500, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000, 17000, 18000, 19000.0])
def diff(db, hz_):
    eng.set_tuning({"tilt_db": db, "tilt_hz": hz_})
    out = []
    for (n, w, i), (fr, d) in zip(CASES, ref):
        fe, de = curve(etake(n, wave1=i / 6.0), hz(n), i)
        sm = lambda v: np.convolve(v, np.ones(7) / 7, mode="same")
        out.append(np.interp(grid, fr, sm(d - de)))
    return np.mean(out, axis=0)
def show(tag, db, hz_):
    d = diff(db, hz_)
    print("%-28s %s   rms %.2f" % (tag, " ".join("%5.1f" % v for v in d), np.sqrt(np.mean(d ** 2))))
print("plug-in minus engine, dB at   " + " ".join("%5.1f" % (g / 1000) for g in grid) + " kHz")
show("shipped (26.7 dB, 20 kHz)", 26.7, 20000.0)
r = minimize(lambda p: np.mean(diff(p[0], p[1] * 1000.0) ** 2), [13.3, 13.6], method="Nelder-Mead", options={"xatol": 0.02, "fatol": 1e-4})
show("fitted (%.2f dB, %.0f Hz)" % (r.x[0], r.x[1] * 1000), r.x[0], r.x[1] * 1000.0)
