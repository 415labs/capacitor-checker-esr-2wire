"""Pick ADC frame rates so odd harmonics of the square drive never alias next to the fundamental.
Harmonic n of drive f, sampled at frame rate fr, appears at |n f - k fr| (folded). Must stay > margin from f for all odd n<=NMAX
and for +-1.5% ADC clock error. Margin is in lock-in bins (resolution ~ 2/T, T = N/fr)."""
import numpy as np
NMAX = 61
def alias_dist(f, fr):
    worst = 1e9
    for n in range(3, NMAX + 1, 2):
        a = (n * f) % fr
        a = min(a, fr - a)
        # amplitude weight 1/n: scale distance requirement by n (weaker harmonics may sit closer)
        worst = min(worst, abs(a - f) / f * (n / 3.0))
    return worst
def plan_n(f, fr):
    return int(min(4096, max(6 * fr / f, 0.3 * fr)))
def score(f, fr):
    n = plan_n(f, fr)
    T = n / fr
    res = 2.0 / T                     # Hann main-lobe half width [Hz]
    s = 1e9
    for e in (-0.015, 0, 0.015):
        fr2 = fr * (1 + e)
        for nn in range(3, NMAX + 1, 2):
            a = (nn * f) % fr2
            a = min(a, fr2 - a)
            d = abs(a - f)
            s = min(s, d / res * (nn / 3.0))
    return s
if __name__ == "__main__":
    for f in (10, 30, 100, 300, 1000, 3000, 8000, 12000, 15000):
        best = None
        for fr in np.arange(max(320, 2.3 * f), 40000.0, 1.0):
            s = score(f, fr)
            if best is None or s > best[0]: best = (s, fr)
        old = min(40000, max(320, 17.37 * f))
        print(f"f={f:6d}  old fr={old:8.0f} score={score(f, old):7.1f}   best fr={best[1]:8.0f} score={best[0]:7.1f}  ratio={best[1]/f:.3f}")
