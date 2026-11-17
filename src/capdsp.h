// capdsp.h - portable (host + ESP32-C3) measurement maths for the in-circuit capacitor checker.
// No ESP-IDF dependencies: unit-tested on the PC against simulated ADC data (see test/).
//
// 2-WIRE variant. Hardware: GPIO square 0/3.3V -> R1(330) -> node A, R2(10) from A to GND, the two clips
// connect straight to node A and GND.  ADC ch0 = node A (AC coupled), ch1 = drive reference (5.6k/1k tap).
// One ADC "frame" = 2 consecutive samples [ch0, ch1].
//
//   H  = X0 / X1                       complex lock-in ratio (node-A voltage / drive reference)
//   Zm = RCAL*rtrim * H / (G0 - H)     G0 = H with the clips OPEN (calibrated, per frequency); Zm includes the test leads
//   Z  = Zm - Z0                       Z0 = Zm with the clips SHORTED together (calibrated, per frequency)
//   rtrim: gain trim from a known resistor.   ctrim: C trim from known capacitor(s).
//
// Every reading carries an uncertainty: statistical (repeat captures + calibrated noise floor, propagated through the fit)
// plus systematic (clip contact, calibration trim). Repeats are added adaptively until the target precision is reached.
#pragma once
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace cap {

using cx = std::complex<double>;

// ---------------------------------------------------------------- configuration
constexpr int NFREQ = 9;
constexpr double FREQS[NFREQ] = {10, 30, 100, 300, 1000, 3000, 8000, 12000, 15000};
// ADC frame rate (frames/s, 2 channels per frame) for each test frequency. Chosen by sim/alias_search.py so that NO odd
// harmonic (3..61) of the square drive aliases within the lock-in bandwidth of the fundamental, also with +-1.5 % ADC clock
// error.  (The old plan of "40 kframes/s for everything fast" put the 9th harmonic of 8 kHz exactly on the fundamental.)
constexpr double FRAME_RATES[NFREQ] = {330, 800, 1880, 5060, 13380, 27828, 25763, 38645, 35732};
constexpr int PROBE_IDX = 4;                 // 1 kHz
constexpr int MAX_FRAMES = 4096;
constexpr double RCAL = 9.73;                // ohm: R2||Rin in parallel with (R1+Ro); refined by the known-resistor calibration

// verdict limits (tunable)
constexpr double OPEN_Z1K = 1500.0;          // |Z| at 1 kHz above this = open / < ~0.1 uF
constexpr double CMAX_UF = 50000.0;          // above this the part is indistinguishable from a short
constexpr double ESRC_MARGINAL = 250.0;      // ESR*C [ohm*uF], used when no reference part is known
constexpr double ESRC_BAD = 600.0;
constexpr double C_LOW_FRAC = 0.80;          // vs nominal
constexpr double C_HIGH_FRAC = 1.35;
constexpr double REF_ESR_WEAK = 1.5;         // vs a learned good part of the same value: ESR >1.5x = WEAK, >2.5x = BAD
constexpr double REF_ESR_BAD = 2.5;
constexpr double REF_C_WEAK = 0.90;          // C < 90 % of the learned good part = WEAK, < 80 % = BAD
constexpr double REF_C_BAD = 0.80;
constexpr double REF_C_HIGH = 1.25;
constexpr double PAR_G_REL = 0.2;            // parallel path must exceed 20% of the capacitive susceptance (else it is noise)
constexpr double ESR_UNC = 0.05;             // ohm: 2-wire clip-contact uncertainty, ignored by the ESR*C rule
constexpr double PAR_G_MS = 5.0;             // excess conductance at low f [mS] -> parallel path (< ~200 ohm)
constexpr double ESR_CONTACT = 0.03;         // ohm: assumed variation of clip contact resistance between calibration and use
constexpr double SYS_REL_UNTRIMMED = 0.03;   // gain uncertainty with no known-resistor calibration (R2 tolerance, drive impedance)
constexpr double SYS_REL_TRIMMED = 0.01;
constexpr double SYS_C_UNTRIMMED = 0.03;     // extra C uncertainty with no known-capacitor calibration
constexpr double SYS_C_TRIMMED = 0.015;
constexpr double H_NOISE_DEFAULT = 5e-4;     // per-capture noise of H if the open calibration has none stored
constexpr int K_OPEN = 12;                   // captures per frequency in the open calibration (its noise is a fixed error in every later test)
constexpr int K_SHORT = 6;                   // ... in the shorted calibration
constexpr double CONNECT_Z = 1000.0;         // |Z| at 1 kHz below this = something is clipped on (a capacitor of >= ~0.16 uF)
constexpr int CONNECT_STABLE = 3;            // consecutive, mutually consistent probes needed before a test starts (about 2 s of firm contact)
constexpr double CONNECT_HOLD_S = 1.0;       // ...and they must span at least this long (contact must be firm, not a touch)
constexpr double CONNECT_JITTER = 0.15;      // max relative change of |Z| between two probes (+ 0.05 ohm absolute)
constexpr double TGT_C_REL = 0.02;           // adaptive repeats stop when the statistical C error is below this ...
constexpr double TGT_ESR_ABS = 0.03;        // ... and the statistical ESR error below this (+2 % of ESR)

enum Level { LV_QUICK = 0, LV_NORMAL = 1, LV_PRECISE = 2 };
struct LevelCfg { int kbase; int kmax_total; };       // captures per frequency at least / total captures at most
constexpr LevelCfg LEVELS[3] = {{1, 0}, {2, 28}, {3, 60}};

struct Plan {
  double f;           // drive frequency [Hz]
  double frame_rate;  // requested frames/s (ADC total rate = 2x)
  int n;              // frames to capture
};

inline Plan plan(int fi) {
  const double f = FREQS[fi], fr = FRAME_RATES[fi];
  double n = 6.0 * fr / f;
  if (n < 0.3 * fr) n = 0.3 * fr;
  if (n > MAX_FRAMES) n = MAX_FRAMES;
  return {f, fr, (int)n};
}

// ---------------------------------------------------------------- lock-in
inline void hann(double* w, int n) {
  const double th = 2.0 * M_PI / n;
  const double c0 = std::cos(th), s0 = std::sin(th);
  double c = std::cos(0.5 * th), s = std::sin(0.5 * th);
  for (int k = 0; k < n; k++) {
    w[k] = 0.5 - 0.5 * c;
    const double cn = c * c0 - s * s0;
    s = s * c0 + c * s0;
    c = cn;
  }
}

inline double chan_mean(const uint16_t* d, int n, int ch) {
  double s = 0;
  for (int k = 0; k < n; k++) s += d[2 * k + ch];
  return s / n;
}

// sum_k w[k] * (x[k]-mean) * exp(-j*om*(k + ch/3))
inline cx demod(const uint16_t* d, int n, int ch, double om, const double* w, double mean) {
  const double ph0 = -om * (ch / 2.0);
  double c = std::cos(ph0), s = std::sin(ph0);
  const double rc = std::cos(om), rs = -std::sin(om);
  double sr = 0, si = 0;
  for (int k = 0; k < n; k++) {
    const double v = w[k] * (d[2 * k + ch] - mean);
    sr += v * c;
    si += v * s;
    const double cn = c * rc - s * rs;
    s = s * rc + c * rs;
    c = cn;
  }
  return cx(sr, si);
}

inline double ref_mag(const uint16_t* d, int n, const double* w, double mean, double om) {
  return std::abs(demod(d, n, 1, om, w, mean));
}

// Peak search on the reference channel, cheap enough for a soft-float RISC-V:
// coarse scan on 192 frames, then 2 rounds of 3-point parabolic refinement on 192/768/3072 frames.
inline double find_omega(const uint16_t* d, int N, double om0, double* w) {
  int st[4], ns = 0;
  const int cand[3] = {192, 768, 3072};
  for (int i = 0; i < 3; i++)
    if (cand[i] < N) st[ns++] = cand[i];
  if (N <= 3072) st[ns++] = N;
  double o = om0;
  for (int si = 0; si < ns; si++) {
    const int n = st[si];
    hann(w, n);
    const double mean = chan_mean(d, n, 1);
    double dd = M_PI / n;  // half bin
    if (si == 0) {
      const int m = (int)std::ceil(0.04 * om0 / dd);
      double best = -1, ob = o;
      for (int i = -m; i <= m; i++) {
        const double mg = ref_mag(d, n, w, mean, om0 + dd * i);
        if (mg > best) { best = mg; ob = om0 + dd * i; }
      }
      o = ob;
    }
    for (int r = 0; r < 2; r++) {
      const double m0 = ref_mag(d, n, w, mean, o - dd);
      const double m1 = ref_mag(d, n, w, mean, o);
      const double m2 = ref_mag(d, n, w, mean, o + dd);
      const double den = m0 - 2 * m1 + m2;
      if (den != 0) o += 0.5 * dd * (m0 - m2) / den;
      dd *= 0.5;
    }
  }
  return o;
}

struct LockIn {
  cx h;          // X0/X1
  double refmag; // |X1| (drive present?)
  double omega;
};

// d: N interleaved frames [node A, ref]. w: scratch, >= N doubles. om0 = 2*pi*f/frame_rate.
inline LockIn lockin(const uint16_t* d, int N, double om0, double* w) {
  const double om = find_omega(d, N, om0, w);
  hann(w, N);
  const cx x0 = demod(d, N, 0, om, w, chan_mean(d, N, 0));
  const cx x1 = demod(d, N, 1, om, w, chan_mean(d, N, 1));
  const double m = std::abs(x1);
  return {m > 0 ? x0 / x1 : cx(0, 0), m, om};
}


// ---------------------------------------------------------------- calibration data
enum : uint32_t { F_OPEN = 1, F_SHORT = 2, F_RES = 4, F_CAP0 = 8, F_CAP1 = 16 };
constexpr uint32_t CAL_MAGIC = 0xCA9C0003u;

struct Cal {
  uint32_t magic = CAL_MAGIC;
  uint32_t flags = 0;
  cx g0[NFREQ];       // open-clip ratio
  cx z0[NFREQ];       // shorted-clip impedance (test leads)
  double sh[NFREQ];   // per-capture noise of H (std per component), from the open calibration repeats
  double rtrim = 1.0; // known-resistor gain trim
  double cref[2] = {2.2, 100.0};  // C [uF] of the known capacitors used for the trims
  double ctrim[2] = {1.0, 1.0};
  Cal() { for (int i = 0; i < NFREQ; i++) { g0[i] = 0; z0[i] = 0; sh[i] = 0; } }
  bool ready() const { return (flags & F_OPEN) && (flags & F_SHORT); }
};

inline double ctrim_for(const Cal& cal, double c_uF) {
  const bool a = cal.flags & F_CAP0, b = cal.flags & F_CAP1;
  if (!a && !b) return 1.0;
  if (a && !b) return cal.ctrim[0];
  if (b && !a) return cal.ctrim[1];
  if (cal.cref[1] <= cal.cref[0] * 1.2) {  // two refs of nearly equal value: use the closer one
    return std::fabs(std::log(c_uF / cal.cref[0])) < std::fabs(std::log(c_uF / cal.cref[1])) ? cal.ctrim[0] : cal.ctrim[1];
  }
  double x = std::log(c_uF / cal.cref[0]) / std::log(cal.cref[1] / cal.cref[0]);
  if (x < 0) x = 0;
  if (x > 1) x = 1;
  return cal.ctrim[0] + (cal.ctrim[1] - cal.ctrim[0]) * x;
}

inline cx zmeas(const Cal& cal, int fi, cx h) { return (RCAL * cal.rtrim) * h / (cal.g0[fi] - h); }

// ---------------------------------------------------------------- repeat accumulation and uncertainty
struct Acc {
  int k = 0;
  cx s = 0;
  double sr2 = 0, si2 = 0, f = 0;
  void add(cx h, double fa) { k++; s += h; sr2 += h.real() * h.real(); si2 += h.imag() * h.imag(); f = fa; }
  cx mean() const { return k ? s / (double)k : cx(0, 0); }
  double sd() const {  // pooled per-component sample std of H
    if (k < 2) return 0;
    const cx m = mean();
    const double v = (sr2 - k * m.real() * m.real() + si2 - k * m.imag() * m.imag()) / (2.0 * (k - 1));
    return v > 0 ? std::sqrt(v) : 0;
  }
};

struct Pt {
  double f = 0;
  cx z;          // DUT impedance (leads removed)
  double a = 0;  // measurement noise of H (after averaging k captures) as sigma(Z) = a * |Z + zoff|^2
  double ac = 0; // noise of the stored open calibration G0: a FIXED error in every test, sigma(Z) = ac * |Z+zoff| * |Z+z0|
  cx zoff, z0;   // zoff = z0 + RCAL*rtrim
  int k = 0;
  double sig_meas(cx zd) const { return a * std::norm(zd + zoff); }
  double sig(cx zdut) const {
    const double m = std::abs(zdut + zoff);
    const double s = m * std::sqrt(a * a * m * m + ac * ac * std::norm(zdut + z0));
    return s > 1e-4 ? s : 1e-4;
  }
};

inline Pt make_pt(const Cal& cal, int fi, const Acc& a) {
  Pt p;
  p.f = a.f;
  p.k = a.k;
  const cx h = a.mean();
  const double rc = RCAL * cal.rtrim;
  const cx zm = zmeas(cal, fi, h);
  p.z = zm - cal.z0[fi];
  const double sh = cal.sh[fi] > 0 ? cal.sh[fi] : H_NOISE_DEFAULT;
  double sh1 = sh / std::sqrt((double)a.k);
  if (a.k >= 3) sh1 = std::fmax(sh1, a.sd() / std::sqrt((double)a.k));
  const double ng = rc * std::abs(cal.g0[fi]);
  p.a = sh1 / ng;
  p.ac = (sh / std::sqrt((double)K_OPEN)) / ng;
  p.zoff = cal.z0[fi] + rc;
  p.z0 = cal.z0[fi];
  return p;
}

// ---------------------------------------------------------------- model fit
struct Fit {
  double esr = 0;     // ohm
  double c = 0;       // farad (INFINITY if not capacitive)
  double sesr = 0;    // statistical 1-sigma of ESR [ohm]
  double sc_rel = 1;  // statistical 1-sigma of C, relative
  double chi2red = 1; // fit quality: >>1 means the data do not follow ESR + 1/(jwC)
};

// Weighted fit of Z = ESR - j/(wC) using the propagated measurement noise of every point as weight.
// ESL is < 1% of |Z| below 15 kHz for the caps of interest and is not fitted.
inline Fit fit(const Pt* p, int n) {
  double w[NFREQ], x[NFREQ], sg[NFREQ], c0 = 0, med[NFREQ];
  int nm = 0;
  for (int i = 0; i < n; i++) {
    w[i] = 2 * M_PI * p[i].f;
    x[i] = 1.0 / w[i];
    const double a = std::fabs(p[i].z.imag());
    if (a > 0.02 && a < 400) med[nm++] = 1.0 / (w[i] * a);
  }
  if (nm) {  // median
    for (int i = 1; i < nm; i++) { double v = med[i]; int j = i - 1; while (j >= 0 && med[j] > v) { med[j + 1] = med[j]; j--; } med[j + 1] = v; }
    c0 = (nm & 1) ? med[nm / 2] : 0.5 * (med[nm / 2 - 1] + med[nm / 2]);
  } else {
    c0 = 1.0 / (w[0] * std::fmax(std::abs(p[0].z), 1e-3));
  }
  // pass 1: sigma from the measured Z; pass 2: sigma from the model Z (a noise-shrunk point must not get extra weight)
  cx zm[NFREQ];
  for (int i = 0; i < n; i++) zm[i] = p[i].z;
  double esr = 0, d = 1.0 / c0, sw = 0, swxx = 0;
  for (int pass = 0; pass < 3; pass++) {
    sw = swxx = 0;
    double swr = 0, swx = 0;
    for (int i = 0; i < n; i++) {
      sg[i] = p[i].sig(zm[i]);
      const double wt = 1.0 / (sg[i] * sg[i]);
      sw += wt;
      swr += wt * p[i].z.real();
      swx += wt * p[i].z.imag() * x[i];
      swxx += wt * x[i] * x[i];
    }
    esr = swr / sw;
    d = -swx / swxx;
    const double dm = d > 0 ? d : 1.0 / c0;
    for (int i = 0; i < n; i++) zm[i] = cx(esr > 0 ? esr : 0, -dm * x[i]);
  }
  Fit r;
  r.esr = esr;
  r.c = d > 0 ? 1.0 / d : INFINITY;
  double chi2 = 0;
  for (int i = 0; i < n; i++) {
    const double wt = 1.0 / (sg[i] * sg[i]);
    chi2 += wt * (std::pow(p[i].z.real() - esr, 2) + std::pow(p[i].z.imag() + d * x[i], 2));
  }
  const int dof = 2 * n - 2;
  r.chi2red = dof > 0 ? chi2 / dof : 1.0;
  const double scale = r.chi2red > 1.0 ? std::sqrt(r.chi2red) : 1.0;
  r.sesr = std::sqrt(1.0 / sw) * scale;
  r.sc_rel = d > 0 ? std::sqrt(1.0 / swxx) * scale / d : 1.0;
  return r;
}

// choose frequencies where |Z| (extrapolated from the 1 kHz probe) is measurable
inline int pick(cx z1k, int* idx) {
  int n = 0;
  const double xc = std::fabs(z1k.imag()) * 1000.0;
  const double esr = z1k.real() > 0 ? z1k.real() : 0;
  for (int i = 0; i < NFREQ; i++) {
    const double ze = std::hypot(esr, xc / FREQS[i]);
    if (ze >= 0.015 && ze <= 400.0) idx[n++] = i;
  }
  if (!n) idx[n++] = PROBE_IDX;
  return n;
}

// ---------------------------------------------------------------- learned good parts
struct Ref { double nominal = 0, c_uF = 0, esr = 0; int n = 0; };
constexpr int MAX_REFS = 16;
struct RefTable {
  uint32_t magic = 0xCA9C0102u;
  int n = 0;
  Ref r[MAX_REFS];
};
inline int ref_index(const RefTable& t, double nominal) {
  for (int i = 0; i < t.n; i++)
    if (std::fabs(t.r[i].nominal - nominal) < 0.005 * nominal) return i;
  return -1;
}
// running average over all good parts learned for this nominal value
inline bool ref_learn(RefTable& t, double nominal, double c_uF, double esr) {
  int i = ref_index(t, nominal);
  if (i < 0) {
    if (t.n >= MAX_REFS) return false;
    i = t.n++;
    t.r[i] = Ref();
    t.r[i].nominal = nominal;
  }
  Ref& r = t.r[i];
  r.c_uF = (r.c_uF * r.n + c_uF) / (r.n + 1);
  r.esr = (r.esr * r.n + (esr > 0 ? esr : 0)) / (r.n + 1);
  r.n++;
  return true;
}
inline bool ref_delete(RefTable& t, int i) {
  if (i < 0 || i >= t.n) return false;
  for (int k = i; k + 1 < t.n; k++) t.r[k] = t.r[k + 1];
  t.n--;
  return true;
}

// ---------------------------------------------------------------- verdict
enum Code { V_GOOD, V_WEAK, V_BAD, V_OPEN, V_SHORT, V_PARALLEL, V_ERROR };
static const char* const CODE_NAME[] = {"GOOD", "WEAK", "BAD", "OPEN", "SHORT", "PARALLEL R", "ERROR"};

struct Report {
  Code code = V_ERROR;
  Code best = V_ERROR, worst = V_ERROR;  // verdict if the true value is at the favourable / unfavourable end of the error bars
  bool sure = true;                       // best == worst
  double c_uF = 0, sc_rel = 0, esr = 0, sesr = 0, z1k = 0, esrc = 0, gexcess_mS = 0, b_mS = 0, rpar = 0, chi2red = 1;
  double ref_nominal = 0, ref_c = 0, ref_esr = 0;  // learned good part used for the comparison (0 = none)
  int npts = 0, ncap = 0;
  double pf[NFREQ], pre[NFREQ], pim[NFREQ];  // measured Z per frequency (for diagnostics)
  char l1[24] = "", l2[24] = "", l3[24] = "", l4[24] = "", l5[24] = "";  // 72x40 OLED: 12 chars per line
  char why[160] = "";
};

inline void fmt_c(char* o, size_t n, double uF) {
  if (uF >= 10000) std::snprintf(o, n, "C %.1fmF", uF / 1000);
  else if (uF >= 1000) std::snprintf(o, n, "C %.2fmF", uF / 1000);
  else if (uF >= 100) std::snprintf(o, n, "C %.0fuF", uF);
  else if (uF >= 10) std::snprintf(o, n, "C %.1fuF", uF);
  else std::snprintf(o, n, "C %.2fuF", uF);
}
inline void fmt_esr(char* o, size_t n, double e) {
  if (e < 0) e = 0;
  if (e < 0.1) std::snprintf(o, n, "ESR %.0fmR", e * 1000);
  else std::snprintf(o, n, "ESR %.2fR", e);
}
inline void fmt_unc(char* o, size_t n, double sesr, double sc_rel) {  // "+-.04R +-5%" (<= 12 chars)
  char a[12], b[10];
  if (sesr < 0.0095) std::snprintf(a, sizeof a, "+-%.0fmR", sesr * 1000 + 0.5);
  else if (sesr < 0.995) std::snprintf(a, sizeof a, "+-.%02.0fR", sesr * 100);
  else std::snprintf(a, sizeof a, "+-%.1fR", sesr);
  if (sc_rel >= 0.995) std::snprintf(b, sizeof b, ">99%%");
  else std::snprintf(b, sizeof b, "+-%.0f%%", sc_rel * 100);
  std::snprintf(o, n, "%s %s", a, b);
}

struct Judged { Code code; const char* why1; const char* why2; };

// classify one (C, ESR) point: with a learned good part `ref` (preferred) or a nominal value / ESR*C rule
inline Judged classify(double c_uF, double esr, double nominal_uF, const Ref* ref) {
  Code code = V_GOOD;
  const char* why1 = "";
  const char* why2 = "";
  auto raise = [&](Code k) { if (k > code) code = k; };
  if (ref) {
    const double cr = c_uF / ref->c_uF;
    if (cr < REF_C_BAD) { raise(V_BAD); why1 = "C LOW"; }
    else if (cr < REF_C_WEAK) { raise(V_WEAK); why1 = "C DOWN"; }
    else if (cr > REF_C_HIGH) { raise(V_WEAK); why1 = "C HIGH?"; }
    const double base = ref->esr > ESR_UNC ? ref->esr : ESR_UNC;
    if (esr > REF_ESR_BAD * base + ESR_UNC) { raise(V_BAD); why2 = "ESR HIGH"; }
    else if (esr > REF_ESR_WEAK * base + ESR_UNC) { raise(V_WEAK); why2 = "ESR UP"; }
  } else {
    if (nominal_uF > 0) {
      if (c_uF < C_LOW_FRAC * nominal_uF) { raise(V_BAD); why1 = "C LOW"; }
      else if (c_uF > C_HIGH_FRAC * nominal_uF) { raise(V_WEAK); why1 = "C HIGH?"; }
    }
    const double esrc = (esr > ESR_UNC ? esr - ESR_UNC : 0.0) * c_uF;
    if (esrc > ESRC_BAD) { raise(V_BAD); why2 = "ESR HIGH"; }
    else if (esrc > ESRC_MARGINAL) { raise(V_WEAK); why2 = "ESR UP"; }
  }
  return {code, why1, why2};
}

inline void judge(Report& r, double nominal_uF, const RefTable* refs) {
  const double c = r.c_uF, esr = r.esr;
  r.esrc = (esr > ESR_UNC ? esr - ESR_UNC : 0.0) * c;
  fmt_c(r.l2, sizeof r.l2, c);
  fmt_esr(r.l3, sizeof r.l3, esr);
  fmt_unc(r.l4, sizeof r.l4, r.sesr, r.sc_rel);
  if (r.gexcess_mS > PAR_G_MS && r.gexcess_mS > PAR_G_REL * r.b_mS) {
    r.rpar = 1000.0 / r.gexcess_mS;
    r.code = r.best = r.worst = V_PARALLEL;
    std::snprintf(r.l1, sizeof r.l1, "PARALLEL R");
    std::snprintf(r.l4, sizeof r.l4, "~%.0fR", r.rpar);
    std::snprintf(r.l5, sizeof r.l5, "LEAK/CIRCUIT");
    std::snprintf(r.why, sizeof r.why, "Resistive path ~%.0f ohm in parallel: leaky cap or other circuit", r.rpar);
    return;
  }
  // learned good part: exact nominal, or (AUTO) the closest one within +-30 % of the measured C
  const Ref* ref = nullptr;
  if (refs) {
    if (nominal_uF > 0) { const int i = ref_index(*refs, nominal_uF); if (i >= 0) ref = &refs->r[i]; }
    else {
      double bestd = std::log(1.3);
      for (int i = 0; i < refs->n; i++) {
        const double dd = std::fabs(std::log(c / refs->r[i].c_uF));
        if (dd < bestd) { bestd = dd; ref = &refs->r[i]; }
      }
    }
  }
  if (ref) { r.ref_nominal = ref->nominal; r.ref_c = ref->c_uF; r.ref_esr = ref->esr; }
  const Judged j = classify(c, esr, nominal_uF, ref);
  // verdict at the favourable and unfavourable end of the error bars (1 sigma)
  const double sc = r.sc_rel * c;
  Code jb = V_BAD, jw = V_GOOD;  // best / worst verdict over the four corners of the error box (a product like ESR*C is not monotonic in C alone)
  for (int ic = -1; ic <= 1; ic += 2)
    for (int ie = -1; ie <= 1; ie += 2) {
      const double cc = c + ic * sc, ee = esr + ie * r.sesr;
      const Code k = classify(cc > 0 ? cc : 0.0, ee > 0 ? ee : 0.0, nominal_uF, ref).code;
      if (k < jb) jb = k;
      if (k > jw) jw = k;
    }
  r.code = j.code;
  r.best = jb;
  r.worst = jw;
  r.sure = jb == jw;
  std::snprintf(r.l1, sizeof r.l1, "%s%s", CODE_NAME[j.code], r.sure ? "" : "?");
  if (!r.sure) std::snprintf(r.l5, sizeof r.l5, "BORDERLINE");
  else if (*j.why1) std::snprintf(r.l5, sizeof r.l5, "%s", j.why1);
  else if (*j.why2) std::snprintf(r.l5, sizeof r.l5, "%s", j.why2);
  else if (ref) std::snprintf(r.l5, sizeof r.l5, "VS REF %g", ref->nominal);
  else std::snprintf(r.l5, sizeof r.l5, "ESR*C %.0f", r.esrc);
  if (r.sure)
    std::snprintf(r.why, sizeof r.why, "%s: C=%.3f uF +-%.1f%%, ESR=%.3f +-%.3f ohm, ESR*C=%.0f. %s %s", CODE_NAME[j.code], r.c_uF,
                  r.sc_rel * 100, r.esr, r.sesr, r.esrc, j.why1, j.why2);
  else
    std::snprintf(r.why, sizeof r.why, "%s? C=%.3f uF +-%.1f%%, ESR=%.3f +-%.3f ohm, ESR*C=%.0f (error bars span %s..%s) %s %s",
                  CODE_NAME[j.code], r.c_uF, r.sc_rel * 100, r.esr, r.sesr, r.esrc, CODE_NAME[jb], CODE_NAME[jw],
                  j.why1, j.why2);
}

// ---------------------------------------------------------------- acquisition helpers
// Acq must provide:  bool get(const Plan& p, uint16_t* frames)   // fills p.n frames, interleaved [ch0,ch1]
//                    double f_actual(double nominal_hz)            // real drive frequency of the last get()
template <class Acq>
inline bool capture_h(Acq& acq, int fi, uint16_t* buf, double* w, cx* h, double* fa) {
  const Plan p = plan(fi);
  if (!acq.get(p, buf)) return false;
  *fa = acq.f_actual(p.f);  // frequency the LEDC really produced (needed for C = 1/(w*|Im Z|))
  const LockIn li = lockin(buf, p.n, 2 * M_PI * p.f / p.frame_rate, w);
  *h = li.h;
  return li.refmag > 1000.0;  // drive reference must be present
}

// step 1 of calibration: clips OPEN, apart. K_OPEN captures per frequency -> mean G0 and the noise floor of H.
// Redoing this step invalidates everything measured against the old G0, so later steps are cleared.
template <class Acq, class Progress>
inline bool calibrate_open(Acq& acq, Cal& cal, uint16_t* buf, double* w, Progress progress) {
  constexpr int K = K_OPEN;
  Cal c;
  for (int i = 0; i < NFREQ; i++) {
    Acc a;
    for (int k = 0; k < K; k++) {
      cx h; double fa;
      progress(i * K + k, NFREQ * K);
      if (!capture_h(acq, i, buf, w, &h, &fa)) return false;
      a.add(h, fa);
    }
    c.g0[i] = a.mean();
    c.sh[i] = a.sd();
  }
  c.flags = F_OPEN;
  cal = c;
  return true;
}

// step 2: clips SHORTED together (needs G0). Stores the impedance of the leads alone.
template <class Acq, class Progress>
inline bool calibrate_short(Acq& acq, Cal& cal, uint16_t* buf, double* w, Progress progress) {
  if (!(cal.flags & F_OPEN)) return false;
  constexpr int K = K_SHORT;
  Cal c = cal;
  c.rtrim = 1.0;
  for (int i = 0; i < NFREQ; i++) c.z0[i] = 0;
  for (int i = 0; i < NFREQ; i++) {
    Acc a;
    for (int k = 0; k < K; k++) {
      cx h; double fa;
      progress(i * K + k, NFREQ * K);
      if (!capture_h(acq, i, buf, w, &h, &fa)) return false;
      a.add(h, fa);
    }
    c.z0[i] = zmeas(c, i, a.mean());
  }
  if (std::abs(c.z0[PROBE_IDX]) > 3.0) return false;  // a real short is ~0.1-0.5 ohm; more means the clips are not touching

  for (int i = 0; i < NFREQ; i++) cal.z0[i] = c.z0[i];
  cal.flags |= F_SHORT;
  return true;
}

// step 3: known resistor (0.5 .. 50 ohm, 1 % or better; 4.7..10 ohm is best) across the clips. Sets the gain trim.
// Returns 0 = ok, 1 = no signal, 2 = reading too far from the stated value (wrong value or clips not connected).
template <class Acq, class Progress>
inline int calibrate_res(Acq& acq, Cal& cal, double r_known, uint16_t* buf, double* w, Progress progress, double* r_meas = nullptr) {
  if (!cal.ready()) return 1;
  constexpr int FI[3] = {2, 3, 4};  // 100 Hz, 300 Hz, 1 kHz: purely resistive there
  constexpr int K = 6;
  Cal c = cal;
  c.rtrim = 1.0;
  double sum = 0, sumi = 0;
  for (int j = 0; j < 3; j++) {
    Acc a;
    for (int k = 0; k < K; k++) {
      cx h; double fa;
      progress(j * K + k, 3 * K);
      if (!capture_h(acq, FI[j], buf, w, &h, &fa)) return 1;
      a.add(h, fa);
    }
    const cx z = zmeas(c, FI[j], a.mean()) - c.z0[FI[j]];
    sum += z.real();
    sumi += z.imag();
  }
  const double rm = sum / 3, im = sumi / 3;
  if (r_meas) *r_meas = rm;
  if (rm <= 0.05 || std::fabs(rm / r_known - 1.0) > 0.25 || std::fabs(im) > 0.3 * r_known + 0.05) return 2;
  cal.rtrim = r_known / rm;
  cal.flags |= F_RES;
  return 0;
}

// ---------------------------------------------------------------- test sequence
struct Measurement {   // result of the repeat-averaged measurement of one part (before the verdict)
  bool ok = false;
  bool signal = true;
  cx z1k;
  Fit fit;
  int n = 0, ncap = 0;
  Pt pt[NFREQ];
  int idx[NFREQ];
  double f[NFREQ];
};

// Probe at 1 kHz, choose frequencies, capture `kbase` times each, then add captures at the most informative frequency until the
// statistical error targets are met or the budget (LEVELS[level].kmax_total) is used up.
template <class Acq, class Progress>
inline Measurement measure(Acq& acq, const Cal& cal, int level, uint16_t* buf, double* w, Progress progress) {
  Measurement m;
  Acc acc[NFREQ];
  const LevelCfg lc = LEVELS[level];
  auto grab = [&](int fi) {
    cx h; double fa;
    if (!capture_h(acq, fi, buf, w, &h, &fa)) return false;
    acc[fi].add(h, fa);
    m.ncap++;
    return true;
  };
  progress(0, 1);
  if (!grab(PROBE_IDX)) { m.signal = false; return m; }
  m.z1k = make_pt(cal, PROBE_IDX, acc[PROBE_IDX]).z;
  if (std::abs(m.z1k) > OPEN_Z1K) { m.ok = true; m.n = 0; return m; }  // caller reports OPEN
  m.n = pick(m.z1k, m.idx);
  const int planned = m.n * lc.kbase + lc.kmax_total;
  for (int i = 0; i < m.n; i++)
    while (acc[m.idx[i]].k < lc.kbase) {
      progress(m.ncap, planned);
      if (!grab(m.idx[i])) { m.signal = false; return m; }
    }
  double hist_c[64], hist_e[64];
  int nh = 0;
  for (;;) {
    for (int i = 0; i < m.n; i++) { m.pt[i] = make_pt(cal, m.idx[i], acc[m.idx[i]]); m.f[i] = m.pt[i].f; }
    m.fit = fit(m.pt, m.n);
    const bool cap_ok = std::isfinite(m.fit.c);
    const double ec = m.fit.sc_rel, ee = m.fit.sesr;
    const bool good_enough = !cap_ok || (ec <= TGT_C_REL && ee <= TGT_ESR_ABS + 0.02 * std::fabs(m.fit.esr));
    int spent = m.ncap - m.n * lc.kbase;
    if (nh < 64) { hist_c[nh] = ec; hist_e[nh] = ee; nh++; }
    // stop when the error stopped shrinking (the rest is the fixed calibration noise, repeats cannot fix it)
    const bool stalled = nh > 8 && ec > 0.97 * hist_c[nh - 9] && ee > 0.97 * hist_e[nh - 9];
    if (good_enough || stalled || spent >= lc.kmax_total) break;
    // which target is further away decides what the next capture should improve
    const bool want_c = ec / TGT_C_REL >= ee / (TGT_ESR_ABS + 0.02 * std::fabs(m.fit.esr));
    int bi = 0;
    double bs = -1;
    for (int i = 0; i < m.n; i++) {
      const double sg = m.pt[i].sig(cx(m.fit.esr > 0 ? m.fit.esr : 0, -1.0 / (2 * M_PI * m.pt[i].f * m.fit.c)));
      const double x = 1.0 / (2 * M_PI * m.pt[i].f);
      const double score = (want_c ? x * x : 1.0) / (sg * sg) / (m.pt[i].k + 1.0);
      if (score > bs) { bs = score; bi = i; }
    }
    progress(m.ncap, planned);
    if (!grab(m.idx[bi])) { m.signal = false; return m; }
  }
  m.ok = true;
  return m;
}

template <class Acq, class Progress>
inline Report run_test(Acq& acq, const Cal& cal, const RefTable* refs, double nominal_uF, int level, uint16_t* buf, double* w,
                       Progress progress) {
  Report r;
  const Measurement m = measure(acq, cal, level, buf, w, progress);
  r.ncap = m.ncap;
  if (!m.signal) {
    std::snprintf(r.l1, sizeof r.l1, "NO SIGNAL");
    std::snprintf(r.l2, sizeof r.l2, "check drive/");
    std::snprintf(r.l3, sizeof r.l3, "ADC wiring");
    std::snprintf(r.why, sizeof r.why, "No drive reference seen by the ADC: check wiring");
    return r;
  }
  r.z1k = std::abs(m.z1k);
  if (m.n == 0) {
    r.code = r.best = r.worst = V_OPEN;
    std::snprintf(r.l1, sizeof r.l1, "OPEN");
    std::snprintf(r.l2, sizeof r.l2, "C < 0.1uF");
    std::snprintf(r.l3, sizeof r.l3, "or no cap");
    std::snprintf(r.why, sizeof r.why, "|Z| at 1 kHz = %.0f ohm: open, bad contact or tiny cap", r.z1k);
    return r;
  }
  const Fit& ft = m.fit;
  r.npts = m.n;
  for (int i = 0; i < m.n; i++) { r.pf[i] = m.f[i]; r.pre[i] = m.pt[i].z.real(); r.pim[i] = m.pt[i].z.imag(); }
  r.chi2red = ft.chi2red;
  const double sys_rel = (cal.flags & F_RES) ? SYS_REL_TRIMMED : SYS_REL_UNTRIMMED;
  const bool cal_c = (cal.flags & (F_CAP0 | F_CAP1)) != 0;
  const double sys_c = cal_c ? SYS_C_TRIMMED : SYS_C_UNTRIMMED;
  r.esr = ft.esr;
  r.sesr = std::sqrt(ft.sesr * ft.sesr + ESR_CONTACT * ESR_CONTACT + std::pow(sys_rel * (ft.esr > 0 ? ft.esr : 0), 2));
  if (!std::isfinite(ft.c) || ft.c * 1e6 > CMAX_UF) {
    r.c_uF = INFINITY;
    r.code = r.best = r.worst = V_SHORT;
    std::snprintf(r.l1, sizeof r.l1, "SHORT");
    std::snprintf(r.l2, sizeof r.l2, "R %.2f ohm", ft.esr < 0 ? 0.0 : ft.esr);
    std::snprintf(r.l3, sizeof r.l3, "no C seen");
    std::snprintf(r.why, sizeof r.why, "Resistive/short: %.3f ohm, no capacitive reactance", ft.esr);
    return r;
  }
  r.c_uF = ft.c * 1e6;
  r.c_uF *= ctrim_for(cal, r.c_uF);
  r.sc_rel = std::sqrt(ft.sc_rel * ft.sc_rel + sys_rel * sys_rel + sys_c * sys_c);
  // excess parallel conductance from the two lowest usable frequencies (<=100 Hz): G - ESR*B^2
  double gsum = 0, bsum = 0;
  int gn = 0;
  for (int i = 0; i < m.n && gn < 2; i++) {
    if (m.f[i] > 100.0) break;
    const cx y = 1.0 / m.pt[i].z;
    const double b = -y.imag();
    gsum += y.real() - ft.esr * b * b;
    bsum += std::fabs(b);
    gn++;
  }
  r.gexcess_mS = gn ? 1000.0 * gsum / gn : 0.0;
  r.b_mS = gn ? 1000.0 * bsum / gn : 0.0;
  judge(r, nominal_uF, refs);
  return r;
}

// step 4: known capacitor (value in uF, ideally 1 % film/C0G or a measured-elsewhere cap). Sets the C gain trim for that range
// (slot 0 for <= 3 uF, slot 1 above; between two trims the factor is interpolated on a log scale).
// Returns 0 = ok, 1 = no signal / not a capacitor, 2 = reading too far from the stated value.
template <class Acq, class Progress>
inline int calibrate_cap(Acq& acq, Cal& cal, double c_known_uF, uint16_t* buf, double* w, Progress progress, double* c_meas = nullptr) {
  if (!cal.ready()) return 1;
  Cal c = cal;
  c.ctrim[0] = c.ctrim[1] = 1.0;
  c.flags &= ~(F_CAP0 | F_CAP1);
  const Measurement m = measure(acq, c, LV_PRECISE, buf, w, progress);
  if (!m.signal || m.n == 0 || !std::isfinite(m.fit.c)) return 1;
  const double cm = m.fit.c * 1e6;
  if (c_meas) *c_meas = cm;
  const double ratio = c_known_uF / cm;
  if (ratio < 0.7 || ratio > 1.45) return 2;
  const int s = c_known_uF <= 3.0 ? 0 : 1;
  cal.cref[s] = c_known_uF;
  cal.ctrim[s] = ratio;
  cal.flags |= s ? F_CAP1 : F_CAP0;
  return 0;
}

// ---------------------------------------------------------------- "start when the probes are on the capacitor"
enum WaitResult { WAIT_OK, WAIT_TIMEOUT, WAIT_CANCEL, WAIT_NOSIGNAL };

// one quick look at the clips: |Z| at 1 kHz (leads removed). false = no drive reference seen (or acquisition cancelled)
template <class Acq>
inline bool probe_z(Acq& acq, const Cal& cal, uint16_t* buf, double* w, double* zmag) {
  cx h;
  double fa;
  if (!capture_h(acq, PROBE_IDX, buf, w, &h, &fa)) return false;
  Acc a;
  a.add(h, fa);
  *zmag = std::abs(make_pt(cal, PROBE_IDX, a).z);
  return true;
}

// Probes the clips repeatedly until CONNECT_STABLE consecutive probes all show |Z| < CONNECT_Z and agree with each other
// (a clip that is only touching, or still being wiggled, keeps resetting the count): then returns WAIT_OK.
//   now_s()                 -> seconds (monotonic)           cancel() -> true to give up
//   status(good, need, |Z|) -> called after every probe (progress display)
template <class Acq, class Clock, class Cancel, class Status>
inline WaitResult wait_connected(Acq& acq, const Cal& cal, uint16_t* buf, double* w, double timeout_s, Clock now_s, Cancel cancel,
                                 Status status) {
  const double t0 = now_s();
  int good = 0;
  double last = 0, t_good = 0;
  for (;;) {
    if (cancel()) return WAIT_CANCEL;
    if (now_s() - t0 > timeout_s) return WAIT_TIMEOUT;
    double z;
    if (!probe_z(acq, cal, buf, w, &z)) return cancel() ? WAIT_CANCEL : WAIT_NOSIGNAL;
    const bool conn = z < CONNECT_Z;
    if (conn && (good == 0 || std::fabs(z - last) <= CONNECT_JITTER * std::fmax(z, last) + 0.05)) good++;
    else good = conn ? 1 : 0;
    if (good == 1) t_good = now_s();
    last = z;
    status(good, CONNECT_STABLE, z);
    if (good >= CONNECT_STABLE && now_s() - t_good >= CONNECT_HOLD_S) return WAIT_OK;
  }
}

// step 6: learn a known-good part. Returns the measurement report; the caller commits it with ref_learn() if acceptable.
inline bool ref_acceptable(const Report& r) { return r.code == V_GOOD || r.code == V_WEAK || r.code == V_BAD; }

}  // namespace cap
