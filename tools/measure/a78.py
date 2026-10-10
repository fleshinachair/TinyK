"""A.78 reznotes: how far the plug-in spreads from take to take, and where the engine sits. Also with the program's
distortion, unison and resonance taken off one at a time, to see which part carries the difference."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
progs = load_programs(BANK)
idx = 0 * 64 + 6 * 8 + 7
def lev(x, a=0.0, b=2.0): return 10 * np.log10((x[int(a * SR):int(b * SR)] ** 2).mean() + 1e-12)
def run(tag, edit=None, note=48):
    prog = bytearray(progs[idx]); prog[21] = prog[24] = 0; prog[27] = prog[29] = 64
    if edit: edit(prog)
    prog = bytes(prog)
    vst.load_program(prog); vst.render(note, 0.05, 0.3)
    v = [vst.render(note, 2.0, 3.0) for _ in range(9)]
    e = eng.render_patch(0, parse_program(idx, prog), note, 2.0, 3.0)
    lv = sorted(lev(x) for x in v)
    print("%-34s plug-in %6.1f .. %6.1f (median %6.1f) dB   engine %6.1f   first 50 ms: %6.1f | %6.1f   0.5-2 s: %6.1f | %6.1f" % (
        tag, lv[0], lv[-1], lv[4], lev(e), np.median([lev(x, 0, 0.05) for x in v]), lev(e, 0, 0.05), np.median([lev(x, 0.5, 2.0) for x in v]), lev(e, 0.5, 2.0)))
p = progs[idx]; t = 38
print("A.78: osc1 wave %d ctrl %d/%d, levels %d/%d/%d, filter %d cut %d res %d egint %+d kbd %+d, amp %d dist %d, assign %d, EG1 %s EG2 %s" % (
    p[t+7]&7, p[t+8], p[t+9], p[t+16], p[t+17], p[t+18], p[t+19]&3, p[t+20], p[t+21], p[t+22]-64, p[t+24]-64, p[t+25], p[t+27]&1, p[t+1]>>6, list(p[t+30:t+34]), list(p[t+34:t+38])))
run("as stored")
def nodist(q): q[t+27] &= 0xFE
def poly(q): q[t+1] = (q[t+1] & 0x3F) | 0x40
def res60(q): q[t+21] = 60
def noeg(q): q[t+22] = 64
run("distortion off", nodist)
run("Poly instead of Unison", poly)
run("resonance 60", res60)
run("filter EG int 0", noeg)
run("as stored, C4", None, 60)
run("as stored, C2", None, 36)
