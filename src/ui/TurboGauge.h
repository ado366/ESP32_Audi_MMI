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
    // The bladed compressor WHEEL (2px housing ring + curved blades + hub) with a
    // volute SCROLL wrapping around it (clear gap, thin tongue -> fat outlet) and an
    // outlet duct + flange at the top-right.
    const int Rw = 10, cx = 16;
    int cy = h - 22; if (cy < Rw + 4) cy = Rw + 4;
    drawCompressor(setPx, cx, cy, Rw);
    return bmp;
  }

private:
  template <typename F>
  static void drawCompressor(F setPx, int cx, int cy, int Rw) {
    constexpr float PI = 3.14159265f, TWO = 2.f * PI;
    const int hub = 3, blades = 6;
    const float step = TWO / blades, curve = 0.95f;
    const float Aend = -0.6f;          // scroll fat end / duct direction: top-right

    // --- the wheel (the shape that reads well): 2px ring + 6 curved blades + hub ---
    for (int a = 0; a < 360; ++a) {
      float r = a * PI / 180.f;
      setPx(cx + (int)std::lround(Rw * std::cos(r)),       cy + (int)std::lround(Rw * std::sin(r)));
      setPx(cx + (int)std::lround((Rw - 1) * std::cos(r)), cy + (int)std::lround((Rw - 1) * std::sin(r)));
    }
    for (int b = 0; b < blades; ++b) {
      float base = b * step;
      for (int rr = hub + 1; rr <= Rw - 2; ++rr) {
        float f = (float)(rr - hub - 1) / (Rw - 2 - hub - 1);
        float ang = base + curve * f;
        for (float da = 0.f; da <= 0.16f; da += 0.16f)
          setPx(cx + (int)std::lround(rr * std::cos(ang + da)), cy + (int)std::lround(rr * std::sin(ang + da)));
      }
    }
    for (int y = -hub; y <= hub; ++y)
      for (int x = -hub; x <= hub; ++x)
        if (x * x + y * y <= hub * hub) setPx(cx + x, cy + y);

    // --- volute scroll wrapping the wheel: ~270 deg spiral OUTSIDE it (clear gap),
    //     thin tongue growing to the fat outlet where the duct joins ---
    const float sweep = 4.7f;
    for (float t = 0.f; t <= 1.f; t += 0.006f) {
      float ang = Aend - sweep * (1.f - t);
      float rad = (Rw + 3.0f) + 2.5f * t;
      int thick = t < 0.5f ? 2 : 3;
      for (int w = 0; w < thick; ++w)
        setPx(cx + (int)std::lround((rad + w) * std::cos(ang)), cy + (int)std::lround((rad + w) * std::sin(ang)));
    }
    // --- outlet duct + flange lip at the scroll's fat end (top-right) ---
    const float ax = std::cos(Aend), ay = std::sin(Aend);
    const float px = std::cos(Aend + PI / 2), py = std::sin(Aend + PI / 2);
    const int b0 = Rw + 8;
    for (int rr = b0 - 2; rr <= b0 + 5; ++rr)
      for (float s = -2.5f; s <= 2.5f; s += 0.5f)
        setPx((int)std::lround(cx + rr * ax + s * px), (int)std::lround(cy + rr * ay + s * py));
    for (int rr = b0 + 5; rr <= b0 + 7; ++rr)              // flange lip (wider)
      for (float s = -4.f; s <= 4.f; s += 0.5f)
        setPx((int)std::lround(cx + rr * ax + s * px), (int)std::lround(cy + rr * ay + s * py));
  }
};

} // namespace mmi
