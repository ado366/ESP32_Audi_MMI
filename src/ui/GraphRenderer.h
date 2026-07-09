// GraphRenderer.h — turn a ring of samples into a 1bpp FIS bitmap (plan §4).
// Pure logic; unit-tested. Bit order matches IDisplay::drawBitmap / the emulator:
// bit index = y*w + x, MSB-first within each byte.
#pragma once
#include <cstdint>
#include <vector>
#include <cmath>

namespace mmi {

class GraphRenderer {
public:
  // Render `samples` (oldest..newest) as a line graph scaled to [min,max].
  // guide1/guide2 draw dashed horizontal reference lines when within range.
  static std::vector<uint8_t> render(const std::vector<float>& samples,
                                     float min, float max, uint8_t w, uint8_t h,
                                     float guide1 = -1e9f, float guide2 = -1e9f) {
    std::vector<uint8_t> bmp((static_cast<size_t>(w) * h + 7) / 8, 0);
    if (w == 0 || h == 0) return bmp;
    if (max <= min) max = min + 1.f;

    auto yOf = [&](float v) -> int {
      float t = (v - min) / (max - min);
      if (t < 0) t = 0; if (t > 1) t = 1;
      int y = static_cast<int>(std::lround((1.f - t) * (h - 1)));
      if (y < 0) y = 0; if (y >= h) y = h - 1;
      return y;
    };
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= w || y < 0 || y >= h) return;
      size_t bit = static_cast<size_t>(y) * w + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };

    // Guide lines (dashed).
    for (float g : {guide1, guide2}) {
      if (g <= min - 1e8f) continue;
      if (g < min || g > max) continue;
      int gy = yOf(g);
      for (int x = 0; x < w; x += 2) setPx(x, gy);
    }

    // Plot newest samples at the right edge; connect consecutive points.
    int n = static_cast<int>(samples.size());
    if (n == 0) return bmp;
    int prevY = -1;
    for (int x = 0; x < w; ++x) {
      int si = n - w + x;              // align newest to the right
      if (si < 0) { prevY = -1; continue; }
      int y = yOf(samples[si]);
      if (prevY >= 0) {                // vertical fill between adjacent points
        int a = prevY < y ? prevY : y, b = prevY < y ? y : prevY;
        for (int yy = a; yy <= b; ++yy) setPx(x, yy);
      } else {
        setPx(x, y);
      }
      prevY = y;
    }
    return bmp;
  }

  // Horizontal bar gauge (FIS-Control turbo style): outlined box filled to `frac`.
  static std::vector<uint8_t> renderBar(float frac, uint8_t w, uint8_t h) {
    std::vector<uint8_t> bmp((static_cast<size_t>(w) * h + 7) / 8, 0);
    if (w < 3 || h < 3) return bmp;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    auto setPx = [&](int x, int y) {
      if (x < 0 || x >= w || y < 0 || y >= h) return;
      size_t bit = static_cast<size_t>(y) * w + x;
      bmp[bit >> 3] |= (0x80 >> (bit & 7));
    };
    for (int x = 0; x < w; ++x) { setPx(x, 0); setPx(x, h - 1); }        // top/bottom border
    for (int y = 0; y < h; ++y) { setPx(0, y); setPx(w - 1, y); }        // left/right border
    int fillW = static_cast<int>(frac * (w - 2));                        // filled interior
    for (int x = 1; x <= fillW; ++x) for (int y = 2; y < h - 2; ++y) setPx(x, y);
    return bmp;
  }
};

} // namespace mmi
