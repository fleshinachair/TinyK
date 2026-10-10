"""A saw through each filter type, plug-in minus engine per frequency, for the old and the new tilt."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
N = 1 << 16
KINDS = ["24LPF", "12LPF", "12BPF", "12HPF"]
def curve(x, f0):
    s, f = spectrum(x, N)
    k = np.arange(2, int(19500 / f0) + 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    return k * f0, 20 * np.log10(np.array([s[b - 3:b + 4].max() for b in bins]) * k + 1e-12)
grid = np.array([500, 1000, 2000, 4000, 6000, 8000, 10000, 12000, 14000, 16000, 18000.0])
print("plug-in minus engine, dB at kHz:      " + " ".join("%5.1f" % (g / 1000) for g in grid))
for kind in KINDS:
    for cut, res in ((127, 0), (127, 50), (100, 20), (80, 0)):
        if kind == "12HPF": cut = {127: 0, 100: 20, 80: 40}[cut]
        vst.set(t1_osc1wave="Saw", t1_filtertype=kind, t1_cutoff=float(cut), t1_resonance=float(res))
        vst.render(33, 0.05, 0.2)
        fr, dv = curve(vst.render(33, 2.2, 2.2)[int(0.4 * SR):], hz(33))
        for tag, db, thz in (("old", 26.7, 20000.0), ("new", 12.43, 12726.0)):
            eng.set_tuning({"tilt_db": db, "tilt_hz": thz})
            x = eng.render_patch(0, patch(wave1=0.0, filter_type=KINDS.index(kind) / 3.0, cutoff=cut / 127.0, resonance=res / 127.0), 33, 2.2, 2.2)[int(0.4 * SR):]
            fe, de = curve(x, hz(33))
            d = np.convolve(dv - de, np.ones(9) / 9, mode="same")
            d -= d[(fr > 300) & (fr < 1500)].mean() if kind != "12HPF" or cut == 0 else d[(fr > 300) & (fr < 1500)].mean()
            print("%s cut %3d res %2d  %s  %s" % (kind, cut, res, tag, " ".join("%5.1f" % v for v in np.interp(grid, fr, d))))
