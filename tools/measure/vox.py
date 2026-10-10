import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng(tuple(sys.argv[1:]))
N = 1 << 16
def vtake(note, **kw):
    vst.set(**kw); vst.render(note, 0.05, 0.2); return vst.render(note, 2.2, 2.2)[int(0.4 * SR):]
def etake(note, **kw):
    return eng.render_patch(0, patch(**kw), note, 2.2, 2.2)[int(0.4 * SR):]
def harm(x, f0):
    s, f = spectrum(x, N)
    k = np.arange(1, int(18000 / f0) + 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    return np.array([s[b - 3:b + 4].max() for b in bins])
vu = harm(vtake(45, t1_osc1wave="Sine"), 110.0)[0]
eu = harm(etake(45, wave1=3 / 6.0), 110.0)[0]
print("rms dB between harmonic levels (harmonics within 40 dB of the strongest), and the level difference (plug-in - engine, dB)")
print("note      " + "".join("   c1=%-3d      " % c for c in (0, 16, 32, 64, 96, 127)))
for note in (45, 57, 69, 76, 81, 84, 88, 93, 96, 100, 105):
    row = []
    for c1 in (0, 16, 32, 64, 96, 127):
        a = harm(vtake(note, t1_osc1wave="Vox", t1_osc1control1=float(c1)), hz(note)) / vu
        b = harm(etake(note, wave1=4 / 6.0, osc1_ctrl1=c1 / 127.0), hz(note)) / eu
        da, db = 20 * np.log10(a + 1e-9), 20 * np.log10(b + 1e-9)
        m = (da > da.max() - 40) | (db > db.max() - 40)
        row.append("%5.1f (%+5.1f)" % (np.sqrt(np.mean((da - db)[m] ** 2)), 10 * np.log10((a ** 2).sum() / (b ** 2).sum())))
    print("%3d %6.0f " % (note, hz(note)) + "  ".join(row))
