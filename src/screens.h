// screens.h - every screen of the 72x40 OLED user interface as a pure drawing function (portable: also rendered to PNG on the PC by
// test/render_screens.cpp). 5 text rows of 12 characters; big text = 2x (6 characters).
// Controls (one button): TAP = main action, HOLD (about 0.7 s) = menu / back / accept, HOLD 10 s = screen set-up.
#pragma once
#include <stdio.h>
#include <string.h>
#include "canvas.h"
#include "capdsp.h"

namespace ui {

inline void fmt_nominal(char* o, size_t n, double uF) {  // o must hold >= 24 bytes
  if (uF <= 0) snprintf(o, n, "AUTO");
  else if (uF >= 1000) snprintf(o, n, "%gmF", uF / 1000);
  else snprintf(o, n, "%guF", uF);
}
// capacitance for the big font: at most 6 characters
inline void fmt_c_big(char* o, size_t n, double uF) {
  if (!(uF < 1e9)) snprintf(o, n, "---");
  else if (uF >= 10000) snprintf(o, n, "%.1fmF", uF / 1000);
  else if (uF >= 1000) snprintf(o, n, "%.2fmF", uF / 1000);
  else if (uF >= 100) snprintf(o, n, "%.0fuF", uF);
  else if (uF >= 10) snprintf(o, n, "%.1fuF", uF);
  else snprintf(o, n, "%.2fuF", uF);
}

inline void splash(Canvas& c) {
  c.clear();
  c.row(0, "CAP CHECKER", true, 1);
  c.row(1, "IN-CIRCUIT", false, 1);
  c.row(2, "ESR + C", false, 1);
  c.row(3, "TESTER", false, 1);
  c.row(4, "STARTING...", false, 1);
}

// idle screen. alt toggles the bottom hint between the button help and the web address.
inline void home(Canvas& c, const char* nominal, bool cal_ok, bool alt, const char* ip) {
  c.clear();
  if (cal_ok) {
    c.row(0, "READY", true, 1);
    c.big(1, nominal);
    c.row(3, "TAP: TEST", false, 1);
    c.row(4, alt ? ip : "HOLD: MENU", false, 1);
  } else {
    c.row(0, "NEED CAL!", true, 1);
    c.row(1, "FIRST SETUP:", false, 1);
    c.row(2, "CLIPS APART,", false, 1);
    c.row(3, "THEN TAP", false, 1);
    c.row(4, "HOLD: MENU", false, 1);
  }
}

inline void menu(Canvas& c, const char* const* items, int n, int sel) {
  c.clear();
  c.row(0, "MENU", true, 1);
  int top = sel - 1;
  if (top > n - 4) top = n - 4;
  if (top < 0) top = 0;
  for (int i = 0; i < 4 && top + i < n; i++) {
    char b[16];
    snprintf(b, sizeof b, "%c%s", top + i == sel ? '>' : ' ', items[top + i]);
    c.row(1 + i, b, false, 0);
  }
  // scroll hints
  if (top > 0) c.text(OLED_W - 6, 8, "^");
  if (top + 4 < n) c.text(OLED_W - 6, 32, "v");
}

inline void pick_nominal(Canvas& c, const char* nominal) {
  c.clear();
  c.row(0, "NOMINAL", true, 1);
  c.big(1, nominal);
  c.row(3, "TAP: NEXT", false, 1);
  c.row(4, "HOLD: OK", false, 1);
}

// Wi-Fi access instructions. page 0..2
inline void wifi(Canvas& c, int page, bool on, const char* ssid, const char* pass, const char* ip) {
  c.clear();
  char h[32];
  if (!on) {
    c.row(0, "WIFI OFF", true, 1);
    c.row(1, "WI-FI IS OFF", false, 1);
    c.row(2, "UNPLUG AND", false, 1);
    c.row(3, "REPLUG TO", false, 1);
    c.row(4, "TURN IT ON", false, 1);
    return;
  }
  snprintf(h, sizeof h, "WIFI %d/3 >", (page % 3) + 1);
  c.row(0, h, true, 1);
  if (page == 0) {
    c.row(1, "JOIN WIFI:", false, 1);
    c.row(2, ssid, false, 1);
    c.row(3, "PASSWORD:", false, 1);
    c.row(4, pass, false, 1);
  } else if (page == 1) {
    c.row(1, "THEN OPEN", false, 1);
    c.row(2, "IN A BROWSER", false, 1);
    c.row(3, ip, false, 1);
    c.row(4, "NO INTERNET", false, 1);
  } else {
    c.row(1, "PAGE WONT", false, 1);
    c.row(2, "LOAD? TURN", false, 1);
    c.row(3, "MOBILE DATA", false, 1);
    c.row(4, "OFF", false, 1);
  }
}

// waiting for the probes: good = consecutive stable probes so far
inline void wait_probes(Canvas& c, const char* nominal, int good, int need, int tick) {
  c.clear();
  char h[40];
  snprintf(h, sizeof h, "TEST %.7s", nominal);
  c.row(0, h, true, 1);
  c.row(1, "CLIP PROBES", false, 1);
  c.row(2, "ON THE CAP", false, 1);
  if (good > 0) {
    c.row(3, "HOLD STILL...", false, 1);
    c.bar(4, good, need);
  } else {
    c.row(3, (tick & 1) ? "WAITING.  " : "WAITING . ", false, 1);
    c.row(4, "TAP: CANCEL", false, 1);
  }
}

inline void countdown(Canvas& c, const char* title, const char* line, int secs) {
  c.clear();
  c.row(0, title, true, 1);
  c.row(1, line, false, 1);
  char b[16];
  snprintf(b, sizeof b, "%d", secs > 99 ? 99 : secs < 0 ? 0 : secs);
  c.big(2, b);
  c.row(4, "TAP: CANCEL", false, 1);
}

inline void measuring(Canvas& c, const char* title, const char* l1, const char* l2, int i, int n) {
  c.clear();
  c.row(0, title, true, 1);
  c.row(1, l1, false, 1);
  c.row(2, l2, false, 1);
  c.bar(4, i, n);
}

inline void message(Canvas& c, const char* title, const char* a, const char* b = "", const char* d = "", const char* e = "") {
  c.clear();
  c.row(0, title, true, 1);
  c.row(1, a, false, 1);
  c.row(2, b, false, 1);
  c.row(3, d, false, 1);
  c.row(4, e, false, 1);
}

// test result, page 0 = answer, page 1 = details (only for measured parts)
inline bool result_has_details(const cap::Report& r) { return r.code <= cap::V_BAD || r.code == cap::V_PARALLEL; }
inline void result(Canvas& c, const cap::Report& r, double nominal_uF, int page) {
  c.clear();
  if (!result_has_details(r)) {  // OPEN, SHORT, ERROR: the report's own lines
    c.row(0, r.l1, true, 1);
    c.row(1, r.l2, false, 1);
    c.row(2, r.l3, false, 1);
    c.row(3, r.l4, false, 1);
    c.row(4, "TAP: AGAIN", false, 1);
    return;
  }
  if (page == 0) {
    c.row(0, r.l1, true, 1);
    char b[16];
    if (r.code == cap::V_PARALLEL) {
      c.row(1, "R IN PARALLEL", false, 1);
      c.big(2, r.l4);
      c.row(4, r.l5, false, 1);
      return;
    }
    fmt_c_big(b, sizeof b, r.c_uF);
    c.big(1, b);
    c.row(3, r.l3, false, 1);  // "ESR 0.31R"
    c.row(4, r.l4, false, 1);  // "+-.04R +-2%"
  } else {
    c.row(0, "DETAILS", true, 1);
    c.row(1, r.l5, false, 1);
    char b[32], n[24];
    fmt_nominal(n, sizeof n, nominal_uF);
    snprintf(b, sizeof b, "NOM %s", n);
    c.row(2, b, false, 1);
    snprintf(b, sizeof b, "ESR*C %.0f", r.esrc);
    c.row(3, b, false, 1);
    snprintf(b, sizeof b, "READS %d", r.ncap);
    c.row(4, b, false, 1);
  }
}

inline void screen_setup(Canvas& c, int stage, int x, int y, bool flip) {
  c.clear();
  c.frame();
  char b[16];
  c.row(1, "SCREEN FIT", false, 1);
  if (stage == 0) snprintf(b, sizeof b, "Y OFFSET %d", y);
  else if (stage == 1) snprintf(b, sizeof b, "X OFFSET %d", x);
  else snprintf(b, sizeof b, "FLIP: %s", flip ? "YES" : "NO");
  c.row(2, b, false, 1);
  c.row(3, stage == 2 ? "TAP: TOGGLE" : "TAP: +1", false, 1);
  c.row(4, stage == 2 ? "HOLD: SAVE" : "HOLD: NEXT", false, 1);
  c.frame();
}

}  // namespace ui
