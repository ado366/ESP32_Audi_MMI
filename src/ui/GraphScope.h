// GraphScope.h — rolling "oscilloscope" graph buffer (FIS-Control style).
//
// Instead of redrawing the whole plot every sample (a 64x64 bitmap = ~512 bytes
// that saturated the FIS bus and froze the loop), a write cursor sweeps
// left->right: each sample draws its connector at the cursor and clears a small
// GAP of columns just ahead, leaving a visible "cut" between the newest data and
// the older data still on screen from the previous sweep. It wraps at the right
// edge.
//
// The plot lives in a persistent 1bpp buffer emitted in 8px-wide vertical STRIPS.
// As the cursor moves it only disturbs the strip(s) it is under, so the frame
// differ re-sends just those strips — a few bytes per sample instead of the whole
// plot — while the trace itself stays at full pixel resolution.
#pragma once
#include <cstdint>
#include <cstring>

namespace mmi {

class GraphScope {
public:
  static constexpr int kW = 64;            // plot width (px)
  static constexpr int kH = 64;            // plot height (px)
  static constexpr int kBytesPerRow = kW / 8;
  static constexpr int kStrips = kW / 8;   // 8 strips, 8px each

  void reset() { std::memset(buf_, 0, sizeof(buf_)); cursor_ = 0; prevY_ = -1; }

  int  cursor() const { return cursor_; }
  bool atSweepStart() const { return cursor_ == 0; }

  // Push one sample. y (and the optional second-trace y2) are plot rows, with
  // 0 = top .. kH-1 = bottom. `step` = columns advanced per sample, `gap` =
  // columns cleared ahead of the cursor (the visible cut).
  void push(int y, int step, int gap, bool hasY2 = false, int y2 = 0) {
    if (step < 1) step = 1;
    clampY(y); if (hasY2) clampY(y2);
    for (int k = 0; k < step; ++k) {
      int x = (cursor_ + k) % kW;
      clearCol(x);                                   // erase this column's old sweep
      int a, b;
      if (prevY_ < 0) { a = b = y; }                 // first sample: a single point
      else { a = prevY_ < y ? prevY_ : y; b = prevY_ < y ? y : prevY_; }
      for (int yy = a; yy <= b; ++yy) setPx(x, yy);  // connector fill prev..cur
      if (hasY2 && (x & 1) == 0) setPx(x, y2);        // dotted second trace
    }
    for (int k = 0; k < gap; ++k) clearCol((cursor_ + step + k) % kW);   // the cut
    prevY_ = y;
    cursor_ = (cursor_ + step) % kW;
  }

  // Strip s (columns s*8 .. s*8+7) as an 8xkH bitmap: one byte per row, MSB =
  // leftmost column, matching IDisplay::drawBitmap (bit index = y*w + x).
  void strip(int s, uint8_t out[kH]) const { for (int y = 0; y < kH; ++y) out[y] = buf_[y][s]; }

  // Test/inspection helper: is pixel (x,y) lit?
  bool pixel(int x, int y) const {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return false;
    return (buf_[y][x >> 3] & (uint8_t)(0x80 >> (x & 7))) != 0;
  }

private:
  void clampY(int& y) const { if (y < 0) y = 0; if (y >= kH) y = kH - 1; }
  void setPx(int x, int y) { buf_[y][x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }
  void clearCol(int x) {
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    for (int y = 0; y < kH; ++y) buf_[y][x >> 3] &= (uint8_t)~m;
  }

  uint8_t buf_[kH][kBytesPerRow] = {{0}};
  int cursor_ = 0;
  int prevY_ = -1;
};

} // namespace mmi
