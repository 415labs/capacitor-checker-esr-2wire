// Emits sample JSON (report with and without reference, calibration, references) so it can be validated with python json.loads
#include "../src/capjson.h"
using namespace cap;
int main() {
  static char buf[4096];
  Report r;
  r.code = V_WEAK; r.best = V_GOOD; r.worst = V_BAD; r.sure = false; r.c_uF = 99.5; r.sc_rel = 0.02; r.esr = 0.31; r.sesr = 0.03; r.esrc = 26;
  r.npts = 3; r.ncap = 17; r.ref_nominal = 100; r.ref_c = 99.1; r.ref_esr = 0.3;
  for (int i = 0; i < 3; i++) { r.pf[i] = 100.0 * (i + 1); r.pre[i] = 0.3; r.pim[i] = -1.6 / (i + 1); }
  std::snprintf(r.why, sizeof r.why, "quote \" back \\ slash, ESR*C=26 %%");
  std::snprintf(r.l1, sizeof r.l1, "WEAK?");
  JB a(buf, sizeof buf); json_report(a, r, 100, 1); puts(buf);
  r.c_uF = INFINITY; r.ref_nominal = 0; JB a2(buf, sizeof buf); json_report(a2, r, 0, 1); puts(buf);
  Cal c; c.flags = F_OPEN | F_SHORT | F_RES | F_CAP0; c.z0[PROBE_IDX] = cx(0.12, 0.01);
  JB b(buf, sizeof buf); json_cal(b, c); puts(buf);
  RefTable t; ref_learn(t, 100, 99.5, 0.3); ref_learn(t, 470, 470, 0.1);
  JB d(buf, sizeof buf); json_refs(d, t); puts(buf);
  JB e(buf, 40); e.raw("{\"truncated\":"); e.str("0123456789012345678901234567890"); puts(buf);
}
