"""Exact small-signal (complex nodal) model of the analog front end, built from schematic/netlist.py.
3V3 and GND are AC ground; the GPIO3 drive is an ideal source behind its output resistance RO; ADC inputs = C_ADC to ground."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "schematic"))
import netlist

COMPONENTS = netlist.COMPONENTS
RO = 40.0           # ESP32-C3 GPIO output resistance (approx.)
C_ADC = 5e-12       # ADC pin + sample/hold capacitance
NODES = sorted({n for c in COMPONENTS for n in c[3:5]} - {"GND", "3V3"})   # DRV = the GPIO3 pad
IDX = {n: i for i, n in enumerate(NODES)}


def _stamp(Y, a, b, y):
    ia, ib = IDX.get(a), IDX.get(b)
    if ia is not None:
        Y[ia, ia] += y
    if ib is not None:
        Y[ib, ib] += y
    if ia is not None and ib is not None:
        Y[ia, ib] -= y
        Y[ib, ia] -= y


def solve(w, zdut, comps=None, ro=RO):
    """Node voltages (dict) for a 1 V Thevenin drive at angular frequency w; zdut between the clips (A, GND)."""
    comps = comps or COMPONENTS
    Y = np.zeros((len(NODES), len(NODES)), complex)
    s = 1j * w
    for ref, kind, val, n1, n2 in comps:
        _stamp(Y, n1, n2, 1 / val if kind == "R" else s * val)
    _stamp(Y, "DRV", "GND", 1 / ro)
    if np.isfinite(zdut):
        _stamp(Y, netlist.DUT[0], netlist.DUT[1], 1e9 if zdut == 0 else 1 / zdut)
    _stamp(Y, "ADC", "GND", s * C_ADC)
    _stamp(Y, "REF", "GND", s * C_ADC)
    I = np.zeros(len(NODES), complex)
    I[IDX["DRV"]] = 1 / ro
    V = np.linalg.solve(Y, I)
    return {k: V[i] for k, i in IDX.items()}


def transfer(w, zdut):
    """(pad, ref) phasor gains per volt of Thevenin drive."""
    v = solve(w, zdut)
    return v["ADC"], v["REF"]


def dc_bias(vdd=3.3):
    R = {c[0]: c[2] for c in COMPONENTS}
    return vdd * R["R8"] / (R["R7"] + R["R8"])
