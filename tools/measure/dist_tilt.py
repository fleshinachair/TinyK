"""Where the brightness tilt sits against the distortion: a SINE (no harmonics of its own) clipped hard, its
harmonics on the plug-in and on the engine. If the tilt belongs to the oscillators, the clip's harmonics carry
none of it."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
N = 1 << 16
def harm(x, f0):
    s, f = spectrum(x, N)
    k = np.arange(1, int(20000 / f0) + 1, 2)
    bins = np.round(k * f0 * N / SR).astype(int)
    return k * f0, 20 * np.log10(np.array([s[b - 3:b + 4].max() for b in bins]) + 1e-12)
vst.set(t1_osc1wave="Sine", t1_distortion=True, t1_amplevel=127.0)
pts = [1, 3, 5, 7, 9, 11, 15, 21, 29, 37, 45]
for note in (57, 69, 81):
    f0 = hz(note)
    vst.render(note, 0.05, 0.2)
    fr, a = harm(vst.render(note, 2.2, 2.2)[int(0.4 * SR):], f0)
    fe, b = harm(eng.render_patch(0, patch(wave1=0.5, drive=0.5, amp_level=1.0, level=1.0), note, 2.2, 2.2)[int(0.4 * SR):], f0)
    a -= a[0]; b -= b[0]
    sel = [i for i, k in enumerate(range(1, 2 * len(fr), 2)) if k in pts]
    print("note %d (%.0f Hz), odd harmonics re the fundamental, dB" % (note, f0))
    print("   Hz       " + " ".join("%6.0f" % fr[i] for i in sel))
    print("   plug-in  " + " ".join("%6.1f" % a[i] for i in sel))
    print("   engine   " + " ".join("%6.1f" % b[i] for i in sel))
    print("   square   " + " ".join("%6.1f" % (-20 * np.log10(fr[i] / f0)) for i in sel))
