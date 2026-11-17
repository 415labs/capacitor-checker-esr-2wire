"""Draw the front-end schematic AND prove it matches netlist.py.

The drawing is built from explicit coordinates.  The checker then reads the connectivity straight from the drawn
geometry (wire endpoints, part pins, ground / 3V3 symbols, junction dots) and compares it with netlist.py, so a wire that
looks connected but isn't (or the other way round) is caught.  Decorations (board outline, captions) are not wires.

usage: python3 draw_schematic.py [out_basename]
"""
import sys, itertools
import schemdraw
import schemdraw.elements as elm
from netlist import COMPONENTS, BOARD_PINS, POLAR

schemdraw.config(fontsize=13, lw=1.6)
d = schemdraw.Drawing()
d.config(unit=3)

WIRES, PARTS, GNDS, VDDS, DOTS, PINS, LABELS = [], [], [], [], [], {}, []
_net_elems = set()


def key(p):
    return (round(float(p[0]), 3), round(float(p[1]), 3))


def wire(a, b, dashed=False):
    e = elm.Line().at(a).to(b)
    if dashed:
        e.linestyle('--')
    d.add(e)
    WIRES.append((key(a), key(b)))


def part(ref, cls, a, b, label, loc='top', **kw):
    e = cls(**kw).at(a).to(b)
    d.add(e)
    LABELS.append(label)
    PARTS.append((ref, key(a), key(b)))
    return e


def dot(p, open_=False):
    d.add(elm.Dot(open=open_).at(p))
    if not open_:
        DOTS.append(key(p))


def gnd(p):
    d.add(elm.Ground().at(p))
    GNDS.append(key(p))


def vdd(p, label='3V3'):
    d.add(elm.Vdd().at(p).label(label, loc='top', ofst=0.1))
    VDDS.append(key(p))


def pin(name, p, label):
    d.add(elm.Dot(open=True).at(p))
    PINS[name] = key(p)
    d.add(elm.Label().at((p[0] - 0.25, p[1])).label(label, halign='right', valign='center', fontsize=13))


def note(xy, text, size=12, halign='left', valign='center', color='#444'):
    d.add(elm.Label().at(xy).label(text, halign=halign, valign=valign, fontsize=size, color=color))


def decor_line(a, b, **kw):
    d.add(elm.Line().at(a).to(b).color('#7a5cc7').linewidth(1.4))


# ------------------------------------------------------------------ board outline (decoration only)
BX0, BX1, BY0, BY1 = 0.0, 3.0, -4.2, 16.0
for a, b in [((BX0, BY0), (BX1, BY0)), ((BX1, BY0), (BX1, BY1)), ((BX1, BY1), (BX0, BY1)), ((BX0, BY1), (BX0, BY0))]:
    decor_line(a, b)
note((1.5, 8.5), 'ESP32-C3\n0.42" OLED\nboard', size=13, halign='center', color='#5b3fa8')

# ------------------------------------------------------------------ pins
pin('GPIO0', (3, 14), 'GPIO0\n(ADC in)')
pin('GPIO3', (3, 3.5), 'GPIO3\n(drive)')
pin('GPIO4', (3, 1), 'GPIO4\n(ADC ref)')

# ------------------------------------------------------------------ ADC input chain  (y = 14)
wire((3, 14), (6, 14)); dot((6, 14))
part('R5', elm.Resistor, (6, 14), (9, 14), 'R5\n2.2 kΩ')
part('C1', elm.Capacitor, (9, 14), (12, 14), 'C1  10 µF', polar=True)
wire((12, 14), (16, 14))
wire((16, 14), (16, 3.5))

# ------------------------------------------------------------------ bias network (VB bus y = 9)
part('R6', elm.Resistor, (6, 14), (6, 9), 'R6\n10 kΩ', loc='left')
wire((6, 9), (8, 9)); dot((8, 9)); wire((8, 9), (10, 9)); dot((10, 9)); wire((10, 9), (12, 9))
part('R7', elm.Resistor, (8, 9), (8, 11.5), 'R7\n47 kΩ', loc='left')
vdd((8, 11.5))
part('R8', elm.Resistor, (10, 9), (10, 6), 'R8\n10 kΩ', loc='right')
gnd((10, 6))
part('C2', elm.Capacitor, (12, 9), (12, 6), 'C2  10 µF\n(+ up)', loc='right', polar=True)
gnd((12, 6))

# ------------------------------------------------------------------ drive + clips  (y = 3.5)
wire((3, 3.5), (5, 3.5)); dot((5, 3.5))
part('R1', elm.Resistor, (5, 3.5), (9, 3.5), 'R1  330 Ω')
wire((9, 3.5), (16, 3.5)); dot((16, 3.5))
part('R2', elm.Resistor, (16, 3.5), (16, 0.5), 'R2')
gnd((16, 0.5))
wire((16, 3.5), (19, 3.5))
d.add(elm.Dot(open=True).at((19, 3.5)))
wire((19, 3.5), (22, 3.5), dashed=True)
part('DUT', elm.Capacitor, (22, 3.5), (22, 0.5), 'DUT', polar=True)
wire((22, 0.5), (19, 0.5), dashed=True)
d.add(elm.Dot(open=True).at((19, 0.5)))
gnd((19, 0.5))

# ------------------------------------------------------------------ reference tap
wire((3, 1), (5, 1)); dot((5, 1))
part('R3', elm.Resistor, (5, 3.5), (5, 1), 'R3\n5.6 kΩ', loc='right')
part('R4', elm.Resistor, (5, 1), (5, -1.5), 'R4\n1 kΩ', loc='right')
gnd((5, -1.5))

# ------------------------------------------------------------------ labels (explicit positions)
BLUE, BROWN, GREEN = '#1f5fa8', '#a85a1f', '#1b6f4a'
note((7.5, 15.0), 'R5  2.2 kΩ', halign='center', size=13, color='black')
note((10.5, 15.0), 'C1  10 µF', halign='center', size=13, color='black')
note((10.5, 13.0), 'electrolytic, + toward R5', halign='center', size=11)
note((5.35, 11.5), 'R6\n10 kΩ', halign='right', size=13, color='black')
note((8.35, 10.3), 'R7  47 kΩ', halign='left', size=13, color='black')
note((10.35, 7.5), 'R8\n10 kΩ', halign='left', size=13, color='black')
note((12.35, 7.5), 'C2  10 µF\n+ on VB', halign='left', size=13, color='black')
note((8.0, 8.2), 'VB = 0.58 V', halign='center', size=12, color=GREEN)
note((7.0, 4.4), 'R1  330 Ω', halign='center', size=13, color='black')
note((5.4, 2.3), 'R3  5.6 kΩ', halign='left', size=13, color='black')
note((5.4, -0.2), 'R4  1 kΩ', halign='left', size=13, color='black')
note((16.4, 2.0), 'R2\n10 Ω 1 %', halign='left', size=13, color='black')
note((15.7, 4.35), 'node A', halign='right', size=12, color=BROWN)
note((19.0, 4.4), 'clip +', halign='center', size=12)
note((19.0, -0.9), 'clip −', halign='center', size=12)
note((22.5, 2.0), 'capacitor\nunder test', halign='left', size=13, color='black')
note((5.4, 1.0), 'REF = drive ÷ 6.6', halign='left', size=11, color=BLUE)
note((3.5, -2.6), 'Every ground symbol goes to the board GND pin, every 3V3 symbol to the 3V3 pin.', halign='left', size=11)
note((3.5, -3.6), 'No diodes. Test signal: 0-3.3 V square wave on GPIO3, 10 Hz to 8 kHz.', halign='left', size=11)

# ================================================================== connectivity extraction from the geometry
parent = {}


def find(x):
    parent.setdefault(x, x)
    while parent[x] != x:
        parent[x] = parent[parent[x]]
        x = parent[x]
    return x


def union(a, b):
    parent[find(a)] = find(b)


for a, b in WIRES:
    union(a, b)
for g in GNDS:
    union(g, ('GND',))
for v in VDDS:
    union(v, ('3V3',))
for name, p in PINS.items():
    union(p, ('PIN', name))

nets = {}
for ref, a, b in PARTS:
    base = ref
    tok_a = base if ref not in POLAR and ref != 'DUT' else f'{ref}.1'
    tok_b = base if ref not in POLAR and ref != 'DUT' else f'{ref}.2'
    nets.setdefault(find(a), set()).add(tok_a)
    nets.setdefault(find(b), set()).add(tok_b)
for name, p in PINS.items():
    nets.setdefault(find(p), set()).add(name)
for g in GNDS:
    nets.setdefault(find(g), set()).add('GND')
for v in VDDS:
    nets.setdefault(find(v), set()).add('3V3')
drawn = {frozenset(s) for s in nets.values()}

# expected nets from netlist.py (DUT: + on A, - on GND)
exp = {}
for ref, kind, val, n1, n2 in COMPONENTS:
    t1 = f'{ref}.1' if ref in POLAR else ref
    t2 = f'{ref}.2' if ref in POLAR else ref
    exp.setdefault(n1, set()).add(t1)
    exp.setdefault(n2, set()).add(t2)
exp['A'].add('DUT.1')
exp['GND'].update({'DUT.2', 'GND'})
exp['3V3'].add('3V3')
for p, n in BOARD_PINS.items():
    exp[n].add(p)
expected = {frozenset(s) for s in exp.values()}

# ERC: junction dots, floating pins
problems = []
if drawn != expected:
    problems.append('drawing != netlist.py')
    for s in sorted(drawn - expected, key=sorted):
        problems.append('  only in drawing : ' + ', '.join(sorted(s)))
    for s in sorted(expected - drawn, key=sorted):
        problems.append('  only in netlist : ' + ', '.join(sorted(s)))
endpoints = {}
for a, b in WIRES:
    for p in (a, b):
        endpoints[p] = endpoints.get(p, 0) + 1
for ref, a, b in PARTS:
    for p in (a, b):
        endpoints[p] = endpoints.get(p, 0) + 1
for p, n in endpoints.items():
    if n >= 3 and p not in DOTS:
        problems.append(f'3-way junction at {p} has no dot')
for p in DOTS:
    if endpoints.get(p, 0) < 3:
        problems.append(f'dot at {p} joins fewer than 3 things')


def seg_cross(s1, s2):                       # axis-aligned segments crossing in their interiors
    (a, b), (c, e) = s1, s2
    def horiz(s): return abs(s[0][1] - s[1][1]) < 1e-6
    if horiz(s1) == horiz(s2):
        return False
    h, v = (s1, s2) if horiz(s1) else (s2, s1)
    y, x = h[0][1], v[0][0]
    return min(h[0][0], h[1][0]) + 1e-6 < x < max(h[0][0], h[1][0]) - 1e-6 and min(v[0][1], v[1][1]) + 1e-6 < y < max(v[0][1], v[1][1]) - 1e-6


allsegs = [(a, b) for a, b in WIRES] + [(a, b) for _, a, b in PARTS]
for s1, s2 in itertools.combinations(allsegs, 2):
    if seg_cross(s1, s2):
        problems.append(f'wires cross without a junction: {s1} x {s2}')
for s in drawn:
    parts_in = [t for t in s if t not in ('GND', '3V3') and not t.startswith('GPIO')]
    if len(s) < 2:
        problems.append('floating terminal: ' + ', '.join(s))

if __name__ == '__main__':
    base = sys.argv[1] if len(sys.argv) > 1 else 'two_wire_front_end'
    print('nets read back from the drawing:')
    for s in sorted(drawn, key=lambda s: (-len(s), sorted(s))):
        print('   ', ' - '.join(sorted(s)))
    print('CHECK:', 'OK, drawing == netlist.py, no unmarked junctions/crossings' if not problems else 'FAILED')
    for p in problems:
        print('  ', p)
    d.save(base + '.png', dpi=170)
    d.save(base + '.svg')
    sys.exit(1 if problems else 0)
