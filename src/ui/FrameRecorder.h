// FrameRecorder.h — records the display draw-ops of one frame. Used for the
// browser mirror (toJson) and, on the ESP32, for frame diffing so only changed
// regions are written to the slow FIS bus (no full-screen flashing).
#pragma once
#include "../Config.h"
#include <string>
#include <vector>
#include <cstdint>

namespace mmi {

struct FrameOp { char t; uint8_t x = 0, y = 0, f = 0, w = 0, h = 0; std::string s;
  bool sameSlot(const FrameOp& o) const { return t == o.t && x == o.x && y == o.y && w == o.w && h == o.h; }
};

class FrameRecorder {
public:
  void topLines(const char* l1, const char* l2) {
    mode_ = "top"; top1_ = l1 ? l1 : ""; top2_ = l2 ? l2 : ""; ops_.clear();
  }
  void beginFull(bool clear, uint8_t graphicsTop = 0) { mode_ = "full"; gtop_ = graphicsTop; htop_.clear(); if (clear) ops_.clear(); }
  uint8_t gtop() const { return gtop_; }   // graphics region starts here (0 = whole screen)
  void setHalfTop(const char* s) { htop_ = s ? s : ""; }   // radio/now-playing text for the top band
  const std::string& htop() const { return htop_; }
  void clear() { ops_.clear(); }
  void release() { mode_ = "top"; top1_.clear(); top2_.clear(); ops_.clear(); }

  void text(uint8_t x, uint8_t y, uint8_t font, const char* s, uint8_t clearW = 0) {
    FrameOp o; o.t = 't'; o.x = x; o.y = y; o.f = font; o.w = clearW; o.s = s ? s : ""; ops_.push_back(o);
  }
  void bitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* data) {
    FrameOp o; o.t = 'b'; o.x = x; o.y = y; o.w = w; o.h = h;
    int bytes = (w * h + 7) / 8;
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < bytes; ++i) { o.s.push_back(hex[data[i] >> 4]); o.s.push_back(hex[data[i] & 0xF]); }
    ops_.push_back(o);
  }
  // Solid rectangle, filled lit or cleared dark. On the FIS this is a single
  // 7-byte no-claim 0x53 command — vastly cheaper than bitmap data (f = lit).
  void rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool lit) {
    FrameOp o; o.t = 'r'; o.x = x; o.y = y; o.w = w; o.h = h; o.f = lit ? 1 : 0;
    ops_.push_back(o);
  }

  const std::string& mode() const { return mode_; }
  const std::string& top1() const { return top1_; }
  const std::string& top2() const { return top2_; }
  const std::vector<FrameOp>& ops() const { return ops_; }

  std::string toJson() const {
    std::string j = "{\"mode\":\"" + mode_ + "\",\"w\":";
    j += std::to_string(cfg::FIS_WIDTH) + ",\"h\":" + std::to_string(cfg::FIS_HEIGHT);
    j += ",\"top\":[\"" + esc(top1_) + "\",\"" + esc(top2_) + "\"],\"ops\":[";
    for (size_t i = 0; i < ops_.size(); ++i) {
      const FrameOp& o = ops_[i];
      if (i) j += ',';
      if (o.t == 't')
        j += "{\"t\":\"text\",\"x\":" + std::to_string(o.x) + ",\"y\":" + std::to_string(o.y)
           + ",\"f\":" + std::to_string(o.f) + ",\"s\":\"" + esc(o.s) + "\"}";
      else if (o.t == 'r')
        j += "{\"t\":\"rect\",\"x\":" + std::to_string(o.x) + ",\"y\":" + std::to_string(o.y)
           + ",\"w\":" + std::to_string(o.w) + ",\"h\":" + std::to_string(o.h)
           + ",\"f\":" + std::to_string(o.f) + "}";
      else
        j += "{\"t\":\"bmp\",\"x\":" + std::to_string(o.x) + ",\"y\":" + std::to_string(o.y)
           + ",\"w\":" + std::to_string(o.w) + ",\"h\":" + std::to_string(o.h)
           + ",\"d\":\"" + o.s + "\"}";
    }
    j += "]}";
    return j;
  }

private:
  static std::string esc(const std::string& in) {
    static const char* kHex = "0123456789abcdef";
    std::string o;
    for (unsigned char c : in) {
      if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back((char)c); }
      else if (c < 0x20 || c >= 0x80) { o += "\\u00"; o.push_back(kHex[c >> 4]); o.push_back(kHex[c & 0xF]); }  // FIS accent/control bytes
      else o.push_back((char)c);
    }
    return o;
  }
  std::string mode_ = "top", top1_, top2_;
  uint8_t gtop_ = 0;
  std::string htop_;
  std::vector<FrameOp> ops_;
};

} // namespace mmi
