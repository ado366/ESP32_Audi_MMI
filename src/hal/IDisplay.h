// IDisplay.h — the FIS cluster display abstraction.
// esp32 impl drives VAGFISWriter; native impl records ops for the browser canvas.
#pragma once
#include <cstdint>

namespace mmi {

// FIS font byte codes (see VAGFISWriter font notes):
//   bit0 1=positive/0=negative(inverted), bit1 1=wipe area (overwrite, not XOR),
//   bit2 1=compressed, bit3 special, bit5 1=centered.
// bit1 is set so redrawn text overwrites its area cleanly (needed for diff redraws).
constexpr uint8_t kFontLeft              = 0x03; // standard, positive, wipe, left
constexpr uint8_t kFontCentered          = 0x23; // standard, positive, wipe, centered
constexpr uint8_t kFontInvertedLeft      = 0x02; // standard, negative, wipe, left (highlight)
constexpr uint8_t kFontCompressedLeft    = 0x07; // compressed, positive, wipe, left
constexpr uint8_t kFontCompressedCenter  = 0x27; // compressed, positive, wipe, centered
constexpr uint8_t kFontCompressedInverted= 0x06; // compressed, negative, wipe, left (highlight)

class IDisplay {
public:
  virtual ~IDisplay() = default;

  // Top "radio mode" area: two 8-char lines coexisting with the cluster BC.
  virtual void showTopLines(const char* line1, const char* line2) = 0;

  // Full-screen graphics mode.
  virtual void beginFullScreen(bool clear = true) = 0;
  virtual void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) = 0;
  // 1bpp bitmap, row-major, width/height in pixels (height a multiple of 8).
  virtual void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                          const uint8_t* data) = 0;
  virtual void clear() = 0;

  // Return control of the display to the car (exit graphics mode).
  virtual void release() = 0;
};

} // namespace mmi
