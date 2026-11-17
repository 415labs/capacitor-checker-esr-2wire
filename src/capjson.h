// capjson.h - portable JSON builders for the web interface (host-testable, no ESP-IDF).
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include "capdsp.h"

namespace cap {

struct JB {  // tiny bounded string builder
  char* p;
  size_t cap, len = 0;
  JB(char* buf, size_t n) : p(buf), cap(n) { if (n) p[0] = 0; }
  void raw(const char* s) { for (; *s && len + 1 < cap; s++) p[len++] = *s; p[len] = 0; }
  void f(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int k = vsnprintf(p + len, cap - len, fmt, ap);
    va_end(ap);
    if (k > 0) len = (size_t)k >= cap - len ? cap - 1 : len + k;
  }
  void num(double v, const char* fmt = "%.6g") { if (std::isfinite(v)) f(fmt, v); else raw("null"); }
  void str(const char* s) {  // quoted, escaped
    raw("\"");
    for (; *s; s++) {
      if (*s == '"' || *s == '\\') { char t[3] = {'\\', *s, 0}; raw(t); }
      else if ((unsigned char)*s < 32) raw(" ");
      else { char t[2] = {*s, 0}; raw(t); }
    }
    raw("\"");
  }
};

inline void json_report(JB& j, const Report& r, double nominal_uF, int level) {
  j.raw("{\"verdict\":"); j.str(CODE_NAME[r.code]);
  j.f(",\"code\":%d,\"sure\":%s,\"best\":", (int)r.code, r.sure ? "true" : "false"); j.str(CODE_NAME[r.best]);
  j.raw(",\"worst\":"); j.str(CODE_NAME[r.worst]);
  j.raw(",\"c_uF\":"); j.num(r.c_uF);
  j.raw(",\"c_rel\":"); j.num(r.sc_rel, "%.4f");
  j.raw(",\"esr\":"); j.num(r.esr, "%.5f");
  j.raw(",\"esr_unc\":"); j.num(r.sesr, "%.5f");
  j.raw(",\"esrc\":"); j.num(r.esrc, "%.1f");
  j.raw(",\"z1k\":"); j.num(r.z1k, "%.4g");
  j.raw(",\"par_mS\":"); j.num(r.gexcess_mS, "%.3f");
  j.raw(",\"chi2\":"); j.num(r.chi2red, "%.3g");
  j.f(",\"ncap\":%d,\"npts\":%d,\"level\":%d,\"nominal\":", r.ncap, r.npts, level); j.num(nominal_uF);
  if (r.ref_nominal > 0) {
    j.raw(",\"ref\":{\"nominal\":"); j.num(r.ref_nominal);
    j.raw(",\"c_uF\":"); j.num(r.ref_c, "%.4g");
    j.raw(",\"esr\":"); j.num(r.ref_esr, "%.4f"); j.raw("}");
  } else j.raw(",\"ref\":null");
  j.raw(",\"why\":"); j.str(r.why);
  j.raw(",\"lines\":["); j.str(r.l1); j.raw(","); j.str(r.l2); j.raw(","); j.str(r.l3); j.raw(","); j.str(r.l4); j.raw(","); j.str(r.l5); j.raw("]");
  j.raw(",\"pts\":[");
  for (int i = 0; i < r.npts; i++) {
    if (i) j.raw(",");
    j.raw("{\"f\":"); j.num(r.pf[i], "%.0f"); j.raw(",\"re\":"); j.num(r.pre[i], "%.4g"); j.raw(",\"im\":"); j.num(r.pim[i], "%.4g"); j.raw("}");
  }
  j.raw("]}");
}

inline void json_cal(JB& j, const Cal& c) {
  j.f("{\"open\":%s,\"short\":%s,\"res\":%s,\"cap0\":%s,\"cap1\":%s", (c.flags & F_OPEN) ? "true" : "false", (c.flags & F_SHORT) ? "true" : "false",
      (c.flags & F_RES) ? "true" : "false", (c.flags & F_CAP0) ? "true" : "false", (c.flags & F_CAP1) ? "true" : "false");
  j.raw(",\"rtrim\":"); j.num(c.rtrim, "%.4f");
  j.raw(",\"cref0\":"); j.num(c.cref[0]); j.raw(",\"ctrim0\":"); j.num(c.ctrim[0], "%.4f");
  j.raw(",\"cref1\":"); j.num(c.cref[1]); j.raw(",\"ctrim1\":"); j.num(c.ctrim[1], "%.4f");
  j.raw(",\"lead_r\":"); j.num(c.z0[PROBE_IDX].real(), "%.3f");
  j.raw(",\"lead_x\":"); j.num(c.z0[PROBE_IDX].imag(), "%.3f");
  j.raw("}");
}

inline void json_refs(JB& j, const RefTable& t) {
  j.raw("[");
  for (int i = 0; i < t.n; i++) {
    if (i) j.raw(",");
    j.f("{\"i\":%d,\"n\":%d,\"nominal\":", i, t.r[i].n); j.num(t.r[i].nominal);
    j.raw(",\"c_uF\":"); j.num(t.r[i].c_uF, "%.4g"); j.raw(",\"esr\":"); j.num(t.r[i].esr, "%.4f"); j.raw("}");
  }
  j.raw("]");
}

}  // namespace cap
