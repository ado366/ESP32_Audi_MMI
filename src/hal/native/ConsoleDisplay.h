// ConsoleDisplay.h — native IDisplay that renders a 64x88 mono framebuffer to
// the terminal as ASCII. Stand-in until the browser canvas server is added;
// it also proves the display ops the real VAGFISWriter will emit.
#pragma once
#include "../IDisplay.h"
#include "../../Config.h"
#include <cstdio>
#include <cstring>

namespace mmi {

class ConsoleDisplay : public IDisplay {
public:
  void showTopLines(const char* line1, const char* line2) override {
    std::printf("\n[FIS top] |%-8.8s|\n          |%-8.8s|\n", line1, line2);
  }
  void beginFullScreen(bool clear) override { if (clear) this->clear(); full_ = true; }
  void clear() override { std::memset(fb_, 0, sizeof(fb_)); std::memset(grid_, 0, sizeof(grid_)); }
  void drawText(uint8_t x, uint8_t y, uint8_t, const char* text) override {
    // 5x7-ish: just stamp characters at (x/6, y/8) into a text overlay grid.
    int col = x / 6, row = y / 8;
    for (int i = 0; text[i] && col + i < kCols; ++i)
      if (row >= 0 && row < kRows) grid_[row][col + i] = text[i];
    dirty_ = true;
  }
  void drawBitmap(uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) override { dirty_ = true; }
  void release() override { full_ = false; flush(); }

  // Render the current text overlay (called by the host loop when idle).
  void flush() {
    if (!dirty_) return;
    std::printf("+----------------+\n");
    for (int r = 0; r < kRows; ++r) {
      char line[kCols + 1];
      for (int c = 0; c < kCols; ++c) line[c] = grid_[r][c] ? grid_[r][c] : ' ';
      line[kCols] = 0;
      std::printf("|%-16.16s|\n", line);
    }
    std::printf("+----------------+\n");
    dirty_ = false;
  }

private:
  static constexpr int kCols = 16; // 64px / ~4px char cell for console
  static constexpr int kRows = 11; // 88px / 8
  char grid_[kRows][kCols] = {};
  uint8_t fb_[cfg::FIS_WIDTH * cfg::FIS_HEIGHT / 8] = {};
  bool full_ = false;
  bool dirty_ = false;
};

} // namespace mmi
