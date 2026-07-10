// SpeedoRenderer.h — renders a number as big 7-segment digits into a 1bpp bitmap
// (row-major, MSB = leftmost pixel), matching IDisplay::drawBitmap. Used by the
// speedometer view.
#pragma once
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mmi {

struct SpeedoRenderer {
  // Lit-segment mask per digit: bit0=a(top) 1=b(top-right) 2=c(bot-right)
  // 3=d(bottom) 4=e(bot-left) 5=f(top-left) 6=g(middle).
  static uint8_t segMask(char d) {
    switch (d) {
      case '0': return 0x3F; case '1': return 0x06; case '2': return 0x5B; case '3': return 0x4F;
      case '4': return 0x66; case '5': return 0x6D; case '6': return 0x7D; case '7': return 0x07;
      case '8': return 0x7F; case '9': return 0x6F; default: return 0x00;
    }
  }
  static void px(std::vector<uint8_t>& b, int w, int h, int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    int bit = y * w + x; b[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
  }
  static void rect(std::vector<uint8_t>& b, int w, int h, int x, int y, int rw, int rh) {
    for (int j = 0; j < rh; ++j) for (int i = 0; i < rw; ++i) px(b, w, h, x + i, y + j);
  }
  static void digit(std::vector<uint8_t>& b, int w, int h, char d, int cx, int cy, int dw, int dh, int t) {
    uint8_t m = segMask(d);
    int midY = cy + (dh - t) / 2, botY = cy + dh - t, half = (dh - t) / 2;
    if (m & 0x01) rect(b, w, h, cx + t, cy,   dw - 2 * t, t);          // a
    if (m & 0x08) rect(b, w, h, cx + t, botY, dw - 2 * t, t);          // d
    if (m & 0x40) rect(b, w, h, cx + t, midY, dw - 2 * t, t);          // g
    if (m & 0x20) rect(b, w, h, cx,          cy,   t, half + t);       // f
    if (m & 0x02) rect(b, w, h, cx + dw - t, cy,   t, half + t);       // b
    if (m & 0x10) rect(b, w, h, cx,          midY, t, half + t);       // e
    if (m & 0x04) rect(b, w, h, cx + dw - t, midY, t, half + t);       // c
  }
  // Render `value` as big 7-seg digits, right-aligned within a fixed 3-digit field
  // (so the ones digit stays put and the number doesn't shift as digits change).
  static std::vector<uint8_t> render(int value, int w, int h) {
    std::vector<uint8_t> b((w * h + 7) / 8, 0);
    char s[8]; snprintf(s, sizeof(s), "%d", value < 0 ? 0 : (value > 999 ? 999 : value));
    int n = (int)strlen(s);
    const int field = 3, dw = 18, gap = 4, t = 3;
    int totalW = field * dw + (field - 1) * gap;      // width of a 3-digit field
    int x0 = (w - totalW) / 2; if (x0 < 0) x0 = 0;
    int startCell = field - n;                        // right-align the actual digits
    for (int i = 0; i < n; ++i) digit(b, w, h, s[i], x0 + (startCell + i) * (dw + gap), 0, dw, h, t);
    return b;
  }
};

} // namespace mmi
