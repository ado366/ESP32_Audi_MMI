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

    // ---- turbocharger compressor, lower-left ----
    // A FILLED volute (snail scroll) housing with a central hub and an outlet duct
    // at the top-right — reads as a compressor, not "a wheel with blades".
    const int Rmax = 12, cx = 14;
    int cy = h - 22; if (cy < Rmax + 2) cy = Rmax + 2;
    drawCompressor(setPx, cx, cy, Rmax);
    return bmp;
  }

private:
  // Filled turbocharger compressor: a volute scroll (radius grows with angle, so it
  // spirals like a snail housing) around a hub, plus an outlet duct + flange off the
  // fat end. Modelled on flat turbo logos rather than a plain bladed wheel.
  template <typename F>
  static void drawCompressor(F setPx, int cx, int cy, int Rmax) {
    constexpr float PI = 3.14159265f, TWO = 2.f * PI;
    const float phase = -0.5f;        // orient the scroll opening / outlet up-right
    const float da    = -0.35f;       // outlet duct direction (shallow up-right)
    const int   Rmin  = (int)(Rmax * 0.44f);
    const float hole  = Rmax * 0.34f; // central wheel window
    auto wrap = [&](float a) { while (a < 0) a += TWO; while (a >= TWO) a -= TWO; return a; };
    // filled volute scroll
    for (int y = -Rmax; y <= Rmax; ++y)
      for (int x = -Rmax; x <= Rmax; ++x) {
        float r  = std::sqrt((float)(x * x + y * y));
        float th = wrap(std::atan2((float)y, (float)x) - phase);
        float Ro = Rmin + (Rmax - Rmin) * (th / TWO);      // spiral outer edge
        if (r >= hole && r <= Ro) setPx(cx + x, cy + y);
      }
    // hub boss (the shaft centre)
    for (int y = -2; y <= 2; ++y)
      for (int x = -2; x <= 2; ++x)
        if (x * x + y * y <= 2) setPx(cx + x, cy + y);
    // outlet duct: a distinct neck + a wider flange lip
    const float ax = std::cos(da), ay = std::sin(da);
    const float px = std::cos(da + PI / 2), py = std::sin(da + PI / 2);
    for (int rr = Rmax - 3; rr <= Rmax + 6; ++rr)
      for (float s = -2.f; s <= 2.f; s += 0.5f)
        setPx((int)std::lround(cx + rr * ax + s * px), (int)std::lround(cy + rr * ay + s * py));
    for (int rr = Rmax + 6; rr <= Rmax + 8; ++rr)          // flange lip (wider)
      for (float s = -3.5f; s <= 3.5f; s += 0.5f)
        setPx((int)std::lround(cx + rr * ax + s * px), (int)std::lround(cy + rr * ay + s * py));
  }
};

} // namespace mmi
