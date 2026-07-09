// FrameRecorder.h — records the display draw-ops of one frame and serializes
// them to JSON for the browser UI. Shared by the native EmulatorDisplay and the
// real Esp32Display (which records alongside driving the FIS), so the same
// browser control page works against the emulator and the actual device.
#pragma once
#include "../Config.h"
#include <string>
#include <vector>
#include <cstdint>

namespace mmi {

class FrameRecorder {
public:
  void topLines(const char* l1, const char* l2) {
    mode_ = "top"; top1_ = l1 ? l1 : ""; top2_ = l2 ? l2 : ""; ops_.clear();
  }
  void beginFull(bool clear) { mode_ = "full"; if (clear) ops_.clear(); }
  void clear() { ops_.clear(); }
  void release() { mode_ = "top"; ops_.clear(); }

  void text(uint8_t x, uint8_t y, uint8_t font, const char* s) {
    Op o; o.t = 't'; o.x = x; o.y = y; o.f = font; o.s = s ? s : ""; ops_.push_back(o);
  }
  void bitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* data) {
    Op o; o.t = 'b'; o.x = x; o.y = y; o.w = w; o.h = h;
    int bytes = (w * h + 7) / 8;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) { o.s.push_back(hex[data[i] >> 4]); o.s.push_back(hex[data[i] & 0xF]); }
    ops_.push_back(o);
  }

  std::string toJson() const {
    std::string j = "{\"mode\":\"" + mode_ + "\",\"w\":";
    j += std::to_string(cfg::FIS_WIDTH) + ",\"h\":" + std::to_string(cfg::FIS_HEIGHT);
    j += ",\"top\":[\"" + esc(top1_) + "\",\"" + esc(top2_) + "\"],\"ops\":[";
    for (size_t i = 0; i < ops_.size(); ++i) {
      const Op& o = ops_[i];
      if (i) j += ',';
      if (o.t == 't')
        j += "{\"t\":\"text\",\"x\":" + std::to_string(o.x) + ",\"y\":" + std::to_string(o.y)
           + ",\"f\":" + std::to_string(o.f) + ",\"s\":\"" + esc(o.s) + "\"}";
      else
        j += "{\"t\":\"bmp\",\"x\":" + std::to_string(o.x) + ",\"y\":" + std::to_string(o.y)
           + ",\"w\":" + std::to_string(o.w) + ",\"h\":" + std::to_string(o.h)
           + ",\"d\":\"" + o.s + "\"}";
    }
    j += "]}";
    return j;
  }

private:
  struct Op { char t; uint8_t x = 0, y = 0, f = 0, w = 0, h = 0; std::string s; };
  static std::string esc(const std::string& in) {
    std::string o; for (char c : in) { if (c == '"' || c == '\\') o.push_back('\\'); o.push_back(c); } return o;
  }
  std::string mode_ = "top", top1_, top2_;
  std::vector<Op> ops_;
};

} // namespace mmi
