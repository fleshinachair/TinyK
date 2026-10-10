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
for c1 in (96, 64):
    for note in (45, 69):
        a = 20 * np.log10(harm(vtake(note, t1_osc1wave="Vox", t1_osc1control1=float(c1)), hz(note)) / vu + 1e-9)
        b = 20 * np.log10(harm(etake(note, wave1=4 / 6.0, osc1_ctrl1=c1 / 127.0), hz(note)) / eu + 1e-9)
        f0 = hz(note); k = np.arange(1, len(a) + 1)
        sel = np.unique(np.round(np.geomspace(1, len(a), 14)).astype(int)) - 1
        print("c1 %3d note %3d  Hz:   " % (c1, note) + " ".join("%6.0f" % ((i + 1) * f0) for i in sel))
        print("          plug-in:     " + " ".join("%6.1f" % a[i] for i in sel))
        print("          engine:      " + " ".join("%6.1f" % b[i] for i in sel))
