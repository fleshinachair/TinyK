"""How often a bank's programs use the features under study (vibrato int, bend range, EG reset, Vox, ring on a
sine, patches into LFO2 FREQ)."""
import sys, collections
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from h import BANK, load_programs
progs = load_programs(BANK)
W = ["Saw","Pulse","Tri","Sine","Vox","DWGS","Noise","AudioIn"]
def lab(i): return "%s.%d%d" % ("AB"[i//64], (i%64)//8+1, i%8+1)
vib = collections.Counter(); bend = collections.Counter(); egr = collections.Counter()
vox=[]; sinering=[]; synclfo2=[]; anysync=[]
for i,p in enumerate(progs):
    mode=(p[16]>>4)&3
    if mode==3: continue
    name=bytes(p[:12]).decode("ascii","replace").strip()
    for n,t in enumerate((38,146)[:2 if mode==2 else 1],1):
        vib[p[t+6]-64]+=1; bend[p[t+4]-64]+=1
        egr[((p[t+1]>>5)&1,(p[t+1]>>4)&1, p[t+1]>>6, (p[t+1]>>3)&1)]+=1
        w=W[p[t+7]&7]
        if w=="Vox": vox.append((lab(i),name,n,p[t+8],p[t+9],p[t+5]-64))
        mod=(p[t+12]>>4)&3
        if w=="Sine" and mod in (1,3) and p[t+17]: sinering.append((lab(i),name,n,mod,p[t+8],p[t+9],p[t+16],p[t+17]))
        l2sync = p[t+43]>>7
        for k in range(4):
            route,amt=p[t+44+2*k],p[t+45+2*k]-64
            if amt and (route>>4)==7:
                synclfo2.append((lab(i),name,n,"src",route&15,"int",amt,"lfo2sync",l2sync,"note",p[t+43]&31))
print("vibrato int:",sorted(vib.items()))
print("bend range:",sorted(bend.items()))
print("(ampEGreset, filtEGreset, assign, trig):",sorted(egr.items()))
print("vox:",*vox,sep="\n  ")
print("sine+ring:",*sinering,sep="\n  ")
print("patch->LFO2 freq:",*synclfo2,sep="\n  ")
