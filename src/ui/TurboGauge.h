// TurboGauge.h — the turbo gauge graphic (Boost view), composed into ONE 64-wide
// 1bpp canvas and drawn as 64x8 horizontal BANDS.
//
// WHY 64-wide bands: VAGFISWriter::GraphicFromArray only uses 32-byte "jumbo"
// packets (4 rows per packet) when the bitmap is exactly 64 wide — the FIS
// graphics packet fills horizontally and wraps at the INIT'D SCREEN width, so
// full-width rows are the only safe multi-row payload. Any narrower bitmap falls
// back to ONE packet per pixel row (huge per-packet handshake overhead + 2ms
// inter-packet delay). The old per-cell layout (40x34 icon + 32x21 wheel + eight
// 8-wide cells) cost ~350 row-packets = seconds per paint and 250ms+ blocking
// per tall-cell redraw. As 7 bands of 64x8 the whole gauge is 14 jumbo packets,
// and a boost change redraws only the 1-2 bands the bar tops crossed.
//
// Layout (screen coords): gauge occupies y32..87. Compressor symbol on the left
// (static, spin 0 — animation dropped: the FIS bus is too slow), histogram bars
// across the full width; bars under the compressor (x<=39) clip to the bottom
// 21 rows, bars to its right (x>=40) rise to the gauge top.
// Bit order = y*64 + x, MSB-first (row-major, matches jumbo packets exactly).
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace mmi {

class TurboGauge {
public:
  static constexpr int kBars  = 16;
  static constexpr int kBands = 7;    // 7 bands x 8 rows = y32..87
  static constexpr int kBandH = 8;
  static constexpr int kTop   = 32;   // screen Y of band 0
  static constexpr int kW     = 64;
  static constexpr int kH     = kBands * kBandH;  // 56
  static int bandY(int j) { return kTop + j * kBandH; }

  // The full gauge (compressor + bars at `frac` fill) as one 64x56 row-major
  // bitmap (448 bytes). Band j is the 64 bytes at offset j*64 — the caller
  // slices with data() + j*64, no per-band recompose.
  static std::vector<uint8_t> compose(float frac) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    std::vector<uint8_t> bmp(static_cast<size_t>(kW) * kH / 8, 0);
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= kW || y < 0 || y >= kH) return;
      size_t bit = static_cast<size_t>(y) * kW + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };
    // Compressor: wheel centre at local (16,20) == screen (16,52), same place as
    // the old split-sprite layout. Static spin (no animation).
    drawCompressor(setPx, 16, 20, 11, 0);

    // Histogram: 16 bars, 3px wide on a 4px pitch. Filled solid up to the lit
    // level, hollow outline beyond it (rising power-curve heights).
    int lit = static_cast<int>(frac * kBars + 0.5f);
    for (int i = 0; i < kBars; ++i) {
      int x0 = i * 4 + 1, x1 = x0 + 2;             // +1 so the last bar reaches the edge
      int clipTop = (x1 <= 39) ? (kH - 21) : 0;    // under the compressor: bottom 21 rows only
      int y0 = kH - barH(i); if (y0 < clipTop) y0 = clipTop;
      if (i < lit) {
        for (int x = x0; x <= x1; ++x) for (int y = y0; y < kH; ++y) setPx(x, y);
      } else {                                     // hollow outline
        for (int x = x0; x <= x1; ++x) { setPx(x, y0); setPx(x, kH - 1); }
        for (int y = y0; y < kH; ++y) { setPx(x0, y); setPx(x1, y); }
      }
    }
    return bmp;
  }

private:
  // Height of bar i on the 56px-tall gauge (power curve: short -> tall).
  static int barH(int i) {
    float t = static_cast<float>(i + 1) / kBars;
    int h = 1 + static_cast<int>((kH - 1) * std::pow(t, 2.4f) + 0.5f);
    if (h < 6)  h = 6;
    if (h > kH) h = kH;
    return h;
  }

  template <typename F>
  static void drawCompressor(F setPx, int cx, int cy, int Rw, int spin) {
    constexpr float PI = 3.14159265f, TWO = 2.f * PI;
    const int hub = 3, blades = 8;
    const float step = TWO / blades, curve = 0.85f;
    const float phase = -1.15f;        // orient the spiral
    const float grow  = 5.f;           // spiral growth (thin tongue -> fat outlet)
    auto wrap = [&](float a) { while (a < 0) a += TWO; while (a >= TWO) a -= TWO; return a; };
    auto Rof  = [&](float th) { return (Rw + 1.f) + grow * (wrap(th - phase) / TWO); };

    // --- thin volute housing HUGGING the wheel (inner = Rw, no gap) ---
    for (int y = -Rw - 7; y <= Rw + 7; ++y)
      for (int x = -Rw - 7; x <= Rw + 7; ++x) {
        float r  = std::sqrt((float)(x * x + y * y));
        if (r >= Rw && r <= Rof(std::atan2((float)y, (float)x))) setPx(cx + x, cy + y);
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

    // --- HORIZONTAL outlet duct whose TOP aligns with the scroll's topmost row, so
    //     only the flange lip extends above the housing (not the whole pipe) ---
    int scrollTop = cy - Rw;
    for (float th = -PI - 0.4f; th <= 0.4f; th += 0.02f) {   // scan the upper outer edge
      int yy = (int)std::lround(cy + Rof(th) * std::sin(th));
      if (yy < scrollTop) scrollTop = yy;
    }
    const int dxs = cx - 2, dxe = cx + Rw + 5;            // shorter pipe
    const int pipeTop = scrollTop + 1;                   // sit a touch lower than the fat-lobe top
    // tapering pipe: widens DOWNWARD toward the flange (continues the volute's growth)
    for (int xx = dxs; xx <= dxe; ++xx) {
      float t = (float)(xx - dxs) / (float)(dxe - dxs);
      int bot = pipeTop + 3 + (int)std::lround(3.f * t); // 4px at the scroll -> 7px at the flange
      for (int yy = pipeTop; yy <= bot; ++yy) setPx(xx, yy);
    }
    // vertical flange lip: 1px taller than the pipe's flange end, top and bottom
    const int fbot = pipeTop + 6;
    for (int yy = pipeTop - 1; yy <= fbot + 1; ++yy)
      for (int xx = dxe; xx <= dxe + 2; ++xx) setPx(xx, yy);
  }
};

} // namespace mmi
