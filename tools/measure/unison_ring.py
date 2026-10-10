"""Do a unison stack's filters sit at one frequency? Nothing but the note-on kick ringing a 12LPF at resonance 127
(oscillator level 0), Unison with detune 99, key track 0 / +63; and how the ring's level spreads over takes."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst = make_vst()
vst.set(t1_osc1wave="Saw", t1_osc1level=0.0, t1_filtertype="12LPF", t1_cutoff=50.0, t1_resonance=127.0, t1_unisondetune=99.0,
        t1_ampegsustain=127.0, t1_distortion=False)
def peaks(x):
    n = 1 << 17
    s = np.abs(np.fft.rfft(x[:n] * np.hanning(n), n)); f = np.fft.rfftfreq(n, 1 / SR)
    out = []
    s2 = s.copy()
    for _ in range(6):
        k = int(np.argmax(s2))
        if s2[k] < 0.05 * s.max(): break
        out.append((f[k], 20 * np.log10(s2[k] / s.max())))
        s2[max(0, k - 12):k + 13] = 0
    return sorted(out)
for assign in ("Mono", "Unison"):
    for kt in (0.0, 63.0):
        vst.set(t1_voiceassign=assign, t1_filterkbdtrack=kt)
        vst.render(72, 0.05, 0.3)
        lv = []
        for i in range(8):
            x = vst.render(72, 3.2, 3.2)[int(0.1 * SR):]
            lv.append(10 * np.log10((x ** 2).mean() + 1e-12))
            if i == 0: pk = peaks(x)
        print("%-6s key track %+3d: ring peaks %s   level over 8 takes %s" % (assign, kt, "  ".join("%.1f Hz (%.0f dB)" % p for p in pk),
              " ".join("%.0f" % v for v in sorted(lv))))
