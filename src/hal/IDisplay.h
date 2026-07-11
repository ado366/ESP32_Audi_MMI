// IDisplay.h — the FIS cluster display abstraction.
// esp32 impl drives VAGFISWriter; native impl records ops for the browser canvas.
#pragma once
#include <cstdint>

namespace mmi {

// FIS font byte codes (see VAGFISWriter font notes):
//   bit0 1=positive/0=negative, bit1 1=wipe/0=XOR, bit2 1=compressed, bit5 1=centered.
// NOTE (verified on this Audi B5 cluster): the "negative" fonts render the text
// area BLANK rather than as an inverse highlight, and the "wipe" bit garbles text
// by overlaying. So we use POSITIVE, XOR fonts only; the selected menu row is
// marked with a '>' prefix instead of a colour inversion. Each op's own strip is
// cleared before it is redrawn (see Esp32Display), so XOR mode stays clean.
constexpr uint8_t kFontLeft              = 0x01; // standard, positive, left
constexpr uint8_t kFontCentered          = 0x21; // standard, positive, centered
constexpr uint8_t kFontCompressedLeft    = 0x05; // compressed, positive, left
constexpr uint8_t kFontCompressedCenter  = 0x25; // compressed, positive, centered
// Display-layer flag (NOT a FIS font bit): OR onto a base font to mark a row as
// "highlighted". The esp32 display expands it into a filled inverse bar (XOR) +
// the text drawn over it (XOR -> dark glyphs on a lit bar), and strips the flag
// before it reaches the FIS. Because the op stays a single text op, the flash-
// free XOR erase-then-draw scroll path still applies. The browser mirror draws a
// red bar + dark text for it.
constexpr uint8_t kFontHighlight         = 0x40; // e.g. kFontCompressedLeft | kFontHighlight

// Kept for reference only — do NOT use, these render blank on this cluster:
constexpr uint8_t kFontInvertedLeft      = 0x00; // standard, negative, left
constexpr uint8_t kFontCompressedInverted= 0x04; // compressed, negative, left

class IDisplay {
public:
  virtual ~IDisplay() = default;

  // Top "radio mode" area: two 8-char lines coexisting with the cluster BC.
  virtual void showTopLines(const char* line1, const char* line2) = 0;

  // Full-screen graphics mode.
  virtual void beginFullScreen(bool clear = true) = 0;
  virtual void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) = 0;
  // Like drawText but sends bytes verbatim (no charset mapping) — used by the
  // charset explorer to see what each raw ROM code renders as.
  virtual void drawTextRaw(uint8_t x, uint8_t y, uint8_t font, const char* text) { drawText(x, y, font, text); }
  // 1bpp bitmap, row-major, width/height in pixels (height a multiple of 8).
  virtual void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                          const uint8_t* data) = 0;
  virtual void clear() = 0;

  // Return control of the display to the car (exit graphics mode).
  virtual void release() = 0;
};

} // namespace mmi
