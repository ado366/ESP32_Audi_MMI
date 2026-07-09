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

  // Radio-text area: two 8-char lines via the 0x81 message.
  void showTopLines(const char* line1, const char* line2) override {
    char buf[17];
    memset(buf, ' ', 16); buf[16] = 0;
    if (line1) for (int i = 0; i < 8 && line1[i]; ++i) buf[i] = line1[i];
    if (line2) for (int i = 0; i < 8 && line2[i]; ++i) buf[8 + i] = line2[i];
    fis_.sendMsg(buf);
    graphics_ = false;
    rec_.topLines(line1, line2);
  }

  void beginFullScreen(bool clear) override {
    if (clear || !graphics_) fis_.initFullScreen();
    graphics_ = true;
    rec_.beginFull(clear);
  }
  void clear() override { fis_.initFullScreen(); rec_.clear(); }

  void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) override {
    fis_.sendStringFS(x, y, font, String(text ? text : ""));
    rec_.text(x, y, font, text);
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
