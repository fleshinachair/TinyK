import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
N = 1 << 16
def vtake(note, **kw):
    vst.set(**kw); vst.render(note, 0.05, 0.2); return vst.render(note, 2.2, 2.2)[int(0.4 * SR):]
def etake(note, **kw):
    return eng.render_patch(0, patch(**kw), note, 2.2, 2.2)[int(0.4 * SR):]
def bands(x, f0):
    s, f = spectrum(x, N)
    k = np.arange(1, int(22000 / f0) + 1)
    bins = np.round(k * f0 * N / SR).astype(int)
    bins = bins[bins < len(s) - 9]
    mask = np.ones(len(s), bool)
    hp = 0.0
    for b in bins:
        mask[max(b - 8, 0):b + 9] = False
        hp += (s[max(b - 8, 0):b + 9] ** 2).sum()
    out = []
    for lo, hi in ((30, 5000), (5000, 10000), (10000, 13000), (13000, 16000), (16000, 20000)):
        m = mask & (f >= lo) & (f < hi)
        out.append(10 * np.log10((s[m] ** 2).sum() / hp + 1e-20))
    return out
waves = [(0, "Saw"), (1, "Square"), (2, "Triangle")] if len(sys.argv) < 2 else [(int(sys.argv[1]), sys.argv[2])]
for wave, vname in waves:
    print(vname, " off-harmonic power re the tone, dB, in 0-5k / 5-10k / 10-13k / 13-16k / 16-20k:   plug-in   |   engine")
    for note in (33, 45, 57, 60, 67, 72, 76, 79, 84, 88, 91, 96, 100, 103, 108):
        f0 = hz(note)
        a = bands(vtake(note, t1_osc1wave=vname), f0)
        b = bands(etake(note, wave1=wave / 6.0), f0)
        print("  note %3d %7.1f Hz   %s  |  %s" % (note, f0, " ".join("%6.1f" % v for v in a), " ".join("%6.1f" % v for v in b)))
