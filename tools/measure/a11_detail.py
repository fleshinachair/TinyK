import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
from scipy.io import wavfile
progs = load_programs(BANK); p = progs[0]; t = 38
W=["Saw","Pulse","Tri","Sine","Vox","DWGS","Noise","AudioIn"]
print("A.11 Clouds: osc1 %s ctrl %d/%d dwgs %d | osc2 wave %d mod %d semi %+d tune %+d | levels %d/%d/%d | filter %s cut %d res %d egint %+d kbd %+d | amp %d dist %d pan %d | assign %d unison det %d | transpose %+d vib %+d bend %+d" % (
    W[p[t+7]&7], p[t+8], p[t+9], p[t+10], p[t+12]&3, (p[t+12]>>4)&3, p[t+13]-64, p[t+14]-64, p[t+16], p[t+17], p[t+18], ["24LPF","12LPF","12BPF","12HPF"][p[t+19]&3], p[t+20], p[t+21], p[t+22]-64, p[t+24]-64, p[t+25], p[t+27]&1, p[t+26], p[t+1]>>6, p[t+2], p[t+5]-64, p[t+6]-64, p[t+4]-64))
print("  EG1 %s EG2 %s | LFO1 wave %d rate %d sync %d | LFO2 wave %d rate %d sync %d | patches %s" % (list(p[t+30:t+34]), list(p[t+34:t+38]), p[t+38]&3, p[t+39], p[t+40]>>7, p[t+41]&3, p[t+42], p[t+43]>>7,
      [(["EG1","EG2","LFO1","LFO2","Vel","Kbd","Bend","Wheel"][p[t+44+2*i]&15], ["pitch","osc2","ctrl1","noise","cutoff","amp","pan","lfo2f"][p[t+44+2*i]>>4], p[t+45+2*i]-64) for i in range(4) if p[t+45+2*i]!=64]))
print("  FX: delay sync %d base %d time %d depth %d type %d | modfx speed %d depth %d type %d | EQ hi %d/%+d low %d/%+d | arp on %d" % (p[19]>>7, p[19]&15, p[20], p[21], p[22], p[23], p[24], p[25], p[26], p[27]-64, p[28], p[29]-64, p[32]>>7))
edges = [50, 100, 200, 400, 800, 1600, 3200, 6400, 12800, 20000]
def bands(x):
    m = x.mean(axis=1); n = 8192
    seg = m[:len(m)//n*n].reshape(-1, n) * np.hanning(n)
    pw = (np.abs(np.fft.rfft(seg, axis=1))**2).mean(axis=0); f = np.fft.rfftfreq(n, 1/SR)
    b = np.array([pw[(f>=lo)&(f<hi)].sum() for lo, hi in zip(edges[:-1], edges[1:])])
    return 10*np.log10(b/b.sum()+1e-12)
for tag in ("A11_dry", "A11"):
    a = wavfile.read(ROOT + "/output/ab_a11/%s_plugin.wav" % tag)[1].astype(float); b = wavfile.read(ROOT + "/output/ab_a11/%s_tinyk.wav" % tag)[1].astype(float)
    print(tag, "plug-in minus engine per octave band, dB:   " + " ".join("%5d" % e for e in edges[:-1]))
    for name, t0, n in (("C3", 0.0, 2.0), ("C4", 3.5, 2.0), ("chord", 7.0, 3.0), ("C5", 11.5, 2.0)):
        i0, i1 = int(t0*SR), int((t0+n)*SR)
        d = bands(a[i0:i1]) - bands(b[i0:i1])
        print("   %-6s                                        %s" % (name, " ".join("%5.1f" % v for v in d)))
