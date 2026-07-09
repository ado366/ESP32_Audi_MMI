// IDisplay.h — the FIS cluster display abstraction.
// esp32 impl drives VAGFISWriter; native impl records ops for the browser canvas.
#pragma once
#include <cstdint>

namespace mmi {

// FIS font byte codes (see VAGFISWriter font notes):
//   bit0 1=positive/0=negative(inverted), bit2 1=compressed, bit3 special, bit5 1=centered.
constexpr uint8_t kFontLeft            = 0x01; // standard, positive, left
constexpr uint8_t kFontCentered        = 0x21; // standard, positive, centered
constexpr uint8_t kFontInvertedLeft    = 0x00; // standard, negative, left (highlight)
constexpr uint8_t kFontCompressedLeft    = 0x05; // compressed, positive, left
constexpr uint8_t kFontCompressedCenter  = 0x25; // compressed, positive, centered
constexpr uint8_t kFontCompressedInverted= 0x04; // compressed, negative, left (highlight)

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
