import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
N = 1 << 16
def vtake(note, **kw):
    vst.set(**kw); vst.render(note, 0.05, 0.2); return vst.render(note, 2.2, 2.2)[int(0.4 * SR):]
def etake(note, **kw):
    return eng.render_patch(0, patch(**kw), note, 2.2, 2.2)[int(0.4 * SR):]
def harm(x, f0, top=21000):
    s, f = spectrum(x, N)
    k = np.arange(1, int(top / f0) + 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    return np.array([s[b - 3:b + 4].max() for b in bins])
vu = harm(vtake(45, t1_osc1wave="Sine"), 110.0)[0]
eu = harm(etake(45, wave1=3 / 6.0), 110.0)[0]
pts = [500, 1000, 2000, 3000, 5000, 7000, 9000, 11000, 13000, 15000, 17000, 19000]
for wave, name in ((0, "Saw"), (1, "Square"), (5, "DWGS")):
    print(name, ": plug-in minus engine, dB, at the harmonic nearest each frequency (Hz): " + " ".join("%6d" % p for p in pts))
    for note in (33, 45, 57, 69, 81, 88, 93, 96, 100, 105):
        f0 = hz(note)
        a = 20 * np.log10(harm(vtake(note, t1_osc1wave=name), f0) / vu + 1e-9)
        b = 20 * np.log10(harm(etake(note, wave1=wave / 6.0), f0) / eu + 1e-9)
        k = np.arange(1, len(a) + 1)
        if wave == 1: a, b, k = a[::2], b[::2], k[::2]
        row = []
        for p in pts:
            i = int(np.argmin(np.abs(k * f0 - p)))
            row.append("%6.1f" % (a[i] - b[i]) if abs(k[i] * f0 - p) < 0.6 * f0 * (2 if wave == 1 else 1) and p >= f0 * 0.9 else "     .")
        print("  note %3d %6.0f Hz  %s" % (note, f0, " " * 24 + " ".join(row)))
