// Esp32Display.h — IDisplay over VAGFISWriter, with frame diffing.
// Draw calls only RECORD a frame; flush() compares it to the last frame sent and
// writes ONLY the changed regions to the slow FIS bus. This removes the whole-
// screen re-init/redraw every frame (flashing) and exits graphics mode cleanly
// when returning to the radio-text lines (so closing the menu updates at once).
#pragma once
#include "../IDisplay.h"
#include "../../Config.h"
#include "../../ui/FrameRecorder.h"
#include <VAGFISWriter.h>
#include <Arduino.h>
#include <cstring>
#include <string>

namespace mmi {

class Esp32Display : public IDisplay {
public:
  Esp32Display() : fis_(cfg::PIN_FIS_CLK, cfg::PIN_FIS_DATA, cfg::PIN_FIS_ENA, 1) {}

  void begin() { fis_.begin(); fis_.reset(); }

  // The FIS ROM is UPPERCASE-only; map to the safe set (drop non-ASCII/control).
  static std::string fisSafe(const char* s) {
    std::string o;
    for (const char* p = s; p && *p; ++p) {
      unsigned char c = static_cast<unsigned char>(*p);
      if (c >= 0x80 || c < 0x20) { o.push_back(' '); continue; }
      if (c >= 'a' && c <= 'z') c -= 32;
      o.push_back(static_cast<char>(c));
    }
    return o;
  }

  // ---- IDisplay: record only ----
  void showTopLines(const char* l1, const char* l2) override { rec_.topLines(fisSafe(l1).c_str(), fisSafe(l2).c_str()); }
  void beginFullScreen(bool clear) override { rec_.beginFull(clear); }
  void clear() override { rec_.clear(); }
  void drawText(uint8_t x, uint8_t y, uint8_t font, const char* text) override { rec_.text(x, y, font, fisSafe(text).c_str()); }
  void drawBitmap(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const uint8_t* data) override { rec_.bitmap(x, y, w, h, data); }
  void release() override { rec_.release(); }

  std::string toJson() const { return rec_.toJson(); }

  // Send the minimal set of changes for the current frame to the FIS.
  void flush() {
    if (rec_.mode() == "top") {
      if (!haveSent_ || sent_.mode() != "top" || sent_.top1() != rec_.top1() || sent_.top2() != rec_.top2()) {
        if (graphics_) { fis_.initScreen(0, 0, 1, 1, 0x80); graphics_ = false; } // leave graphics -> radio text
        char buf[17]; memset(buf, ' ', 16); buf[16] = 0;
        for (int i = 0; i < 8 && i < (int)rec_.top1().size(); ++i) buf[i] = rec_.top1()[i];
        for (int i = 0; i < 8 && i < (int)rec_.top2().size(); ++i) buf[8 + i] = rec_.top2()[i];
        // The FIS centres lines shorter than 8 chars. For full/scrolling lines
        // (>=8 chars) set the last char to 0x1C (blank, non-space) so the line is
        // treated as full-width and stays LEFT-aligned — no jump while scrolling.
        if ((int)rec_.top1().size() >= 8 && buf[7]  == ' ') buf[7]  = 0x1C;
        if ((int)rec_.top2().size() >= 8 && buf[15] == ' ') buf[15] = 0x1C;
        fis_.sendMsg(buf);
        commit();
      }
      return;
    }

    // full-screen graphics mode
    const auto& n = rec_.ops();
    const auto& o = sent_.ops();
    bool structural = !haveSent_ || sent_.mode() != "full" || !graphics_ || n.size() != o.size();
    if (!structural)
      for (size_t i = 0; i < n.size(); ++i) if (!n[i].sameSlot(o[i])) { structural = true; break; }

    if (structural) {
      fis_.initFullScreen(); graphics_ = true;
      for (const auto& op : n) drawOp(op, "");
    } else {
      for (size_t i = 0; i < n.size(); ++i) {
        if (n[i].t == 't') { if (n[i].s != o[i].s || n[i].f != o[i].f) drawOp(n[i], o[i].s); }
        else               { if (n[i].s != o[i].s) drawOp(n[i], ""); }
      }
    }
    commit();
  }

private:
  void commit() { sent_ = rec_; haveSent_ = true; }

  // Draw one op; for text, pad to `oldText` length so the (wiping) font clears
  // any longer previous text at that slot.
  void drawOp(const FrameOp& op, const std::string& oldText) {
    if (op.t == 't') {
      std::string t = op.s;
      while (t.size() < oldText.size()) t.push_back(' ');
      fis_.sendStringFS(op.x, op.y, op.f, String(t.c_str()));
    } else {
      uint8_t buf[1024];
      int bytes = (op.w * op.h + 7) / 8;
      if (bytes > (int)sizeof(buf)) bytes = sizeof(buf);
      for (int i = 0; i < bytes; ++i)
        buf[i] = (uint8_t)((hexv(op.s[i * 2]) << 4) | hexv(op.s[i * 2 + 1]));
      fis_.GraphicFromArray(op.x, op.y, op.w, op.h, buf, 1);
    }
  }
  static uint8_t hexv(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0; }

  VAGFISWriter fis_;
  FrameRecorder rec_, sent_;
  bool graphics_ = false;
  bool haveSent_ = false;
};

} // namespace mmi
