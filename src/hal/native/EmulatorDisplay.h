// EmulatorDisplay.h — native IDisplay backed by the shared FrameRecorder.
#pragma once
#include "../IDisplay.h"
#include "../../ui/FrameRecorder.h"

namespace mmi {

class EmulatorDisplay : public IDisplay {
public:
  void showTopLines(const char* l1, const char* l2) override { rec_.topLines(l1, l2); }
  void beginFullScreen(bool clear, uint8_t graphicsTop) override { rec_.beginFull(clear, graphicsTop); }
  void clear() override { rec_.clear(); }
  void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) override { rec_.text(x, y, font, text); }
  void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* data) override { rec_.bitmap(x, y, w, h, data); }
  void release() override { rec_.release(); }

  std::string toJson() const { return rec_.toJson(); }

private:
  FrameRecorder rec_;
};

} // namespace mmi
