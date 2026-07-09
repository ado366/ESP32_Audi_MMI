// Esp32Display.h — IDisplay implemented over the VAGFISWriter 3-wire bus.
// Maps the abstract display ops to the cluster's radio-text and graphics modes.
#pragma once
#include "../IDisplay.h"
#include "../../Config.h"
#include "../../ui/FrameRecorder.h"
#include <VAGFISWriter.h>
#include <Arduino.h>
#include <cstring>

namespace mmi {

class Esp32Display : public IDisplay {
public:
  Esp32Display() : fis_(cfg::PIN_FIS_CLK, cfg::PIN_FIS_DATA, cfg::PIN_FIS_ENA, 1) {}

  void begin() { fis_.begin(); fis_.reset(); }

  // The FIS character ROM is UPPERCASE-only; lowercase and non-ASCII bytes render
  // as wrong glyphs. Map to the safe set before sending (and before recording, so
  // the browser mirror matches the cluster).
  static std::string fisSafe(const char* s) {
    std::string o;
    for (const char* p = s; p && *p; ++p) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c >= 0x80 || c < 0x20) { o.push_back(' '); continue; } // non-ASCII / control
      if (c >= 'a' && c <= 'z') c -= 32;                          // uppercase
      o.push_back(static_cast<char>(c));
    }
    return o;
  }

  // Radio-text area: two 8-char lines via the 0x81 message.
  void showTopLines(const char* line1, const char* line2) override {
    std::string l1 = fisSafe(line1), l2 = fisSafe(line2);
    char buf[17];
    memset(buf, ' ', 16); buf[16] = 0;
    for (int i = 0; i < 8 && i < (int)l1.size(); ++i) buf[i] = l1[i];
    for (int i = 0; i < 8 && i < (int)l2.size(); ++i) buf[8 + i] = l2[i];
    fis_.sendMsg(buf);
    graphics_ = false;
    rec_.topLines(l1.c_str(), l2.c_str());
  }

  void beginFullScreen(bool clear) override {
    if (clear || !graphics_) fis_.initFullScreen();
    graphics_ = true;
    rec_.beginFull(clear);
  }
  void clear() override { fis_.initFullScreen(); rec_.clear(); }

  void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) override {
    std::string t = fisSafe(text);
    fis_.sendStringFS(x, y, font, String(t.c_str()));
    rec_.text(x, y, font, t.c_str());
  }
  void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* data) override {
    fis_.GraphicFromArray(x, y, w, h, data, 1);
    rec_.bitmap(x, y, w, h, data);
  }
  void release() override {
    fis_.initScreen(0, 0, 1, 1, 0x80); // leave graphics mode -> back to normal cluster
    graphics_ = false;
    rec_.release();
  }

  // Latest frame as JSON, for the browser control/debug UI.
  std::string toJson() const { return rec_.toJson(); }

private:
  VAGFISWriter fis_;
  FrameRecorder rec_;
  bool graphics_ = false;
};

} // namespace mmi
