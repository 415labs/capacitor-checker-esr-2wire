// Renders every OLED screen to text bitmaps ('#'/'.') for test/render_screens.py (PNG preview).
#include <stdio.h>
#include "../src/screens.h"
using namespace ui;
static void dump(const char* name, const Canvas& c) {
  printf("@%s\n", name);
  for (int y = 0; y < OLED_H; y++) { for (int x = 0; x < OLED_W; x++) putchar(c.get(x, y) ? '#' : '.'); putchar('\n'); }
}
int main() {
  Canvas c;
  c.xoff = 0; c.yoff = 0;  // logical window only
  splash(c); dump("1 splash", c);
  home(c, "100uF", true, false, "192.168.4.1"); dump("2 home", c);
  home(c, "0.47uF", true, true, "192.168.4.1"); dump("3 home (alt)", c);
  home(c, "AUTO", false, false, "192.168.4.1"); dump("4 home needs cal", c);
  static const char* items[] = {"NOM 100uF", "ACC NORMAL", "GO ON CAP", "WIFI INFO", "CALIBRATE", "SCREEN FIT", "BACK"};
  menu(c, items, 7, 0); dump("5 menu", c);
  menu(c, items, 7, 4); dump("6 menu (scrolled)", c);
  pick_nominal(c, "4700uF"); dump("7 pick nominal", c);
  wifi(c, 0, true, "CapChecker", "capcheck1", "192.168.4.1"); dump("8 wifi 1", c);
  wifi(c, 1, true, "CapChecker", "capcheck1", "192.168.4.1"); dump("9 wifi 2", c);
  wifi(c, 2, true, "CapChecker", "capcheck1", "192.168.4.1"); dump("10 wifi 3", c);
  wait_probes(c, "100uF", 0, 3, 0); dump("11 wait for probes", c);
  wait_probes(c, "100uF", 2, 3, 0); dump("12 probes stable", c);
  countdown(c, "CAL 1/2", "CLIPS APART", 4); dump("13 countdown", c);
  measuring(c, "MEASURING", "KEEP PROBES", "ON THE CAP", 14, 30); dump("14 measuring", c);
  cap::Report r; r.code = cap::V_GOOD; r.sure = true; r.c_uF = 99.84; r.sc_rel = 0.02; r.esr = 0.31; r.sesr = 0.04; r.esrc = 26; r.ncap = 17;
  snprintf(r.l1, sizeof r.l1, "GOOD"); snprintf(r.l3, sizeof r.l3, "ESR 0.31R"); snprintf(r.l4, sizeof r.l4, "+-.04R +-2%%"); snprintf(r.l5, sizeof r.l5, "VS REF 100");
  result(c, r, 100, 0); dump("15 result good", c);
  result(c, r, 100, 1); dump("16 result details", c);
  r.code = cap::V_BAD; r.c_uF = 47.0; r.esr = 15; snprintf(r.l1, sizeof r.l1, "BAD?"); snprintf(r.l3, sizeof r.l3, "ESR 15.00R"); snprintf(r.l4, sizeof r.l4, "+-.16R +-3%%");
  result(c, r, 47, 0); dump("17 result bad", c);
  cap::Report o; o.code = cap::V_OPEN; snprintf(o.l1, sizeof o.l1, "OPEN"); snprintf(o.l2, sizeof o.l2, "C < 0.1uF"); snprintf(o.l3, sizeof o.l3, "or no cap");
  result(c, o, 100, 0); dump("18 result open", c);
  screen_setup(c, 0, 30, 24, false); dump("19 screen fit", c);
  message(c, "CANCELLED", "TEST STOPPED", "", "", "TAP: HOME"); dump("20 message", c);
}
