"""Write harmonic transfer tables of simulated test cases for the host test (test_dsp.cpp synthesises unlimited noisy ADC
captures from them). Per case: NVAR variants (different clip contact resistance), per test frequency: complex gains
(node A, reference) of the drive's odd harmonics 1..59, from the exact nodal model of the schematic (sim/chain_ac.py)."""
import os, struct, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "sim"))
import sim2 as sim
import chain_ac

NVAR, NH = 8, 30
JIT = 0.03          # clip contact resistance variation between calibration and use [ohm]
RLEAD = sim.R_LEADS_CAL
zc = sim.zcap
rng = np.random.default_rng(7)
jit = [rng.normal(0, JIT) for _ in range(NVAR)]
INF = lambda w: np.inf
# (name, nominal uF for the verdict (0 = AUTO), true C uF, true ESR ohm, Z(w) of the part alone, jitter?)
CASES = [
    ("CAL_OPEN", 0, 0, 0, INF, False),
    ("CAL_SHORT", 0, 0, 0, lambda w: 0j, False),
    ("CAL_RES4.7", 0, 0, 4.7, lambda w: 4.7 + 0j, False),
    ("CAL_CAP1.0", 0, 1.0, 0.05, lambda w: zc(w, 1.0e-6, 0.05), False),
    ("CAL_CAP100", 0, 100.0, 0.30, lambda w: zc(w, 100e-6, 0.30), False),
    ("0.47u_esr0.05_film", 0.47, 0.47, 0.05, lambda w: zc(w, 0.47e-6, 0.05), True),
    ("0.47u_esr3", 0.47, 0.47, 3.0, lambda w: zc(w, 0.47e-6, 3.0), True),
    ("1u_esr1", 1.0, 1.0, 1.0, lambda w: zc(w, 1e-6, 1.0), True),
    ("2.2u_esr3", 2.2, 2.2, 3.0, lambda w: zc(w, 2.2e-6, 3.0), True),
    ("10u_esr1", 10, 10, 1.0, lambda w: zc(w, 10e-6, 1.0), True),
    ("47u_esr15_dried", 47, 47, 15.0, lambda w: zc(w, 47e-6, 15.0), True),
    ("100u_esr0.3", 100, 100, 0.3, lambda w: zc(w, 100e-6, 0.3), True),
    ("100u_esr0.1", 100, 100, 0.1, lambda w: zc(w, 100e-6, 0.1), True),
    ("100u_esr0.9_aged", 100, 100, 0.9, lambda w: zc(w, 100e-6, 0.9), True),
    ("100u_esr2.56_borderline", 100, 100, 2.56, lambda w: zc(w, 100e-6, 2.56), True),
    ("80.5u_nom100_borderline", 100, 80.5, 0.3, lambda w: zc(w, 80.5e-6, 0.3), True),
    ("100u_esr3_weak", 100, 100, 3.0, lambda w: zc(w, 100e-6, 3.0), True),
    ("100u_esr0.3_nom220", 220, 100, 0.3, lambda w: zc(w, 100e-6, 0.3), True),
    ("100u_esr0.3_nom100", 100, 100, 0.3, lambda w: zc(w, 100e-6, 0.3), True),
    ("1000u_esr0.1", 1000, 1000, 0.1, lambda w: zc(w, 1000e-6, 0.1), True),
    ("1000u_esr0.03", 1000, 1000, 0.03, lambda w: zc(w, 1000e-6, 0.03), True),
    ("4700u_esr0.05", 4700, 4700, 0.05, lambda w: zc(w, 4700e-6, 0.05), True),
    ("open", 0, 0, 0, INF, False),
    ("short_clips", 0, 0, 0, lambda w: 0j, True),
    ("100u_par100ohm", 0, 100, 0.3, lambda w: zc(w, 100e-6, 0.3, rp=100), True),
    ("100u_in_parallel_with_100u", 0, 200, 0.1, lambda w: 1 / (1 / zc(w, 100e-6, 0.2) + 1 / zc(w, 100e-6, 0.2)), True),
]
with open(os.path.join(HERE, "cases.bin"), "wb") as f:
    f.write(struct.pack("<ii", len(CASES), sim.FREQS.__len__()))
    for name, nom, ct, et, zd, usejit in CASES:
        f.write(name.encode().ljust(32, b"\0"))
        f.write(struct.pack("<dddi", nom, ct, et, NVAR))
        for v in range(NVAR):
            zf = sim.with_leads(zd, RLEAD + (jit[v] if usejit else 0.0)) if np.isfinite(zd(1.0)) or True else zd
            for fr in sim.FREQS:
                for h in range(NH):
                    w = 2 * np.pi * fr * (2 * h + 1)
                    z = zf(w)
                    # open circuit stays open (no leads): with_leads would add series R to inf -> still inf
                    gp, gr = chain_ac.transfer(w, z)
                    f.write(struct.pack("<dddd", gp.real, gp.imag, gr.real, gr.imag))
print("wrote", len(CASES), "cases")
