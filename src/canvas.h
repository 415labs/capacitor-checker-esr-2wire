// canvas.h - portable 72x40 monochrome canvas (framebuffer in SSD1306 page layout). No hardware access: used by oled.h on the
// ESP32 and by the PC tool test/render_screens.cpp that draws every screen to a PNG.
#pragma once
#include <stdint.h>
#include <string.h>
#include "font5x7.h"

#ifndef OLED_XOFF
#define OLED_XOFF 30   // window position inside the SSD1306's 128x64 RAM (adjustable at run time: menu > SCREEN SETUP, or the web page)
#endif
#ifndef OLED_YOFF
#define OLED_YOFF 24
#endif
#define OLED_W 72
#define OLED_H 40

class Canvas {
 public:
  int xoff = OLED_XOFF, yoff = OLED_YOFF;
  uint8_t fb_[128 * 8] = {};

  void clear() { memset(fb_, 0, sizeof fb_); }
  bool get(int x, int y) const {  // logical window coordinates
    x += xoff; y += yoff;
    if (x < 0 || y < 0 || x >= 128 || y >= 64) return false;
    return (fb_[(y >> 3) * 128 + x] >> (y & 7)) & 1;
  }
  void px(int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return;
    x += xoff; y += yoff;
    if (x >= 128 || y >= 64) return;
    if (on) fb_[(y >> 3) * 128 + x] |= (1 << (y & 7));
    else fb_[(y >> 3) * 128 + x] &= ~(1 << (y & 7));
  }
  void fill(int x, int y, int w, int h, bool on) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(x + i, y + j, on); }

  // draw a string at pixel position; scale 1 = 6x8 cell (5x7 glyph), scale 2 = 12x16 cell. Returns the width drawn.
  int text(int x, int y, const char* s, bool invert = false, int scale = 1) {
    int cx0 = x;
    for (; *s; s++, cx0 += 6 * scale) {
      const uint8_t* g = glyph(*s);
      for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 8; cy++) {
          const bool on = ((g[cx] >> cy) & 1) != invert;
          if (on == invert) continue;  // only paint glyph pixels
          for (int sy = 0; sy < scale; sy++) for (int sx = 0; sx < scale; sx++) px(cx0 + cx * scale + sx, y + cy * scale + sy, !invert);
        }
    }
    return cx0 - x;
  }
  static int text_w(const char* s, int scale = 1) { return (int)strlen(s) * 6 * scale - scale; }

  // text row r = 0..4 (8 px each, 12 characters). align: 0 left, 1 centre, 2 right. invert = white bar with black text.
  void row(int r, const char* s, bool invert = false, int align = 0) {
    const int y0 = r * 8;
    fill(0, y0, OLED_W, 8, invert);
    char b[16];
    strncpy(b, s, 12);
    b[12] = 0;
    const int w = text_w(b);
    const int x = align == 1 ? (OLED_W - w) / 2 : align == 2 ? OLED_W - w : 0;
    text(x < 0 ? 0 : x, y0 + (invert ? 1 : 0), b, invert);
  }
  // large text (2x, up to 6 characters) centred on rows r and r+1 (16 px)
  void big(int r, const char* s) {
    char b[8];
    strncpy(b, s, 6);
    b[6] = 0;
    const int w = text_w(b, 2);
    fill(0, r * 8, OLED_W, 16, false);
    text((OLED_W - w) / 2, r * 8 + 1, b, false, 2);
  }
  // progress bar on text row r
  void bar(int r, int num, int den) {
    const int y0 = r * 8 + 1, w = den > 0 ? (OLED_W - 2) * (num < 0 ? 0 : num > den ? den : num) / den : 0;
    fill(0, r * 8, OLED_W, 8, false);
    for (int y = 0; y < 6; y++)
      for (int x = 0; x < OLED_W; x++) {
        const bool border = y == 0 || y == 5 || x == 0 || x == OLED_W - 1;
        px(x, y0 + y, border || (x >= 1 && x <= w && y >= 1 && y <= 4));
      }
  }
  // 1-pixel border around the whole 72x40 window (screen set-up pattern)
  void frame() {
    for (int x = 0; x < OLED_W; x++) { px(x, 0, true); px(x, OLED_H - 1, true); }
    for (int y = 0; y < OLED_H; y++) { px(0, y, true); px(OLED_W - 1, y, true); }
  }
};
