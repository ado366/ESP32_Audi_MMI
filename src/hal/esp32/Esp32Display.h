// Esp32Display.h — IDisplay over VAGFISWriter, using the refresh model proven by
// tomaskovacik's VAGFISPages on this exact cluster:
//
//   * A full-screen page is (re)drawn with initFullScreen() + all ops on ANY
//     change. Sub-region initScreen() clears are NOT used — on this cluster they
//     drop it out of graphics mode (the menu "reverts to radio" on scroll).
//   * The cluster auto-reverts to standard/radio mode after a short silence, so
//     while idle we send the 0xC3 keepalive every ~900ms (VAGFISPages value).
//     A keepalive is NOT a re-init: a re-init blanks the screen on this hardware.
//
// Everything is NON-BLOCKING: flush() only decides what to enqueue, service(now)
// sends at most one FIS command per kGapMs, and the keepalive is paced by millis.
//
// Fast-scroll handling: we never interrupt an in-flight redraw. flush() enqueues
// a redraw only when the queue is empty, then converges to whatever the latest
// recorded frame is — so a burst of scroll steps collapses to at most one redraw
// per completed draw cycle instead of clearing the screen over and over.
#pragma once
#include "../IDisplay.h"
#include "../../Config.h"
#include "../../ui/FrameRecorder.h"
#include <VAGFISWriter.h>
#include <Arduino.h>
#include <cstring>
#include <string>
#include <vector>
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

  // Decide what (if anything) to enqueue for the current recorded frame.
  void flush() {
    if (rec_.mode() == "top") {
      // Build the 16-char radio message. The FIS auto-centres lines under 8 chars;
      // for full/scrolling lines (>=8) replace spaces with 0x1C so they stay left.
      std::string buf(16, ' ');
      for (int i = 0; i < 8 && i < (int)rec_.top1().size(); ++i) buf[i]     = rec_.top1()[i];
      for (int i = 0; i < 8 && i < (int)rec_.top2().size(); ++i) buf[8 + i] = rec_.top2()[i];
      if ((int)rec_.top1().size() >= 8) for (int i = 0; i < 8;  ++i) if (buf[i] == ' ') buf[i] = 0x1C;
      if ((int)rec_.top2().size() >= 8) for (int i = 8; i < 16; ++i) if (buf[i] == ' ') buf[i] = 0x1C;

      if (haveTop_ && buf == topBuf_ && !fullValid_) return; // unchanged; service() re-sends as keepalive
      if (!q_.empty()) return;                               // let a pending redraw/exit drain first
      if (graphics_) q_.push_back({Cmd::ExitGfx, {}, ""});   // leave graphics mode before writing radio text
      q_.push_back({Cmd::TopLine, {}, buf});
      topBuf_ = buf; haveTop_ = true; fullValid_ = false;
      return;
    }

    // ---- full-screen graphics ----
    haveTop_ = false;
    if (!q_.empty()) return;                    // never interrupt an in-flight redraw
    const auto& ops = rec_.ops();
    if (fullValid_ && opsEqual(ops, drawn_)) return; // nothing changed; service() keepalives
    // Bound redraw frequency so a fast scroll doesn't clear+repaint every few ms.
    uint32_t now = millis();
    if (fullValid_ && (uint32_t)(now - lastRedraw_) < kRedrawMinMs) return; // retry next loop
    q_.push_back({Cmd::Init, {}, ""});
    for (const auto& op : ops) q_.push_back({Cmd::Draw, op, ""});
    drawn_ = ops; fullValid_ = true; lastRedraw_ = now;
  }

  // Send at most one queued command, paced by millis(). Call every loop.
  void service(uint32_t now) {
    if (q_.empty()) {
      // Keep the cluster from reverting to standard mode. In graphics mode the
      // 0xC3 keepalive holds the page; in radio mode we re-assert the top text.
      if ((uint32_t)(now - lastWrite_) < kKeepAliveMs) return;
      if (graphics_)      fis_.sendKeepAliveMsgNB();
      else if (haveTop_)  { char b[17]; memcpy(b, topBuf_.data(), 16); b[16] = 0; fis_.sendMsg(b); }
      else return;
      lastWrite_ = now; haveWritten_ = true;
      return;
    }
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

  static bool opsEqual(const std::vector<FrameOp>& a, const std::vector<FrameOp>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      const FrameOp& x = a[i]; const FrameOp& y = b[i];
      if (!x.sameSlot(y) || x.f != y.f || x.s != y.s) return false;
    }
    return true;
  }

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

  static constexpr uint32_t kGapMs       = 5;    // min gap between FIS writes (VAGFISPages DELAY)
  static constexpr uint32_t kKeepAliveMs = 900;  // idle keepalive cadence (VAGFISPages value)
  static constexpr uint32_t kRedrawMinMs = 90;   // cap full-redraw rate during fast scroll

  VAGFISWriter fis_;
  FrameRecorder rec_;
  std::vector<FrameOp> drawn_;      // ops currently committed to the screen
  std::string topBuf_;              // last 16-char radio message
  std::deque<Cmd> q_;
  uint32_t lastWrite_ = 0, lastRedraw_ = 0;
  bool graphics_ = false;           // bus currently in graphics mode
  bool fullValid_ = false;          // drawn_ describes the live full-screen page
  bool haveTop_ = false, haveWritten_ = false;
};

} // namespace mmi
