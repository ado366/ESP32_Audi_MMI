// TurboGauge.h — the full-screen turbo gauge graphic (Boost view).
// One 1bpp bitmap composites two things so they can't fight over pixels:
//   * a rising histogram that spans the FULL width, lit left->right to `frac`;
//     the tallest (rightmost) bar reaches the very top of the bitmap.
//   * a turbocharger compressor-wheel symbol in the TOP-LEFT (curved impeller
//     blades in a round housing) — reads as a turbo, not a snail. Its top sits
//     at the top of the bitmap, i.e. level with the tallest bar.
// The short left-hand bars stay below the wheel, so they don't collide. Bit
// order matches IDisplay::drawBitmap and the real FIS: bit = y*w + x, MSB-first.
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace mmi {

class TurboGauge {
public:
  static std::vector<uint8_t> render(float frac, uint8_t w, uint8_t h, int bars = 16) {
    std::vector<uint8_t> bmp((static_cast<size_t>(w) * h + 7) / 8, 0);
    if (w < 8 || h < 8 || bars < 1) return bmp;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= w || y < 0 || y >= h) return;
      size_t bit = static_cast<size_t>(y) * w + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };

    // ---- rising histogram, full width, lit up to `frac` ----
    // Uniform cell width (integer i*w/bars gave alternating 4/5px bars); pick `bars`
    // that divides the width evenly (e.g. 16 into 64) for identical 3px bars + 1px gap.
    int cellW = static_cast<int>(w) / bars; if (cellW < 2) cellW = 2;
    int barW  = cellW - 1;                                 // 1px gap between bars
    int lit = static_cast<int>(frac * bars + 0.5f);
    for (int i = 0; i < bars; ++i) {
      int x0 = i * cellW + 1;               // +1 so the LAST bar reaches the right edge while
      int x1 = x0 + barW - 1;               // every bar keeps the same width (1px margin moves left)
      // Power curve: heights accelerate toward the right (the last few bars grow
      // fastest), so the gauge reads exponentially, not linearly. Floor at 6px so
      // the shortest (hollow) bars still show ~4px of empty fillable space inside.
      float t = static_cast<float>(i + 1) / bars;          // 0..1 left..right
      int barH = 1 + static_cast<int>((h - 1) * std::pow(t, 2.4f) + 0.5f);
      if (barH < 6) barH = 6;
      if (barH > h) barH = h;
      int y0 = h - barH;                                   // bar top
      if (i < lit) {
        for (int x = x0; x <= x1; ++x) for (int y = y0; y < h; ++y) setPx(x, y);
      } else {                                             // hollow outline
        for (int x = x0; x <= x1; ++x) { setPx(x, y0); setPx(x, h - 1); }
        for (int y = y0; y < h; ++y) { setPx(x0, y); setPx(x1, y); }
      }
    }

    // ---- turbocharger compressor wheel, lower-left ----
    // cy = h-41 keeps the wheel at a fixed screen position (bottom ~= same row) as
    // the gauge grows taller, so growing the bars doesn't drag the wheel around.
    const int R = 14, hub = 3, cx = 15;
    int cy = h - 41; if (cy < R + 1) cy = R + 1;
    drawWheel(setPx, cx, cy, R, hub);
    return bmp;
  }

private:
  template <typename F>
  static void drawWheel(F setPx, int cx, int cy, int R, int hub) {
    // Round housing (two-pixel ring so it stands out against the bars).
    for (int a = 0; a < 360; ++a) {
      float rad = a * 3.14159265f / 180.f;
      setPx(cx + (int)std::lround(R * std::cos(rad)),       cy + (int)std::lround(R * std::sin(rad)));
      setPx(cx + (int)std::lround((R - 1) * std::cos(rad)), cy + (int)std::lround((R - 1) * std::sin(rad)));
    }
    // Filled centre hub.
    for (int y = -hub; y <= hub; ++y)
      for (int x = -hub; x <= hub; ++x)
        if (x * x + y * y <= hub * hub) setPx(cx + x, cy + y);
    // Curved impeller blades sweeping out from the hub to the rim — the swirl is
    // what makes it read as a compressor wheel rather than a plain disc/snail.
    const int blades = 8;
    const float curve = 1.15f;                              // radians of sweep, hub->rim
    for (int b = 0; b < blades; ++b) {
      float base = b * (2.f * 3.14159265f / blades);
      for (int rr = hub; rr <= R - 2; ++rr) {
        float f = static_cast<float>(rr - hub) / (R - 2 - hub);  // 0 at hub, 1 at rim
        float ang = base + curve * f;
        setPx(cx + (int)std::lround(rr * std::cos(ang)),
              cy + (int)std::lround(rr * std::sin(ang)));
      }
    }
  }
};

} // namespace mmi
