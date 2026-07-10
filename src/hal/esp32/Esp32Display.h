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
  uint32_t writeFails() const { return writeFails_; }   // count of dropped FIS writes (diagnostics)

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
    uint32_t now = millis();
    if (fullValid_ && opsEqual(ops, drawn_)) {
      // Nothing changed. If we did XOR partial updates while scrolling, do ONE
      // authoritative full redraw once things go quiet, to correct any desync
      // (a dropped bar/text write) that would otherwise persist. Flashes once,
      // only after scrolling stops — not periodically.
      if (needHeal_ && (uint32_t)(now - lastRedraw_) >= kSettleMs) {
        q_.push_back({Cmd::Init, {}, ""});
        for (const auto& op : ops) q_.push_back({Cmd::Draw, op, ""});
        needHeal_ = false; lastRedraw_ = now;
      }
      return;
    }
    // Bound redraw frequency so a fast scroll doesn't repaint every few ms.
    if (fullValid_ && (uint32_t)(now - lastRedraw_) < kRedrawMinMs) return;

    // Same layout, only text/font differs — the menu/list scroll case. Update just
    // the changed rows with no full-screen clear (flash-free): text is drawn XOR,
    // so re-drawing the OLD row erases it, then draw the NEW row. (Sub-region init
    // can't be used to clear a row — it drops this cluster out of graphics mode.)
    if (sameStructureText(ops)) {
      for (size_t i = 0; i < ops.size(); ++i)
        if (ops[i].f != drawn_[i].f || ops[i].s != drawn_[i].s) {
          q_.push_back({Cmd::Draw, drawn_[i], ""});   // XOR-erase old row
          q_.push_back({Cmd::Draw, ops[i], ""});      // XOR-draw new row
        }
      needHeal_ = true;                               // schedule an authoritative heal
    } else {
      q_.push_back({Cmd::Init, {}, ""});
      for (const auto& op : ops) q_.push_back({Cmd::Draw, op, ""});
      needHeal_ = false;
    }
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
    Cmd c = q_.front();
    bool pop = true;
    switch (c.kind) {
      // A FIS write can be dropped if the cluster's ENA handshake times out. Text
      // draws are XOR, so a failed line must NOT be re-drawn in place (that would
      // erase a row the cluster actually painted). Instead, on ANY failure restart
      // the whole page: initFullScreen clears everything and all rows redraw onto
      // blank — self-correcting, no erase, no partial state.
      case Cmd::Init:     if (!fis_.initFullScreen()) { restartRedraw(); pop = false; } else graphics_ = true; break;
      case Cmd::ExitGfx:  fis_.initScreen(0, 0, 1, 1, 0x80); graphics_ = false; break;
      case Cmd::TopLine:  { char b[17]; memcpy(b, c.buf.data(), 16); b[16] = 0; fis_.sendMsg(b); } break;
      case Cmd::Draw:     if (!drawOp(c.op)) { restartRedraw(); pop = false; } break;
    }
    if (pop) { q_.pop_front(); if (q_.empty()) redrawFails_ = 0; } // page drained cleanly
    // Measure the gap from the END of the (blocking) write: the cluster needs the
    // full kGapMs to render each row before the next arrives, or it drops rows
    // even though the write ACKs. Setting lastWrite_ before the write made the
    // real gap ~2ms shorter and caused random rows to drop each redraw.
    lastWrite_ = millis(); haveWritten_ = true;
  }

private:
  struct Cmd { enum Kind { Init, ExitGfx, TopLine, Draw } kind; FrameOp op; std::string buf; };

  // Same layout as the drawn frame and every changed op is a text row — so each
  // changed row can be updated in place by XOR-erasing the old and drawing the new.
  bool sameStructureText(const std::vector<FrameOp>& ops) const {
    if (!fullValid_ || ops.size() != drawn_.size()) return false;
    for (size_t i = 0; i < ops.size(); ++i) {
      if (!ops[i].sameSlot(drawn_[i])) return false;                    // layout moved
      if (ops[i].f == drawn_[i].f && ops[i].s == drawn_[i].s) continue; // unchanged
      if (ops[i].t != 't') return false;                               // bitmap change -> full redraw
    }
    return true;
  }

  // A write in the current full-screen page failed; requeue a clean redraw of the
  // whole page (bounded, so a persistently unresponsive cluster can't thrash).
  void restartRedraw() {
    ++writeFails_;
    if (!fullValid_ || drawn_.empty() || ++redrawFails_ > kMaxRestarts) { redrawFails_ = 0; return; }
    q_.clear();
    q_.push_back({Cmd::Init, {}, ""});
    for (const auto& op : drawn_) q_.push_back({Cmd::Draw, op, ""});
  }

  static bool opsEqual(const std::vector<FrameOp>& a, const std::vector<FrameOp>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      const FrameOp& x = a[i]; const FrameOp& y = b[i];
      if (!x.sameSlot(y) || x.f != y.f || x.s != y.s) return false;
    }
    return true;
  }

  // Returns false if the write was dropped so service() can restart the page.
  bool drawOp(const FrameOp& op) {
    if (op.t == 't') {
      // Highlighted row: XOR a 64x7 solid bar (matches glyph height) so the row
      // lights up, then XOR the text over it (glyphs toggle dark) -> inverse. Both
      // are XOR, so re-running drawOp erases the row exactly (scroll erase-then-
      // draw). The bar is graphics packets with no built-in trailing gap, so wait
      // before the text write or the cluster drops it (rows vanished on 7px bars).
      if (op.f & kFontHighlight) {
        uint8_t bar[56]; memset(bar, 0xFF, sizeof(bar));
        fis_.GraphicFromArray(0, op.y, 64, 7, bar, 1);       // 1 = XOR mode
        delayMicroseconds(2000);                             // settle before text
      }
      return fis_.sendStringFS(op.x, op.y, (uint8_t)(op.f & ~kFontHighlight), String(op.s.c_str())) != 0;
    }
    uint8_t buf[1024];
    int bytes = (op.w * op.h + 7) / 8;
    if (bytes > (int)sizeof(buf)) bytes = sizeof(buf);
    for (int i = 0; i < bytes; ++i)
      buf[i] = (uint8_t)((hexv(op.s[i * 2]) << 4) | hexv(op.s[i * 2 + 1]));
    fis_.GraphicFromArray(op.x, op.y, op.w, op.h, buf, 1);  // bitmaps: no status, no retry
    return true;
  }
  static uint8_t hexv(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0; }

  static constexpr uint32_t kGapMs       = 5;    // gap AFTER each FIS write (VAGFISPages value)
  static constexpr uint32_t kKeepAliveMs = 900;  // idle keepalive cadence (VAGFISPages value)
  static constexpr uint32_t kRedrawMinMs = 90;   // cap full-redraw rate during fast scroll
  static constexpr uint32_t kSettleMs    = 300;  // after scroll stops, one heal redraw
  static constexpr uint8_t  kMaxRestarts = 6;    // bound page-redraw restarts on write failure

  VAGFISWriter fis_;
  FrameRecorder rec_;
  std::vector<FrameOp> drawn_;      // ops currently committed to the screen
  std::string topBuf_;              // last 16-char radio message
  std::deque<Cmd> q_;
  uint32_t lastWrite_ = 0, lastRedraw_ = 0;
  bool graphics_ = false;           // bus currently in graphics mode
  bool fullValid_ = false;          // drawn_ describes the live full-screen page
  bool haveTop_ = false, haveWritten_ = false;
  uint8_t redrawFails_ = 0;         // consecutive failed-write page restarts
  uint32_t writeFails_ = 0;         // total dropped FIS writes (diagnostics)
  bool needHeal_ = false;           // a partial (XOR) update happened; heal when quiet
};

} // namespace mmi
