"""A.78 on the engine, twelve notes in a row (each a new draw of the note-on kick): the level of each, against the
plug-in's spread over as many takes."""
import sys; sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import *
vst, eng = make_vst(), make_eng()
progs = load_programs(BANK); idx = 6 * 8 + 7
prog = bytearray(progs[idx]); prog[21] = prog[24] = 0; prog[27] = prog[29] = 64; prog = bytes(prog)
p = parse_program(idx, prog)
ev = []
for i in range(12):
    ev += [(i * 4.0, 0x90, 48, 100), (i * 4.0 + 1.0, 0x80, 48, 0)]
x = eng.render_midi(0, p, ev, 48.0)
lev = lambda y: 10 * np.log10((y ** 2).mean() + 1e-12)
print("engine, 12 notes : " + " ".join("%6.1f" % lev(x[int(i * 4.0 * SR):int((i * 4.0 + 1.0) * SR)]) for i in range(12)))
vst.load_program(prog); vst.render(48, 0.05, 0.3)
print("plug-in, 12 takes: " + " ".join("%6.1f" % lev(vst.render(48, 1.0, 1.0)) for i in range(12)))
