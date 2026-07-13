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
  static std::vector<uint8_t> render(float frac, uint8_t w, uint8_t h, int bars = 16, int spin = 0) {
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

    // ---- turbocharger compressor, upper-left (top 1px below the readout) ----
    // Bigger bladed wheel inside a THIN volute housing (hugging it, no gap) with a
    // HORIZONTAL outlet duct + flange. `spin` rotates the blades (0..2) for animation.
    const int Rw = 11, cx = 16, cy = Rw + 12;   // cy so the flange top lands at bitmap y9
    drawCompressor(setPx, cx, cy, Rw, spin);
    return bmp;
  }

private:
  template <typename F>
  static void drawCompressor(F setPx, int cx, int cy, int Rw, int spin) {
    constexpr float PI = 3.14159265f, TWO = 2.f * PI;
    const int hub = 3, blades = 8;
    const float step = TWO / blades, curve = 0.85f;
    const float phase = -1.15f;        // fat lobe near the top, where the duct joins
    auto wrap = [&](float a) { while (a < 0) a += TWO; while (a >= TWO) a -= TWO; return a; };

    // --- thin volute housing HUGGING the wheel (inner = Rw, no gap) ---
    for (int y = -Rw - 6; y <= Rw + 6; ++y)
      for (int x = -Rw - 6; x <= Rw + 6; ++x) {
        float r  = std::sqrt((float)(x * x + y * y));
        float th = wrap(std::atan2((float)y, (float)x) - phase);
        float Ro = (Rw + 1.f) + 3.5f * (th / TWO);         // spiral shell (thin tongue -> fatter outlet)
        if (r >= Rw && r <= Ro) setPx(cx + x, cy + y);
      }
    // --- the wheel: 2px ring + 8 curved blades (rotated by `spin`) + hub ---
    for (int a = 0; a < 360; ++a) {
      float r = a * PI / 180.f;
      setPx(cx + (int)std::lround(Rw * std::cos(r)),       cy + (int)std::lround(Rw * std::sin(r)));
      setPx(cx + (int)std::lround((Rw - 1) * std::cos(r)), cy + (int)std::lround((Rw - 1) * std::sin(r)));
    }
    float roff = spin * (step / 3.f);
    for (int b = 0; b < blades; ++b) {
      float base = b * step + roff;
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

    // --- HORIZONTAL outlet duct off the fat top, ending in a vertical flange lip ---
    const int dyt = cy - Rw - 2, dyb = cy - Rw + 3;        // wider (6px) pipe
    const int dxs = cx - 2, dxe = cx + Rw + 7;
    for (int yy = dyt; yy <= dyb; ++yy)
      for (int xx = dxs; xx <= dxe; ++xx) setPx(xx, yy);   // pipe
    for (int yy = dyt - 1; yy <= dyb + 1; ++yy)            // flange lip: only 1px wider top/bottom
      for (int xx = dxe; xx <= dxe + 2; ++xx) setPx(xx, yy);
  }
};

} // namespace mmi
