// Host test: runs the exact firmware sequence (capdsp.h) on synthesised ADC captures. Captures are generated on the fly from the
// harmonic transfer tables written by gen_cases.py (exact model of the schematic + 3 mV rms ADC noise + random ADC clock error),
// so every repeat has independent noise. Reports measured error vs. the reported error bars (coverage) for each case.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include "../src/capdsp.h"
using namespace cap;

constexpr int NH = 30;
struct Var { std::vector<cx> gp, gr; };  // [fi*NH + h]
struct Case { char name[33]; double nominal, c_true, esr_true; std::vector<Var> var; };

struct SimAcq {
  const Case* c = nullptr;
  int variant = 0;
  double fs_err = 1.0, noise_mv = 3.0;
  std::mt19937_64 rng{12345};
  double last_f = 0;
  double f_actual(double f) { return f; }
  bool get(const Plan& p, uint16_t* buf) {
    int fi = 0;
    for (int i = 0; i < NFREQ; i++) if (FREQS[i] == p.f) fi = i;
    const Var& v = c->var[variant];
    std::normal_distribution<double> nd(0.0, noise_mv * 1e-3);
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    const double fframe = p.frame_rate * fs_err, t0 = ud(rng) / p.f;
    for (int ch = 0; ch < 2; ch++) {
      cx ph[NH], st[NH];
      for (int h = 0; h < NH; h++) {
        const int n = 2 * h + 1;
        const double amp = 3.3 * 2 / (n * M_PI);
        const cx g = (ch ? v.gr : v.gp)[fi * NH + h];
        const double w = 2 * M_PI * p.f * n;
        const double ts = (ch / 2.0) / fframe + t0;
        ph[h] = amp * g * std::polar(1.0, w * ts);
        st[h] = std::polar(1.0, w / fframe);
      }
      const double dc = ch ? 3.3 / 6.6 * 0.5 : 0.579;
      for (int k = 0; k < p.n; k++) {
        double x = dc + nd(rng);
        for (int h = 0; h < NH; h++) { x += ph[h].imag(); ph[h] *= st[h]; }
        int q = (int)std::floor(x / 0.9 * 4096);
        buf[2 * k + ch] = (uint16_t)(q < 0 ? 0 : q > 4095 ? 4095 : q);
      }
    }
    return true;
  }
};

static std::vector<Case> load(const char* fn) {
  FILE* fp = fopen(fn, "rb");
  if (!fp) { perror(fn); exit(1); }
  auto rd = [&](void* p, size_t sz, size_t n) { if (fread(p, sz, n, fp) != n) { puts("short read"); exit(1); } };
  int nc, nf; rd(&nc, 4, 1); rd(&nf, 4, 1);
  if (nf != NFREQ) { puts("frequency count mismatch"); exit(1); }
  std::vector<Case> cs(nc);
  for (auto& c : cs) {
    rd(c.name, 32, 1); c.name[32] = 0;
    int nv; rd(&c.nominal, 8, 1); rd(&c.c_true, 8, 1); rd(&c.esr_true, 8, 1); rd(&nv, 4, 1);
    c.var.resize(nv);
    for (auto& v : c.var) {
      v.gp.resize(NFREQ * NH); v.gr.resize(NFREQ * NH);
      for (int i = 0; i < NFREQ * NH; i++) { double d[4]; rd(d, 8, 4); v.gp[i] = cx(d[0], d[1]); v.gr[i] = cx(d[2], d[3]); }
    }
  }
  fclose(fp);
  return cs;
}

static const Case* find(const std::vector<Case>& cs, const char* n) {
  for (auto& c : cs) if (!strcmp(c.name, n)) return &c;
  fprintf(stderr, "no case %s\n", n); exit(1);
}

int main(int argc, char** argv) {
  const int runs = argc > 2 ? atoi(argv[2]) : 10;
  const int level = argc > 3 ? atoi(argv[3]) : LV_NORMAL;
  const bool do_cal_trim = argc > 4 ? atoi(argv[4]) : 1;
  auto cs = load(argc > 1 ? argv[1] : "test/cases.bin");
  static uint16_t buf[2 * MAX_FRAMES];
  static double w[MAX_FRAMES];
  SimAcq acq;
  auto noprog = [](int, int) {};
  Cal cal;
  acq.c = find(cs, "CAL_OPEN");
  if (!calibrate_open(acq, cal, buf, w, noprog)) { puts("open calibration failed"); return 1; }
  acq.c = find(cs, "CAL_SHORT");
  if (!calibrate_short(acq, cal, buf, w, noprog)) { puts("short calibration failed"); return 1; }
  printf("leads at 1 kHz: %.3f%+.3fj ohm, H noise floor (1 kHz) %.2e\n", cal.z0[PROBE_IDX].real(), cal.z0[PROBE_IDX].imag(), cal.sh[PROBE_IDX]);
  if (do_cal_trim) {
    double rm = 0, cm = 0;
    acq.c = find(cs, "CAL_RES4.7");
    int rc = calibrate_res(acq, cal, 4.7, buf, w, noprog, &rm);
    printf("known resistor 4.7 ohm: rc=%d read %.3f -> rtrim %.4f\n", rc, rm, cal.rtrim);
    acq.c = find(cs, "CAL_CAP1.0");
    rc = calibrate_cap(acq, cal, 1.0, buf, w, noprog, &cm);
    printf("known cap 1.0 uF: rc=%d read %.4f -> ctrim[0] %.4f\n", rc, cm, cal.ctrim[0]);
    acq.c = find(cs, "CAL_CAP100");
    rc = calibrate_cap(acq, cal, 100.0, buf, w, noprog, &cm);
    printf("known cap 100 uF: rc=%d read %.3f -> ctrim[1] %.4f\n", rc, cm, cal.ctrim[1]);
  }
  {  // "start when the probes are on the part": simulated clock (0.15 s per probe); the clips are open, then connect, wiggle off, reconnect
    struct Seq {
      SimAcq* a; const Case* open; const Case* part; int calls = 0, t_on, t_off, t_on2; double t = 0;
      double f_actual(double f) { return f; }
      bool get(const Plan& p, uint16_t* b) {
        calls++; t += 0.15;
        const bool on = (calls >= t_on && calls < t_off) || calls >= t_on2;
        a->c = on ? part : open;
        return a->get(p, b);
      }
    } sq;
    sq.a = &acq; sq.open = find(cs, "CAL_OPEN"); sq.part = find(cs, "CAL_CAP100"); sq.t_on = 6; sq.t_off = 8; sq.t_on2 = 12;
    int max_good = 0;
    const WaitResult wr = wait_connected(sq, cal, buf, w, 60.0, [&] { return sq.t; }, [] { return false; }, [&](int g, int, double) { if (g > max_good) max_good = g; });
    printf("wait_connected: result %d (0 = OK) after %.2f s simulated, %d probes (part touches at probe 6, drops at 8, firm from 12), best streak %d\n", (int)wr, sq.t, sq.calls, max_good);
    if (wr != WAIT_OK || sq.calls < 12 + CONNECT_STABLE - 1 || sq.t - 0.15 * 11 < CONNECT_HOLD_S) { puts("FAIL: wait_connected started too early or failed"); return 1; }
    Seq nc = sq; nc.calls = 0; nc.t = 0; nc.t_on = 1000000; nc.t_off = 1000001; nc.t_on2 = 1000002;
    const WaitResult w2 = wait_connected(nc, cal, buf, w, 5.0, [&] { return nc.t; }, [] { return false; }, [](int, int, double) {});
    printf("wait_connected with clips open: result %d (1 = timeout)\n", (int)w2);
    if (w2 != WAIT_TIMEOUT) { puts("FAIL: no timeout"); return 1; }
    int cn = 0;
    Seq cc = sq; cc.calls = 0; cc.t = 0; cc.t_on = 1000000; cc.t_off = 1000001; cc.t_on2 = 1000002;
    const WaitResult w3 = wait_connected(cc, cal, buf, w, 60.0, [&] { return cc.t; }, [&] { return ++cn > 3; }, [](int, int, double) {});
    printf("wait_connected cancelled: result %d (2 = cancel)\n", (int)w3);
    if (w3 != WAIT_CANCEL) { puts("FAIL: no cancel"); return 1; }
  }
  {  // calibration must reject wrong inputs
    Cal t = cal; double x;
    acq.c = find(cs, "CAL_RES4.7");
    printf("wrong resistor value (says 10 ohm, is 4.7): rc=%d (2 = rejected)\n", calibrate_res(acq, t, 10.0, buf, w, noprog, &x));
    acq.c = find(cs, "CAL_CAP1.0");
    printf("wrong cap value (says 2.2 uF, is 1.0):       rc=%d (2 = rejected)\n", calibrate_cap(acq, t, 2.2, buf, w, noprog, &x));
    acq.c = find(cs, "open");
    printf("open clips during resistor cal:              rc=%d (2 = rejected)\n", calibrate_res(acq, t, 4.7, buf, w, noprog, &x));
    acq.c = find(cs, "CAL_RES4.7");
    printf("resistor during short cal:                   %s\n", calibrate_short(acq, t, buf, w, noprog) ? "accepted?!" : "rejected");
  }
  if (const char* dbg = getenv("DBG")) {   // noiseless per-frequency check against the exact expected impedance
    acq.c = find(cs, dbg); acq.variant = 0; acq.noise_mv = 0;
    printf("DBG %s: per-frequency Z (leads removed) vs expected [truth ESR %.3f, C %.3f uF]\n", dbg, acq.c->esr_true, acq.c->c_true);
    for (int fi = 0; fi < NFREQ; fi++) {
      Acc a; cx h; double fa;
      capture_h(acq, fi, buf, w, &h, &fa); a.add(h, fa);
      const Pt p = make_pt(cal, fi, a);
      const cx ze(acq.c->esr_true, -1.0 / (2 * M_PI * FREQS[fi] * acq.c->c_true * 1e-6));
      printf("  %6.0f Hz  Z = %9.4f %+9.4fj   expected %9.4f %+9.4fj   dRe %+8.4f dIm %+8.4f (%.2f%% of |Z|)\n", FREQS[fi], p.z.real(), p.z.imag(), ze.real(), ze.imag(), p.z.real() - ze.real(), p.z.imag() - ze.imag(), 100 * std::abs(p.z - ze) / std::abs(ze));
    }
    acq.noise_mv = 3.0;
  }
  RefTable refs;
  printf("\nlevel=%d runs/case=%d.  C and ESR: truth | mean measured (sd of runs) | mean reported +-1sigma | coverage of 2 sigma\n", level, runs);
  printf("%-27s %-10s | %9s %16s %9s %5s | %7s %14s %7s %5s | %4s %s\n", "case", "verdict", "C true", "C meas (sd)", "C +-", "cov", "ESR tr", "ESR meas (sd)", "ESR +-", "cov", "ncap", "verdicts");
  std::mt19937_64 rr(99);
  for (size_t ci = 5; ci < cs.size(); ci++) {
    const Case& c = cs[ci];
    acq.c = &c;
    double sc = 0, sc2 = 0, se = 0, se2 = 0, rsc = 0, rse = 0, ncap = 0;
    int ccov = 0, ecov = 0, nv = 0, cnt[7] = {0}, unsure = 0;
    Report last;
    for (int r = 0; r < runs; r++) {
      acq.variant = r % (int)c.var.size();
      acq.fs_err = 1.0 + ((int)(rr() % 3) - 1) * 0.012;
      acq.rng.seed(1000 + 77 * r + ci);
      Report rp = run_test(acq, cal, &refs, c.nominal, level, buf, w, noprog);
      cnt[rp.code]++;
      if (!rp.sure) unsure++;
      last = rp;
      ncap += rp.ncap;
      if (rp.code <= V_BAD || rp.code == V_PARALLEL) {
        nv++;
        sc += rp.c_uF; sc2 += rp.c_uF * rp.c_uF; se += rp.esr; se2 += rp.esr * rp.esr;
        rsc += rp.sc_rel * rp.c_uF; rse += rp.sesr;
        if (std::fabs(rp.c_uF - c.c_true) < 2 * rp.sc_rel * rp.c_uF) ccov++;
        if (std::fabs(rp.esr - c.esr_true) < 2 * rp.sesr) ecov++;
      }
    }
    char vs[80] = "";
    for (int k = 0; k < 7; k++) if (cnt[k]) { char t[24]; snprintf(t, sizeof t, "%s:%d ", CODE_NAME[k], cnt[k]); strcat(vs, t); }
    { char t[24]; snprintf(t, sizeof t, "unsure:%d", unsure); strcat(vs, t); }
    if (nv) {
      const double mc = sc / nv, me = se / nv;
      const double sdc = std::sqrt(std::fmax(0, sc2 / nv - mc * mc)), sde = std::sqrt(std::fmax(0, se2 / nv - me * me));
      printf("%-27s %-10s | %9.3f %9.3f(%5.3f) %9.3f %4d%% | %7.3f %8.4f(%5.4f) %7.4f %4d%% | %4.0f %s\n", c.name, CODE_NAME[last.code], c.c_true, mc, sdc, rsc / nv, 100 * ccov / nv, c.esr_true, me, sde, rse / nv, 100 * ecov / nv, ncap / runs, vs);
    } else {
      printf("%-27s %-10s | %s  [%s|%s|%s|%s|%s]\n", c.name, CODE_NAME[last.code], vs, last.l1, last.l2, last.l3, last.l4, last.l5);
    }
  }
  // --- learned good parts: learn two 100 uF good parts, then judge the aged and weak 100 uF
  printf("\nLearned references (good 100 uF parts), then verdicts vs reference:\n");
  for (const char* gn : {"100u_esr0.3", "100u_esr0.3_nom100"}) {
    acq.c = find(cs, gn); acq.variant = 1; acq.rng.seed(5);
    Report rp = run_test(acq, cal, &refs, 100, LV_PRECISE, buf, w, noprog);
    if (ref_acceptable(rp)) ref_learn(refs, 100, rp.c_uF, rp.esr);
    printf("  learned %s: C=%.2f ESR=%.3f  (refs now: nominal %g C=%.2f ESR=%.3f n=%d)\n", gn, rp.c_uF, rp.esr, refs.r[0].nominal, refs.r[0].c_uF, refs.r[0].esr, refs.r[0].n);
  }
  for (const char* tn : {"100u_esr0.3_nom100", "100u_esr0.9_aged", "100u_esr3_weak", "100u_esr0.1", "100u_esr0.3_nom220"}) {
    acq.c = find(cs, tn); acq.variant = 2; acq.rng.seed(9);
    Report rp = run_test(acq, cal, &refs, 100, LV_NORMAL, buf, w, noprog);
    printf("  %-22s -> %-6s [%s|%s|%s|%s|%s]  best/worst %s/%s\n", tn, CODE_NAME[rp.code], rp.l1, rp.l2, rp.l3, rp.l4, rp.l5, CODE_NAME[rp.best], CODE_NAME[rp.worst]);
  }
  return 0;
}
