// TurboGauge.h — the turbo gauge graphic (Boost view), rect-op edition.
//
// Layering (cheapest possible FIS traffic, HW-verified no-claim 0x53):
//   1. STATIC layer: compressor (housing+duct+flange+spin-0 wheel) and the hollow
//      OUTLINES of all 10 bars, composed into one 64x56 canvas drawn as 7 bands
//      of 64x8 (jumbo packets). Content never changes -> sent once per page.
//   2. WHEEL sprite (32x21 at screen (0,43)): redrawn on spin steps. Narrow, so
//      Esp32Display sends it via a workspace-relative multi-row path (~5 pkts).
//   3. BARS: each bar is ONE fillRect op on its INTERIOR (outline untouched),
//      fixed slot, only the lit flag toggles -> a bar toggle is a single 7-byte
//      0x53 fill/clear + workspace reset (~2 packets, paints atomically).
//
// Layout (screen coords): gauge occupies y32..87. Bars 5px wide on a 6px pitch
// spanning x2..60; bars under the compressor (x<=39) clip to the bottom 21 rows.
// Bit order = y*64 + x, MSB-first (row-major, matches jumbo packets exactly).
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace mmi {

class TurboGauge {
public:
  static constexpr int kBars  = 10;   // FIS-Control/Maxi-K bar count
  static constexpr int kBands = 7;    // 7 bands x 8 rows = y32..87
  static constexpr int kBandH = 8;
  static constexpr int kTop   = 32;   // screen Y of band 0
  static constexpr int kW     = 64;
  static constexpr int kH     = kBands * kBandH;  // 56
  static int bandY(int j) { return kTop + j * kBandH; }

  // The STATIC layer (compressor + all bar outlines) as one 64x56 row-major
  // bitmap (448 bytes). Band j is the 64 bytes at offset j*64. Never changes.
  static std::vector<uint8_t> composeStatic() {
    std::vector<uint8_t> bmp(static_cast<size_t>(kW) * kH / 8, 0);
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= kW || y < 0 || y >= kH) return;
      size_t bit = static_cast<size_t>(y) * kW + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };
    // Compressor: wheel centre at local (16,20) == screen (16,52). The wheel
    // drawn here (spin 0) is always covered by the live wheel sprite.
    drawCompressor(setPx, 16, 20, 11, 0);
    // Hollow outline for every bar (the fill rects only touch the interiors).
    for (int i = 0; i < kBars; ++i) {
      int x0, y0, w, h; barBox(i, x0, y0, w, h);
      y0 -= kTop;                                  // to local canvas coords
      for (int x = x0; x < x0 + w; ++x) { setPx(x, y0); setPx(x, y0 + h - 1); }
      for (int y = y0; y < y0 + h; ++y) { setPx(x0, y); setPx(x0 + w - 1, y); }
    }
    return bmp;
  }

  // Just the spinning wheel (blades + local housing ring), drawn at screen
  // (0,43). Narrow (32x21) so a spin frame rides the workspace bitmap path.
  static std::vector<uint8_t> wheelSprite(int spin) {
    const int W = 32, H = 21;
    std::vector<uint8_t> bmp((W * H + 7) / 8, 0);
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= W || y < 0 || y >= H) return;
      size_t bit = static_cast<size_t>(y) * W + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };
    drawCompressor(setPx, 16, 9, 11, spin);        // wheel centre local (16,9) == screen (16,52)
    return bmp;
  }
  static constexpr int kWheelX = 0, kWheelY = 43, kWheelW = 32, kWheelH = 21;

  // Full bounding box of bar i in SCREEN coordinates (outline included).
  static void barBox(int i, int& x, int& y, int& w, int& h) {
    x = i * 6 + 2; w = 5;                          // bars span x2..60
    int bh = barH(i);
    int x1 = x + w - 1;
    int clipTop = (x1 <= 39) ? (kH - 21) : 0;      // under the compressor: bottom 21 rows
    int top = kH - bh; if (top < clipTop) top = clipTop;
    y = kTop + top; h = kH - top;
  }
  // INTERIOR of bar i in SCREEN coordinates — the region the fill rect toggles.
  // Fixed slot per bar: only the lit flag changes frame-to-frame.
  static void barInterior(int i, int& x, int& y, int& w, int& h) {
    barBox(i, x, y, w, h);
    x += 1; y += 1; w -= 2; h -= 2;
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
