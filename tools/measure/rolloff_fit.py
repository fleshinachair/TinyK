"""Fit cheap filters to the measured roll-off (rolloff.json): n cascaded one-poles whose coefficient depends on
the oscillator's phase increment dt = f0 / fs only."""
import json, os, sys
import numpy as np
from scipy.optimize import minimize
SR = 44100.0
d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "rolloff.json")))
pts = []
for name in ("Saw", "Square"):
    for note, rows in d[name].items():
        f0 = 440.0 * 2 ** ((int(note) - 69) / 12.0)
        if f0 < 600: continue
        for f, db in rows:
            if db > -22 and f < 20000: pts.append((f0, f, db))
pts = np.array(pts)
f0, f, db = pts.T
print(len(pts), "points")
# 1. the law itself: dB = -(f f0 / X)^p
def law(p):
    X, e = p
    return -np.abs(f * f0 / X) ** e
r = minimize(lambda p: np.mean((law(p) - db) ** 2), [8.45e6, 2.0], method="Nelder-Mead")
print("law dB = -(f f0 / %.3g)^%.2f: rms %.2f dB" % (r.x[0], r.x[1], np.sqrt(r.fun)))
def tpt(g, f):      # bilinear one-pole, magnitude in dB
    return -10 * np.log10(1 + (np.tan(np.pi * f / SR) / g) ** 2)
def rc(a, f):       # y += a (x - y)
    w = 2 * np.pi * f / SR
    return 10 * np.log10(a * a / (1 - 2 * (1 - a) * np.cos(w) + (1 - a) ** 2))
dt = f0 / SR
for n in (1, 2, 3, 4):
    for kind in ("tpt", "rc"):
        def model(p):
            c, e = p
            g = c / dt ** e
            if kind == "tpt": return n * tpt(g, f)
            a = g / (1 + g)
            return n * rc(a, f)
        best = None
        for c0 in (0.01, 0.03, 0.1):
            for e0 in (1.0, 1.5):
                r = minimize(lambda p: np.mean((model(p) - db) ** 2), [c0, e0], method="Nelder-Mead")
                if best is None or r.fun < best.fun: best = r
        r1 = minimize(lambda p: np.mean((model([p[0], 1.0]) - db) ** 2), [best.x[0]], method="Nelder-Mead")
        print("%d x %s: g = %.4g / dt^%.3f  rms %.2f dB   (with dt^1: g = %.4g / dt, rms %.2f)" % (n, kind, best.x[0], best.x[1], np.sqrt(best.fun), r1.x[0], np.sqrt(r1.fun)))

def two_pole(p):
    c, e, k = p
    g = c / dt ** e
    om = np.tan(np.pi * f / SR) / g
    return -10 * np.log10((1 - om ** 2) ** 2 + (k * om) ** 2)
def report(name, m):
    sh, dp = db > -6, db <= -6
    print("%-40s rms %.2f dB all, %.2f above -6 dB, %.2f below" % (name, np.sqrt(np.mean((m - db) ** 2)),
          np.sqrt(np.mean((m - db)[sh] ** 2)), np.sqrt(np.mean((m - db)[dp] ** 2))))
best = None
for c0 in (0.003, 0.01, 0.03):
    for k0 in (1.0, 1.4, 2.0):
        r = minimize(lambda p: np.mean((two_pole(p) - db) ** 2), [c0, 1.8, k0], method="Nelder-Mead", options={"maxiter": 4000})
        if best is None or r.fun < best.fun: best = r
print("two-pole: g = %.4g / dt^%.3f, k = %.3f" % tuple(best.x)); report("two-pole", two_pole(best.x))
for e_fixed in (2.0,):
    b2 = None
    for c0 in (0.001, 0.003, 0.01):
        r = minimize(lambda p: np.mean((two_pole([p[0], e_fixed, p[1]]) - db) ** 2), [c0, 1.4], method="Nelder-Mead")
        if b2 is None or r.fun < b2.fun: b2 = r
    print("two-pole, dt^%.0f: g = %.4g / dt^2, k = %.3f" % (e_fixed, b2.x[0], b2.x[1])); report("two-pole dt^2", two_pole([b2.x[0], e_fixed, b2.x[1]]))
report("one tpt pole, g = 0.0009541 / dt^2.051", tpt(0.0009541 / dt ** 2.051, f))
r = minimize(lambda p: np.mean((tpt(p[0] / dt ** 2, f) - db) ** 2), [0.001], method="Nelder-Mead")
print("one pole dt^2: g = %.4g / dt^2" % r.x[0]); report("one tpt pole dt^2", tpt(r.x[0] / dt ** 2, f))
report("the law", law([7.16e6, 1.72]))
# per note view of the two-pole dt^2 fit
m = two_pole([b2.x[0], 2.0, b2.x[1]]); m1 = tpt(r.x[0] / dt ** 2, f)
for n0 in sorted(set(np.round(f0))):
    s = (np.round(f0) == n0) & (np.array([p in d["Saw"].get(str(int(round(69 + 12 * np.log2(n0 / 440)))), [[0, 0]])[0] or True for p in f]))
    if n0 in (1047, 1319, 1661, 2093, 2637, 3322, 4186):
        idx = np.nonzero(np.round(f0) == n0)[0][:8]
        print("f0 %5d  measured %s\n          2-pole   %s\n          1-pole   %s" % (n0, " ".join("%5.1f" % db[i] for i in idx),
              " ".join("%5.1f" % m[i] for i in idx), " ".join("%5.1f" % m1[i] for i in idx)))

kk = np.round(f / f0)
for wname, w in (("unweighted", np.ones_like(db)), ("1/k", 1.0 / kk), ("1/k^2", 1.0 / kk ** 2)):
    r = minimize(lambda p: np.sum(w * (tpt(p[0] / dt ** 2, f) - db) ** 2), [0.001], method="Nelder-Mead")
    m = tpt(r.x[0] / dt ** 2, f)
    print("one pole, weight %-10s g = %.4g / dt^2   k=1: %.2f  k=2: %.2f  k=3..5: %.2f  k>5: %.2f dB rms" % (wname, r.x[0],
          *[np.sqrt(np.mean((m - db)[s] ** 2)) for s in (kk == 1, kk == 2, (kk >= 3) & (kk <= 5), kk > 5)]))
