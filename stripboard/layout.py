"""Verify a stripboard layout against schematic/netlist.py and draw it.

verification model (independent of the search): holes in a grid, strips = rows, cuts in empty holes split a strip into segments;
every lead sits in a hole; jumpers join two holes; nets are read back from segment membership and compared with netlist.py.
usage: python3 layout.py layout.json out_basename
"""
import sys, os, json, string
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "schematic"))
from netlist import COMPONENTS, POLAR
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Circle, Rectangle

WIRES = [("GPIO0", "ADC"), ("GPIO3", "DRV"), ("GPIO4", "REF"), ("3V3", "3V3"), ("GND-board", "GND"), ("clip+", "A"), ("clip-", "GND")]
CODE = {"GPIO0": "G0", "GPIO3": "G3", "GPIO4": "G4", "3V3": "3V", "GND-board": "GN", "clip+": "C+", "clip-": "C−"}
WIRE_COL = {"GPIO0": "#1f77b4", "GPIO3": "#d62728", "GPIO4": "#9467bd", "3V3": "#ff7f0e", "GND-board": "#444444", "clip+": "#c49c00", "clip-": "#222222"}


def tok(ref, pin):                       # terminal token; polar parts keep pin number
    return f"{ref}.{pin}" if ref in POLAR else ref


def load(path):
    d = json.load(open(path))
    R, N = d["R"], d["N"]
    leads = {}                           # (r,c) -> (token, intended_net)
    comps = []
    for (ref, kind, val, n1, n2), (c, t, f) in zip(COMPONENTS, d["comp"]):
        top, bot = ((n1, 1), (n2, 2)) if not f else ((n2, 2), (n1, 1))
        for (r, (net, pin)) in ((t, top), (t + 1, bot)):
            assert (r, c) not in leads, f"hole collision at {(r, c)}"
            leads[(r, c)] = (tok(ref, pin), net)
        comps.append(dict(ref=ref, kind=kind, val=val, col=c, top=t, flip=f, pins=(top, bot)))
    wires = []
    for (name, net), (r, c) in zip(WIRES, d["wire"]):
        assert (r, c) not in leads, f"hole collision at {(r, c)}"
        leads[(r, c)] = (name, net)
        wires.append(dict(name=name, net=net, r=r, c=c))
    jumps = []
    for (r1, c1, r2, c2, net, used) in d["jump"]:
        if used:
            for (r, c) in ((r1, c1), (r2, c2)):
                if (r, c) in leads and leads[(r, c)][1] == net and not leads[(r, c)][0].startswith(("J", "R", "C")):
                    continue                       # pigtail soldered into the same hole as an off-board wire (same net)
                assert (r, c) not in leads, f"hole collision at {(r, c)}"
                leads[(r, c)] = (f"J{len(jumps)}", net)
            jumps.append(dict(a=(r1, c1), b=(r2, c2), net=net))
    for j in ([] if d.get("long") else jumps):         # physical rule for jumpers (same as the search)
        (r1, c1), (r2, c2) = j["a"], j["b"]
        assert abs(r1 - r2) <= 1 and abs(c1 - c2) <= 2, "jumper too long"
        for cc in range(min(c1, c2) + 1, max(c1, c2)):
            assert (r1, cc) not in leads and (r2, cc) not in leads, "jumper passes over an occupied hole"
    return R, N, leads, comps, wires, jumps


def derive_cuts(R, N, leads):
    cuts = set()
    for r in range(R):
        occ = sorted(c for (rr, c) in leads if rr == r)
        for c1, c2 in zip(occ, occ[1:]):
            if leads[(r, c1)][1] != leads[(r, c2)][1]:
                assert c2 - c1 >= 2, "no room for a cut"
                cuts.add((r, (c1 + c2) // 2))
    return cuts


def extract(R, N, leads, jumps, cuts):
    par = {}
    def find(x):
        par.setdefault(x, x)
        while par[x] != x:
            par[x] = par[par[x]]; x = par[x]
        return x
    seg = {}                              # hole -> segment id
    for r in range(R):
        sid = 0
        for c in range(N):
            if (r, c) in cuts:
                assert (r, c) not in leads, "cut on an occupied hole"
                sid += 1
            seg[(r, c)] = (r, sid)
    for h in leads:
        find(("S",) + seg[h])
    for j in jumps:
        par[find(("S",) + seg[j["a"]])] = find(("S",) + seg[j["b"]])
    nets = {}
    for h, (t, _) in leads.items():
        if t.startswith("J"):
            continue
        nets.setdefault(find(("S",) + seg[h]), set()).add(t)
    return {frozenset(s) for s in nets.values()}


def expected():
    exp = {}
    for ref, kind, val, n1, n2 in COMPONENTS:
        exp.setdefault(n1, set()).add(tok(ref, 1)); exp.setdefault(n2, set()).add(tok(ref, 2))
    for name, net in WIRES:
        exp.setdefault(net, set()).add(name)
    return {frozenset(s) for s in exp.values()}


def label(r, c):
    return f"{string.ascii_uppercase[r]}{c + 1}"


def verify(path):
    R, N, leads, comps, wires, jumps = load(path)
    cuts = derive_cuts(R, N, leads)
    got, want = extract(R, N, leads, jumps, cuts), expected()
    ok = got == want
    return ok, got, want, (R, N, leads, comps, wires, jumps, cuts)


NETCOL = {"GND": "#222", "3V3": "#c25e00", "VB": "#1b6f4a", "ADC": "#1f5fa8", "REF": "#6a3d9a", "DRV": "#b30000", "A": "#8a6d00", "X": "#555"}


def segments(R, N, leads, cuts):
    """[(row, first_col, last_col, net)] for every strip segment that carries a lead"""
    out = []
    for r in range(R):
        start = 0
        for c in range(N + 1):
            if c == N or (r, c) in cuts:
                nets = {leads[(r, cc)][1] for cc in range(start, c) if (r, cc) in leads}
                if nets:
                    assert len(nets) == 1
                    out.append((r, start, c - 1, nets.pop()))
                start = c + 1
    return out


def board_axes(ax, R, N, leads, cuts, mirror, title):
    ax.set_aspect("equal"); ax.axis("off")
    ax.set_xlim(-1.3, N + 0.3); ax.set_ylim(-R + 0.4, 1.5)
    X = (lambda c: N - 1 - c) if mirror else (lambda c: c)
    Y = lambda r: -r
    ax.text(N / 2 - 0.5, 1.28, title, ha="center", va="center", fontsize=17, fontweight="bold", color="#222")
    ax.add_patch(FancyBboxPatch((-0.6, -(R - 1) - 0.6), N - 1 + 1.2, R - 1 + 1.2, boxstyle="round,pad=0.0,rounding_size=0.2", fc="#e8d8a8", ec="#9a7b2f", lw=2))
    for r in range(R):
        ax.add_patch(Rectangle((-0.5, Y(r) - 0.33), N - 1 + 1.0, 0.66, fc="#d9a066", ec="none", alpha=0.9, zorder=1))
    for (r, c) in cuts:
        ax.add_patch(Rectangle((X(c) - 0.5, Y(r) - 0.36), 1.0, 0.72, fc="#e8d8a8", ec="none", zorder=2))
        for sgn in (1, -1):
            ax.plot([X(c) - 0.25, X(c) + 0.25], [Y(r) - 0.25 * sgn, Y(r) + 0.25 * sgn], color="#c00", lw=3, zorder=6)
    for r in range(R):
        for c in range(N):
            if (r, c) not in cuts:
                ax.add_patch(Circle((X(c), Y(r)), 0.11, fc="#fff8e0" if (r, c) not in leads else "#333", ec="#6b4f12", lw=1, zorder=3))
    for c in range(N):
        ax.text(X(c), 0.82, str(c + 1), ha="center", va="center", fontsize=13, color="#333")
    for r in range(R):
        ax.text(-0.95, Y(r), string.ascii_uppercase[r], ha="center", va="center", fontsize=13, color="#333")
    for (r, c1, c2, net) in (segments(R, N, leads, cuts) if mirror else []):
        xs = sorted((X(c1), X(c2)))
        ax.text((xs[0] + xs[1]) / 2, Y(r) + 0.47, net, ha="center", va="center", fontsize=11, color=NETCOL.get(net, "#333"), fontweight="bold", zorder=9)
    return X, Y


def draw(model, out):
    R, N, leads, comps, wires, jumps, cuts = model
    fig, (ax, ax2) = plt.subplots(2, 1, figsize=(2.0 + N * 1.25, 2 * (1.0 + R * 1.2) + 1.2), dpi=150, gridspec_kw={"height_ratios": [1, 1]})
    X, Y = board_axes(ax, R, N, leads, cuts, False, "COMPONENT SIDE (top view)")
    for cp in comps:                                        # standing parts
        c, t = cp["col"], cp["top"]
        x, y1, y2 = X(c), Y(t), Y(t + 1)
        fc = "#9ec5e8" if cp["kind"] == "C" else "#f2d29b"
        ax.add_patch(FancyBboxPatch((x - 0.32, y2 - 0.1), 0.64, (y1 - y2) + 0.2, boxstyle="round,pad=0.0,rounding_size=0.3", fc=fc, ec="#333", lw=1.6, zorder=5))
        ax.text(x, (y1 + y2) / 2, cp["ref"], ha="center", va="center", fontsize=13, fontweight="bold", zorder=7)
        if cp["ref"] in POLAR:
            for (net, pin), y in zip(cp["pins"], (y1, y2)):
                if pin == 1:
                    ax.text(x + 0.40, y + 0.02, "+", ha="left", va="center", fontsize=16, fontweight="bold", color="#b00", zorder=8)
    for w in wires:
        x, y = X(w["c"]), Y(w["r"])
        ax.add_patch(Circle((x, y), 0.3, fc=WIRE_COL[w["name"]], ec="white", lw=1.5, zorder=6))
        ax.text(x, y, CODE[w["name"]], ha="center", va="center", fontsize=11, fontweight="bold", color="white", zorder=7)
    for j in jumps:
        (r1, c1), (r2, c2) = j["a"], j["b"]
        ax.plot([X(c1), X(c2)], [Y(r1), Y(r2)], color="#e6007e", lw=4, solid_capstyle="round", zorder=4)
        for (r, c) in (j["a"], j["b"]):
            ax.add_patch(Circle((X(c), Y(r)), 0.16, fc="#e6007e", ec="white", zorder=6))
    X2, Y2 = board_axes(ax2, R, N, leads, cuts, True, "COPPER SIDE (flip the board left-right like a page)")
    for j in jumps:
        for (r, c) in (j["a"], j["b"]):
            ax2.add_patch(Circle((X2(c), Y2(r)), 0.16, fc="#e6007e", ec="white", zorder=6))
    ax2.text(N / 2 - 0.5, -R + 0.05, "red X = cut the strip here (3 mm drill bit, one turn)", ha="center", va="center", fontsize=13, color="#c00")
    fig.text(0.5, 0.012, "Parts stand upright (one lead straight down, the other bent over the body).  Pink = jumper wire (insulated, run on the copper side if long).  Discs = wires:  "
             "G0 GPIO0   G3 GPIO3   G4 GPIO4   3V 3V3   GN board GND   C+ / C\u2212 test clips", ha="center", fontsize=11, color="#333")
    fig.savefig(out + ".png", bbox_inches="tight", facecolor="white")
    plt.close(fig)


def steps(model):
    R, N, leads, comps, wires, jumps, cuts = model
    out = [f"Board: {R} strips x {N} holes ({N*2.54:.1f} x {R*2.54:.1f} mm of hole grid). Strips run along the long side; rows A..{string.ascii_uppercase[R-1]} top to bottom, holes 1..{N} left to right."]
    out.append("1. Cut the strips (drill a 3 mm bit one turn, or scrape with a knife) at: " + ", ".join(sorted((label(r, c) for r, c in cuts), key=lambda s: (s[0], int(s[1:])))) + ".")
    out.append("2. Jumpers (insulated wire; run the long ones on the copper side): " + (", ".join(f"{label(*j['a'])} to {label(*j['b'])}" for j in jumps) or "none") + ".")
    out.append("3. Parts stand upright, one lead straight down, the other bent over the body into the second hole:")
    for cp in comps:
        (n_top, p_top), (n_bot, p_bot) = cp["pins"]
        v = cp["val"]
        vs = (f"{v/1000:g} k" if v >= 1000 else f"{v:g} ") + "ohm" if cp["kind"] == "R" else f"{v*1e6:g} uF"
        extra = ""
        if cp["ref"] in POLAR:
            extra = f"  (+ lead in {label(cp['top'] if p_top == 1 else cp['top'] + 1, cp['col'])})"
        out.append(f"   {cp['ref']} {vs}: {label(cp['top'], cp['col'])} and {label(cp['top'] + 1, cp['col'])}{extra}")
    out.append("4. Wires: " + ", ".join(f"{w['name']} -> {label(w['r'], w['c'])}" for w in wires) + ".")
    return "\n".join(out)


if __name__ == "__main__":
    ok, got, want, model = verify(sys.argv[1])
    print("VERIFY:", "OK - nets read back from the strips match netlist.py" if ok else "FAILED")
    if not ok:
        for s in sorted(got - want, key=sorted): print("  only on board :", sorted(s))
        for s in sorted(want - got, key=sorted): print("  only in netlist:", sorted(s))
        sys.exit(1)
    if len(sys.argv) > 2:
        draw(model, sys.argv[2])
        print(steps(model))
