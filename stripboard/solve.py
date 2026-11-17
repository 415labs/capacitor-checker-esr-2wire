"""Search for a compact stripboard layout of schematic/netlist.py.

Model: strips run along the long side (rows of the grid = strips, columns = hole positions).  Every 2-terminal part STANDS UPRIGHT in
two vertically adjacent holes of one column (one lead straight down, the other bent over the body).  Board wires (GPIO0, GPIO3, GPIO4,
3V3, GND, clip+, clip-) each take one hole.  Up to J bare-wire jumpers.  Strips are cut (a drilled hole) between neighbouring holes of
different nets; a cut needs an empty hole between them.  Simulated annealing over placements; cost = rule violations (+ small extras).
usage: python3 solve.py ROWS COLS [seconds] [seed]   ->  prints JSON layout on success
"""
import sys, os, json, random, math, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "schematic"))
from netlist import COMPONENTS

WIRES = [("GPIO0", "ADC"), ("GPIO3", "DRV"), ("GPIO4", "REF"), ("3V3", "3V3"), ("GND-board", "GND"), ("clip+", "A"), ("clip-", "GND")]
NETS = sorted({c[3] for c in COMPONENTS} | {c[4] for c in COMPONENTS})
NJ = int(os.environ.get("NJ", "2"))


class State:
    def __init__(s, R, N, rnd):
        s.R, s.N, s.rnd = R, N, rnd
        s.comp = [[rnd.randrange(N), rnd.randrange(R - 1), rnd.randrange(2)] for _ in COMPONENTS]       # col, toprow, flip
        s.wire = [[rnd.randrange(R), rnd.randrange(N)] for _ in WIRES]                                    # row, col
        s.jump = [[rnd.randrange(R), rnd.randrange(N), rnd.randrange(R), rnd.randrange(N), rnd.choice(NETS), 0] for _ in range(NJ)]

    def copy(s):
        t = State.__new__(State)
        t.R, t.N, t.rnd = s.R, s.N, s.rnd
        t.comp = [list(x) for x in s.comp]; t.wire = [list(x) for x in s.wire]; t.jump = [list(x) for x in s.jump]
        return t

    def leads(s):
        L = []   # (row, col, net, tag)
        for (ref, kind, val, n1, n2), (c, t, f) in zip(COMPONENTS, s.comp):
            top, bot = (n1, n2) if not f else (n2, n1)
            L.append((t, c, top, ref + ("1" if not f else "2"))); L.append((t + 1, c, bot, ref + ("2" if not f else "1")))
        for (name, net), (r, c) in zip(WIRES, s.wire):
            L.append((r, c, net, name))
        for k, (r1, c1, r2, c2, net, used) in enumerate(s.jump):
            if used:
                L.append((r1, c1, net, f"J{k}a")); L.append((r2, c2, net, f"J{k}b"))
        return L

    def cost(s, detail=False):
        L = s.leads()
        occ = {}
        v = 0
        for i, (r, c, net, tag) in enumerate(L):
            if (r, c) in occ:
                v += 1
            occ[(r, c)] = i
        par = list(range(len(L)))
        def find(x):
            while par[x] != x:
                par[x] = par[par[x]]; x = par[x]
            return x
        cuts = 0
        for r in range(s.R):
            row = sorted((c, i) for i, (rr, c, n, t) in enumerate(L) if rr == r)
            for (c1, i1), (c2, i2) in zip(row, row[1:]):
                if L[i1][2] == L[i2][2]:
                    par[find(i1)] = find(i2)
                else:
                    if c2 - c1 == 1:
                        v += 1
                    else:
                        cuts += 1
        if os.environ.get("EDGES"):            # MCU wires all in the top strip, probe wires in the last column
            for (name, net), (r, c) in zip(WIRES, s.wire):
                if name in ("clip+", "clip-"):
                    v += (c != s.N - 1)
                else:
                    v += (c > 1) if os.environ.get('EDGES')=='ends' else (r != 0)
        for k, (r1, c1, r2, c2, net, used) in enumerate(s.jump):
            if used:
                # physical rule: short hop (adjacent strips, up to 2 holes along) with no lead in the holes it passes over
                if (r1, c1) == (r2, c2) or (not os.environ.get('LONGJ') and (abs(r1 - r2) > 1 or abs(c1 - c2) > 2)):
                    v += 1
                else:
                    for cc in ([] if os.environ.get('LONGJ') else range(min(c1, c2) + 1, max(c1, c2))):
                        if (r1, cc) in occ or (r2, cc) in occ:
                            v += 1
                a = [i for i, l in enumerate(L) if l[3] == f"J{k}a"][0]; b = [i for i, l in enumerate(L) if l[3] == f"J{k}b"][0]
                par[find(a)] = find(b)
        for net in NETS:
            groups = {find(i) for i, l in enumerate(L) if l[2] == net}
            v += len(groups) - 1
        extra = sum((6 if os.environ.get('LONGJ') else 25) for j in s.jump if j[5]) + cuts
        return (v * 1000 + extra) if not detail else (v, extra, cuts)


def anneal(R, N, seconds, seed):
    rnd = random.Random(seed)
    s = State(R, N, rnd)
    W = 60
    cost = lambda st: (lambda d: d[0] * W + d[1])(st.cost(detail=True))
    cur = cost(s); best, bc = s.copy(), cur
    t0 = time.time(); it = 0
    while time.time() - t0 < seconds:
        it += 1
        frac = (time.time() - t0) / seconds
        T = max(0.3, 90 * (1 - frac) ** 2)
        k = rnd.random()
        if k < 0.55:
            i = rnd.randrange(len(s.comp)); old = list(s.comp[i])
            if rnd.random() < .5:
                s.comp[i] = [rnd.randrange(N), rnd.randrange(R - 1), rnd.randrange(2)]
            else:
                s.comp[i] = [min(N - 1, max(0, old[0] + rnd.choice((-1, 0, 1)))), min(R - 2, max(0, old[1] + rnd.choice((-1, 0, 1)))), old[2] ^ (rnd.random() < .3)]
            undo = lambda: s.comp.__setitem__(i, old)
        elif k < 0.85:
            i = rnd.randrange(len(s.wire)); old = list(s.wire[i])
            if rnd.random() < .4:
                s.wire[i] = [rnd.randrange(R), rnd.randrange(N)]
            else:
                s.wire[i] = [min(R - 1, max(0, old[0] + rnd.choice((-1, 0, 1)))), min(N - 1, max(0, old[1] + rnd.choice((-1, 0, 1))))]
            undo = lambda: s.wire.__setitem__(i, old)
        else:
            i = rnd.randrange(NJ); old = list(s.jump[i]); j = s.jump[i]
            x = rnd.random()
            if x < .25:
                j[5] ^= 1
            elif x < .4:
                j[4] = rnd.choice(NETS)
            else:
                j[0], j[1], j[2], j[3] = rnd.randrange(R), rnd.randrange(N), rnd.randrange(R), rnd.randrange(N)
            undo = lambda: s.jump.__setitem__(i, old)
        c2 = cost(s)
        if c2 <= cur or rnd.random() < math.exp((cur - c2) / T):
            cur = c2
            if cur < bc:
                best, bc = s.copy(), cur
        else:
            undo()
    return best, bc, it


if __name__ == "__main__":
    R, N = int(sys.argv[1]), int(sys.argv[2])
    secs = float(sys.argv[3]) if len(sys.argv) > 3 else 20
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    b, c, it = anneal(R, N, secs, seed); sys.stderr.write(f'iterations {it}\n')
    v, extra, cuts = b.cost(detail=True)
    print(json.dumps({"R": R, "N": N, "violations": v, "extra": extra, "cuts": cuts, "comp": b.comp, "wire": b.wire, "jump": b.jump}))
