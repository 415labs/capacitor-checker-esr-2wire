"""Signal-chain simulation, 2-WIRE variant: ADC ch0 = node A (clips connect straight to it), ch1 = drive reference.
Z_meas = RCAL*H/(G0-H) includes the lead impedance; firmware subtracts a stored 'clips shorted' measurement Z0:  Z = Z_meas - Z0."""
import numpy as np
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import chain_ac
USE_NETLIST = True   # exact nodal model of the schematic (chain_ac.py); False = old simplified chain

R1, RO, R2, RIN = 330.0, 40.0, 10.05, 12000.0
RS = R1 + RO
R2E = 1 / (1 / R2 + 1 / RIN)
RCAL_FW = (R1 + 40.0) * (10.0 * RIN / (10.0 + RIN)) / ((R1 + 40.0) + (10.0 * RIN / (10.0 + RIN)))
VDD, BIAS, KREF = 3.3, 0.579, 1.0 / 6.6
TAU_HP = 11e3 * 10e-6
ADC_FS = 0.9
ADC_SIGMA_MV = 3.0
FREQS = [10, 30, 100, 300, 1000, 3000, 8000, 12000, 15000]
# ADC frame rate per test frequency [frames/s]; chosen by alias_search.py so no odd harmonic of the square drive aliases near f
FR = {10: 330.0, 30: 800.0, 100: 1880.0, 300: 5060.0, 1000: 13380.0, 3000: 27828.0, 8000: 25763.0, 12000: 38645.0, 15000: 35732.0}
L_LEADS = 0.25e-6
R_LEADS_CAL = 0.12


def zcap(w, C, esr, esl=15e-9, rp=None):
    z = esr + 1j * w * esl + 1 / (1j * w * C)
    if rp:
        z = 1 / (1 / z + 1 / rp)
    return z


def with_leads(zdut, r_leads):
    """DUT in series with the two test leads (resistance + inductance)"""
    return lambda w: zdut(w) + r_leads + 1j * w * L_LEADS


def frame_rate(f):
    return FR[f]


def nframes_for(f):
    fr = frame_rate(f)
    return int(min(4096, max(6 * fr / f, 0.3 * fr)))


def acquire(f, zfun, rng, nframes, sigma_mv=ADC_SIGMA_MV, fs_err=1.0037):
    fframe = frame_rate(f) * fs_err
    k = np.arange(nframes)
    out = np.zeros((nframes, 2))
    for c in range(2):
        t = (k + c / 2.0) / fframe
        v = np.full(nframes, VDD * KREF * 0.5 if c == 1 else BIAS)
        for n in range(1, 60, 2):
            w = 2 * np.pi * f * n
            amp = VDD * 2 / (n * np.pi)
            if USE_NETLIST:
                gp, gr = chain_ac.transfer(w, zfun(w))
                ph = amp * (gr if c == 1 else gp)
            elif c == 1:
                ph = amp * KREF
            else:
                z = zfun(w)
                zl = 1 / (1 / z + 1 / R2E) if np.isfinite(z) else R2E
                hp = 1j * w * TAU_HP / (1 + 1j * w * TAU_HP)
                ph = hp * amp * zl / (zl + RS)
            v += np.imag(ph * np.exp(1j * w * t))
        v = v + rng.normal(0, sigma_mv * 1e-3, nframes)
        out[:, c] = np.clip(np.floor(v / ADC_FS * 4096), 0, 4095)
    return out


def hann(n):
    return 0.5 - 0.5 * np.cos(2 * np.pi * (np.arange(n) + 0.5) / n)


def _mag(x, o, n):
    return abs(np.sum(hann(n) * x[:n] * np.exp(-1j * o * (np.arange(n) + 0.5))))


def find_omega(frames, omega0, span=0.04):
    N = len(frames)
    x = frames[:, 1] - frames[:, 1].mean()
    stages = [m for m in (192, 768, 3072) if m < N] + ([N] if N <= 3072 else [])
    o = omega0
    for si, n in enumerate(stages):
        d = np.pi / n
        if si == 0:
            m = int(np.ceil(span * omega0 / d))
            cand = o + d * np.arange(-m, m + 1)
            o = cand[np.argmax([_mag(x, c, n) for c in cand])]
        for _ in range(2):
            m0, m1, m2 = _mag(x, o - d, n), _mag(x, o, n), _mag(x, o + d, n)
            den = m0 - 2 * m1 + m2
            if den != 0:
                o += 0.5 * d * (m0 - m2) / den
            d *= 0.5
    return o


def measure(f, zfun, g0, rng, **kw):
    """returns Z_meas (leads included) or, when g0 is None, the raw ratio H"""
    n = nframes_for(f)
    fr = acquire(f, zfun, rng, n, **kw)
    om = find_omega(fr, 2 * np.pi * f / frame_rate(f))
    w = hann(n)
    k = np.arange(n)
    x = [np.sum(w * (fr[:, c] - fr[:, c].mean()) * np.exp(-1j * om * (k + c / 2.0))) for c in range(2)]
    h = x[0] / x[1]
    return RCAL_FW * h / (g0 - h) if g0 is not None else h


def calibrate(rng, r_cal=R_LEADS_CAL):
    g0 = {f: measure(f, lambda w: np.inf, None, rng) for f in FREQS}
    z0 = {f: measure(f, with_leads(lambda w: 0j, r_cal), g0[f], rng) for f in FREQS}
    return g0, z0


def fit(zs, fs, rcal=RCAL_FW):
    w = 2 * np.pi * np.array(fs)
    z = np.array(zs)
    ok = (np.abs(z.imag) > 0.02) & (np.abs(z.imag) < 100)
    c0 = np.median(1 / (w[ok] * np.abs(z.imag[ok]))) if ok.any() else 1 / (w[0] * max(abs(z[0]), 1e-3))
    esr0 = 0.0
    for _ in range(3):
        zexp = np.hypot(esr0, 1 / (w * c0))
        wt = 1 / (rcal + zexp ** 2 / rcal) ** 2
        esr = np.sum(wt * z.real) / np.sum(wt)
        x = 1 / w
        d = -np.sum(wt * z.imag * x) / np.sum(wt * x * x)
        if d > 0:
            c0 = 1 / d
        esr0 = max(esr, 0.0)
    return esr, (1 / d if d > 0 else np.inf)


def pick(z1k):
    out = []
    xc = abs(z1k.imag) * 1000.0
    esr = max(z1k.real, 0.0)
    for f in FREQS:
        if 0.015 <= np.hypot(esr, xc / f) <= 150.0:
            out.append(f)
    return out or [1000]


def one_test(zdut, g0, z0, rng, jitter, **kw):
    r = R_LEADS_CAL + rng.normal(0, jitter)          # this time's clip contact resistance
    zf = with_leads(zdut, r)
    z1k = measure(1000, zf, g0[1000], rng, **kw) - z0[1000]
    fs = pick(z1k)
    zs = [z1k if f == 1000 else measure(f, zf, g0[f], rng, **kw) - z0[f] for f in fs]
    return fit(zs, fs), fs


if __name__ == "__main__":
    for jitter in (0.0, 0.03, 0.08):
        print(f"\n--- clip contact variation sigma = {jitter*1000:.0f} mohm (sigma ADC noise {ADC_SIGMA_MV} mV) ---")
        print(f"{'C(uF)':>7} {'ESR true':>8} | {'ESR meas':>9} {'+-':>7} | {'C meas':>8} {'+-%':>5}")
        for C, esr in [(2.2e-6, 3.0), (10e-6, 1.0), (100e-6, 1.0), (100e-6, 0.3), (100e-6, 0.1),
                       (1000e-6, 0.3), (1000e-6, 0.1), (1000e-6, 0.03)]:
            rng = np.random.default_rng(3)
            g0, z0 = calibrate(rng)
            res = [one_test(lambda w: zcap(w, C, esr), g0, z0, rng, jitter)[0] for _ in range(12)]
            e = np.array([r[0] for r in res]); c = np.array([r[1] for r in res])
            print(f"{C*1e6:7.1f} {esr:8.3f} | {e.mean():9.4f} {e.std():7.4f} | {c.mean()*1e6:8.1f} {100*c.std()/c.mean():5.2f}")
