// Esp32Display.h — IDisplay over VAGFISWriter, with per-op region diffing and a
// NON-BLOCKING, millis-paced write queue.
//
// Draw calls only RECORD a frame. flush() diffs it against the frame we've
// already ENQUEUED (target_) and appends FIS commands for just the changed ops:
// each changed op clears only its own extent (initScreen 0x82 over that strip)
// then redraws — so unchanged rows are never blanked and fast scrolling can't
// wipe the screen. service(now) sends at most one queued command per kGapMs (the
// slow FIS bus needs a gap between writes) — no delay(), so the loop keeps running.
//
// Correctness note: we NEVER mark a frame "sent" before service() writes it.
// target_ tracks what has been ENQUEUED, so a second flush() before the queue
// drains diffs against the pending picture and can't drop an unsent row.
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
#include <algorithm>

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
      targetFull_ = false; target_.clear();
      if (haveTop_ && topSent1_ == rec_.top1() && topSent2_ == rec_.top2()) return;
      // A pending full-screen update is now stale; drop it and leave graphics mode.
      q_.clear();
      if (graphics_ || graphicsPending_) { q_.push_back({Cmd::ExitGfx, {}, ""}); graphicsPending_ = false; }
      // Build the 16-char radio message. The FIS auto-centres lines under 8 chars;
      // for full/scrolling lines (>=8) replace spaces with 0x1C so they stay left.
      std::string buf(16, ' ');
      for (int i = 0; i < 8 && i < (int)rec_.top1().size(); ++i) buf[i]     = rec_.top1()[i];
      for (int i = 0; i < 8 && i < (int)rec_.top2().size(); ++i) buf[8 + i] = rec_.top2()[i];
      if ((int)rec_.top1().size() >= 8) for (int i = 0; i < 8;  ++i) if (buf[i] == ' ') buf[i] = 0x1C;
      if ((int)rec_.top2().size() >= 8) for (int i = 8; i < 16; ++i) if (buf[i] == ' ') buf[i] = 0x1C;
      q_.push_back({Cmd::TopLine, {}, buf});
      topSent1_ = rec_.top1(); topSent2_ = rec_.top2(); haveTop_ = true;
      return;
    }

    // Full-screen graphics mode.
    haveTop_ = false;
    const auto& ops = rec_.ops();
    if (!targetFull_) {
      // Fresh entry into full-screen: one clear + redraw of everything.
      q_.clear();
      q_.push_back({Cmd::Init, {}, ""});
      for (const auto& op : ops) q_.push_back({Cmd::Draw, op, ""});
      target_ = ops; targetFull_ = true; graphicsPending_ = true;
      return;
    }

    // Incremental: only touch ops whose slot is new or whose content changed, and
    // clear ops that vanished. Each change clears just its own extent, so other
    // rows stay lit — no full-screen blank during fast scrolling.
    for (const auto& op : ops) {
      const FrameOp* old = findSlot(target_, op);
      if (old && old->f == op.f && old->s == op.s) continue; // unchanged
      Region r = old ? unionR(extent(*old), extent(op)) : extent(op);
      dropQueuedSlot(op);
      q_.push_back({Cmd::ClearRegion, op, "", r.x, r.y, r.x2, r.y2});
      q_.push_back({Cmd::Draw, op, ""});
    }
    for (const auto& old : target_) {
      if (findSlot(ops, old)) continue; // still present
      Region r = extent(old);
      dropQueuedSlot(old);
      q_.push_back({Cmd::ClearRegion, old, "", r.x, r.y, r.x2, r.y2});
    }
    target_ = ops;
  }

  // Send at most one queued command, paced by millis(). Call every loop.
  void service(uint32_t now) {
    if (q_.empty()) return;
    if (haveWritten_ && (uint32_t)(now - lastWrite_) < kGapMs) return;
    const Cmd& c = q_.front();
    switch (c.kind) {
      case Cmd::Init:        fis_.initFullScreen(); graphics_ = true; break;
      case Cmd::ExitGfx:     fis_.initScreen(0, 0, 1, 1, 0x80); graphics_ = false; break;
      case Cmd::TopLine:     { char b[17]; memcpy(b, c.buf.data(), 16); b[16] = 0; fis_.sendMsg(b); } break;
      case Cmd::ClearRegion: fis_.initScreen(c.rx, c.ry, c.rx2, c.ry2, 0x82); graphics_ = true; break;
      case Cmd::Draw:        drawOp(c.op); break;
    }
    q_.pop_front();
    lastWrite_ = now; haveWritten_ = true;
  }

private:
  struct Cmd { enum Kind { Init, ExitGfx, TopLine, ClearRegion, Draw } kind;
               FrameOp op; std::string buf; uint8_t rx = 0, ry = 0, rx2 = 0, ry2 = 0; };
  struct Region { uint8_t x, y, x2, y2; };

  static const FrameOp* findSlot(const std::vector<FrameOp>& v, const FrameOp& o) {
    for (const auto& e : v) if (e.sameSlot(o)) return &e;
    return nullptr;
  }
  void dropQueuedSlot(const FrameOp& o) {
    for (auto it = q_.begin(); it != q_.end();) {
      if ((it->kind == Cmd::Draw || it->kind == Cmd::ClearRegion) && it->op.sameSlot(o)) it = q_.erase(it);
      else ++it;
    }
  }

  // The screen extent an op covers, so we clear exactly it (and no neighbour).
  static Region extent(const FrameOp& o) {
    if (o.t == 'b') return {o.x, o.y, (uint8_t)std::min(64, o.x + o.w), (uint8_t)(o.y + o.h)};
    uint8_t y2 = (uint8_t)(o.y + 8);                 // 7px glyph + 1px margin
    if (o.f & 0x20) return {0, o.y, 64, y2};         // centered text spans full width
    uint8_t cw = (o.f & 0x04) ? 5 : 7;               // compressed vs standard glyph width
    int x2 = o.x + (int)o.s.size() * cw + 2;
    return {o.x, o.y, (uint8_t)std::min(64, x2), y2};
  }
  static Region unionR(Region a, Region b) {
    return {(uint8_t)std::min(a.x, b.x), (uint8_t)std::min(a.y, b.y),
            (uint8_t)std::max(a.x2, b.x2), (uint8_t)std::max(a.y2, b.y2)};
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

  static constexpr uint32_t kGapMs = 5;   // min gap between FIS writes

  VAGFISWriter fis_;
  FrameRecorder rec_;
  std::vector<FrameOp> target_;           // what we've ENQUEUED (not necessarily sent yet)
  std::string topSent1_, topSent2_;
  std::deque<Cmd> q_;
  uint32_t lastWrite_ = 0;
  bool graphics_ = false;                 // bus currently in graphics mode
  bool graphicsPending_ = false;          // an Init is queued but not yet sent
  bool targetFull_ = false;               // target_ describes a full-screen frame
  bool haveTop_ = false, haveWritten_ = false;
};

} // namespace mmi
