import sys, matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Circle
sys.path.insert(0,"../schematic"); import netlist as N
# parts: ref -> ((col,row)pin1,(col,row)pin2) pins follow netlist order
P={"R5":((0,0),(4,0)),"R6":((0,1),(4,1)),"R7":((0,2),(4,2)),"R8":((0,3),(4,3)),
   "R2":((0,4),(4,4)),"R1":((0,5),(4,5)),"R3":((0,6),(4,6)),"R4":((0,7),(4,7)),
   "C1":((5,0),(5,1)),"C2":((5,3),(5,4))}
# orientation: which net on which pad (netlist order n1,n2 -> pads)
SW={"R5":1,"R6":0,"R7":0,"R8":0,"R2":0,"R1":0,"R3":0,"R4":0,"C1":0,"C2":0}
# explicit pad nets
PAD={}
def setp(ref,a,b): PAD[P[ref][0]]=(ref,a); PAD[P[ref][1]]=(ref,b)
setp("R5","ADC","X"); setp("R6","ADC","VB"); setp("R7","3V3","VB"); setp("R8","GND","VB")
setp("R2","GND","A"); setp("R1","DRV","A"); setp("R3","DRV","REF"); setp("R4","GND","REF")
setp("C1","X","A"); setp("C2","VB","GND")
# bridges (adjacent pads, solder blob) and wires (insulated, back side)
BR=[((0,0),(0,1)),((4,1),(4,2)),((4,2),(4,3)),((0,3),(0,4)),((4,4),(4,5)),
    ((0,5),(0,6)),((4,6),(4,7)),((4,0),(5,0)),((4,3),(5,3))]
WI=[((5,1),(4,4)),((0,7),(0,4)),((5,4),(0,4))]
# verify
par={p:p for p in PAD}
def f(x):
    while par[x]!=x: par[x]=par[par[x]]; x=par[x]
    return x
for a,b in BR: assert abs(a[0]-b[0])+abs(a[1]-b[1])==1; par[f(a)]=f(b)
for a,b in WI: par[f(a)]=f(b)
# off-board leads solder onto existing pads
OFF={"GPIO3":(0,5),"GPIO0":(0,0),"GPIO4":(4,6),"3V3":(0,2),"GND":(0,3),"clip+":(4,4),"clip-":(0,4)}
want={}
for ref,k,v,a,b in N.COMPONENTS:
    pa,pb=P[ref]
    assert {PAD[pa][1],PAD[pb][1]}=={a,b},ref
groups={}
for p,(r,n) in PAD.items(): groups.setdefault(f(p),set()).add(n)
ok=all(len(s)==1 for s in groups.values())
nets=[next(iter(s)) for s in groups.values()]
ok&=len(nets)==len(set(nets))
for k,p in OFF.items():
    exp={"GPIO3":"DRV","GPIO0":"ADC","GPIO4":"REF","3V3":"3V3","GND":"GND","clip+":"A","clip-":"GND"}[k]
    ok&=PAD[p][1]==exp or next(iter(groups[f(p)]))==exp
print("CHECK:","OK" if ok else "FAIL",sorted(nets))
# draw
fig,ax=plt.subplots(figsize=(5.4,7.4))
NC,NR=8,10; ox,oy=1.2,0.8
ax.add_patch(Rectangle((-0.6,-0.6),NC+0.2+1.8,NR+0.2,fc="#c8a46a",ec="#6b5230",lw=2))
for c in range(-0,NC+2):
    for r in range(NR): ax.add_patch(Circle((c-0.5+0,r),0.12,fc="#3b2f1a"))
def xy(p): return (p[0]+0.5,p[1]+0.5-0.5+0.5)
def xy(p): return (p[0]+0.5-0.5+0.5, p[1]+1)
cols={"ADC":"#1f77b4","X":"#9467bd","VB":"#2ca02c","3V3":"#d62728","GND":"#333","A":"#ff7f0e","DRV":"#8c564b","REF":"#17becf"}
for ref,(a,b) in P.items():
    xa,ya=xy(a);xb,yb=xy(b)
    if ref[0]=="R":
        ax.add_patch(Rectangle((xa+0.5,ya-0.28),xb-xa-1,0.56,fc="#e8d9a8",ec="k",zorder=3))
        ax.text((xa+xb)/2,ya,f"{ref} {dict(R1='330',R2='10',R3='5.6k',R4='1k',R5='2.2k',R6='10k',R7='47k',R8='10k')[ref]}",ha="center",va="center",fontsize=8,zorder=4)
    else:
        ax.add_patch(Rectangle((xa-0.4,ya-0.4),0.8,yb-ya+0.8,fc="#2b4b8a",ec="k",zorder=3))
        ax.text(xa,(ya+yb)/2,ref,rotation=90,color="w",ha="center",va="center",fontsize=8,zorder=4)
        ax.text(xa+0.0,ya-0.55,"+" if ref=="C1" else "",fontsize=11,ha="center")
    for p in (a,b):
        x,y=xy(p); ax.add_patch(Circle((x,y),0.2,fc=cols[PAD[p][1]],ec="k",zorder=5))
ax.text(xy(P["C2"][0])[0],xy(P["C2"][0])[1]-0.55,"+",fontsize=11,ha="center")
for a,b in BR:
    (x1,y1),(x2,y2)=xy(a),xy(b); ax.plot([x1,x2],[y1,y2],lw=8,color="#bbb",solid_capstyle="round",zorder=6,alpha=.9)
for i,(a,b) in enumerate(WI):
    (x1,y1),(x2,y2)=xy(a),xy(b)
    dx=0.45*(i+1)*(-1 if x2<x1 else 1)
    ax.plot([x1,x2],[y1,y2],ls="--",lw=2.2,color="#e0218a",zorder=2.5,alpha=.8)
for k,p in OFF.items():
    x,y=xy(p); side=-1 if p[0]==0 else 1
    ax.annotate(k,(x,y),xytext=(x+side*(1.5 if side<0 else 3.0),y+(0.0 if k not in("GND","clip-") else 0.0)),fontsize=8,fontweight="bold",va="center",ha="center",arrowprops=dict(arrowstyle="-",color="k"),zorder=8)
ax.set_xlim(-1.2,NC+1.6);ax.set_ylim(NR+0.2,-1.0);ax.set_aspect("equal");ax.axis("off")
ax.set_title("Perfboard 6 cols x 8 rows (top view)\ngrey = solder bridge, pink dashed = insulated wire (back)",fontsize=9)
plt.savefig("perfboard.png",dpi=160,bbox_inches="tight")
