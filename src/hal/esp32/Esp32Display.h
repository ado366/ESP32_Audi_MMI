// Esp32Display.h — IDisplay over VAGFISWriter, with frame diffing and a
// NON-BLOCKING, millis-paced write queue.
//
// Draw calls only RECORD a frame. flush() diffs it against the last frame and
// enqueues the FIS commands for just the changed regions. service(now) sends at
// most one queued command per kGapMs (the slow FIS bus needs a gap between
// writes) — no delay(), so the main loop keeps running.
#pragma once
#include "../IDisplay.h"
#include "../../Config.h"
#include "../../ui/FrameRecorder.h"
#include <VAGFISWriter.h>
#include <Arduino.h>
#include <cstring>
#include <string>
#include <deque>

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

  // Diff the current frame and enqueue commands for the changed regions.
  void flush() {
    if (rec_.mode() == "top") {
      if (haveSent_ && sent_.mode() == "top" && sent_.top1() == rec_.top1() && sent_.top2() == rec_.top2()) return;
      q_.clear();
      if (graphics_) q_.push_back({Cmd::ExitGfx, {}, ""});
      // Build the 16-char radio message. The FIS auto-centres lines under 8 chars;
      // for full/scrolling lines (>=8) replace spaces with 0x1C so they stay left.
      std::string buf(16, ' ');
      for (int i = 0; i < 8 && i < (int)rec_.top1().size(); ++i) buf[i]     = rec_.top1()[i];
      for (int i = 0; i < 8 && i < (int)rec_.top2().size(); ++i) buf[8 + i] = rec_.top2()[i];
      if ((int)rec_.top1().size() >= 8) for (int i = 0; i < 8;  ++i) if (buf[i] == ' ') buf[i] = 0x1C;
      if ((int)rec_.top2().size() >= 8) for (int i = 8; i < 16; ++i) if (buf[i] == ' ') buf[i] = 0x1C;
      q_.push_back({Cmd::TopLine, {}, buf});
      commit();
      return;
    }

    // full-screen graphics mode
    const auto& n = rec_.ops();
    const auto& o = sent_.ops();
    bool structural = !haveSent_ || sent_.mode() != "full" || !graphics_ || n.size() != o.size();
    if (!structural)
      for (size_t i = 0; i < n.size(); ++i) if (!n[i].sameSlot(o[i])) { structural = true; break; }

    if (structural) {
      q_.clear();
      q_.push_back({Cmd::Init, {}, ""});
      for (const auto& op : n) q_.push_back({Cmd::Draw, op, ""});
    } else {
      for (size_t i = 0; i < n.size(); ++i) {
        bool changed = (n[i].t == 't') ? (n[i].s != o[i].s || n[i].f != o[i].f) : (n[i].s != o[i].s);
        if (!changed) continue;
        FrameOp op = n[i];
        // pad text to the old length so the wiping font clears longer previous text
        if (op.t == 't') while (op.s.size() < o[i].s.size()) op.s.push_back(' ');
        q_.push_back({Cmd::Draw, op, ""});
      }
    }
    commit();
  }

  // Send at most one queued command, paced by millis(). Call every loop.
  void service(uint32_t now) {
    if (q_.empty()) return;
    if (haveWritten_ && (uint32_t)(now - lastWrite_) < kGapMs) return;
    const Cmd& c = q_.front();
    switch (c.kind) {
      case Cmd::Init:     fis_.initFullScreen(); graphics_ = true; break;
      case Cmd::ExitGfx:  fis_.initScreen(0, 0, 1, 1, 0x80); graphics_ = false; break;
      case Cmd::TopLine:  { char b[17]; memcpy(b, c.buf.data(), 16); b[16] = 0; fis_.sendMsg(b); } break;
      case Cmd::Draw:     drawOp(c.op); break;
    }
    q_.pop_front();
    lastWrite_ = now; haveWritten_ = true;
  }

private:
  struct Cmd { enum Kind { Init, ExitGfx, TopLine, Draw } kind; FrameOp op; std::string buf; };

  void commit() { sent_ = rec_; haveSent_ = true; }

  void drawOp(const FrameOp& op) {
    if (op.t == 't') {
      fis_.sendStringFS(op.x, op.y, op.f, String(op.s.c_str()));
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

  static constexpr uint32_t kGapMs = 5;   // min gap between FIS writes

  VAGFISWriter fis_;
  FrameRecorder rec_, sent_;
  std::deque<Cmd> q_;
  uint32_t lastWrite_ = 0;
  bool graphics_ = false, haveSent_ = false, haveWritten_ = false;
};

} // namespace mmi
