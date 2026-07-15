// Esp32Inputs.h — IInputs from the rotary encoder + three analog button ladders,
// plus guided button-learning (calibration).
//
// The encoder is a digital input, independent of the (uncalibrated) analog
// ladders, so it is the reliable way to trigger/cancel calibration. Calibration
// is auto-capture: prompt a button, wait for a stable press, average, advance.
#pragma once
#include "../IInputs.h"
#include "../IStorage.h"
#include "../../Config.h"
#include <AnalogMultiButton.h>
#include <Arduino.h>
#include <deque>
#include <cstring>
#include <algorithm>
#include <utility>

namespace mmi {

class Esp32Inputs : public IInputs {
public:
  void begin() {
    pinMode(cfg::PIN_ENC_A, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_B, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_BUTTON, INPUT_PULLUP);
    lastEnc_ = (digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B);
    // Deliberate boot gesture: encoder button held CONTINUOUSLY for ~1.5s at
    // power-on requests calibration. A brief tap won't do it; if it isn't held
    // at all we return immediately (no boot delay).
    calibReq_ = false;
    if (digitalRead(cfg::PIN_ENC_BUTTON) == LOW) {
      uint32_t t0 = millis(); bool held = true;
      while (millis() - t0 < kBootHoldMs) {
        if (digitalRead(cfg::PIN_ENC_BUTTON) != LOW) { held = false; break; }
        delay(10);
      }
      calibReq_ = held;
    }
    setDefaults();
    build();
  }

  void loadThresholds(const IStorage& s) {
    loadLadder(s, "cal.v1", "cal.k1", v1_, k1_, 5);
    loadLadder(s, "cal.v2", "cal.k2", v2_, k2_, 5);
    loadLadder(s, "cal.vs", "cal.ks", vs_, ks_, 8);
    build();
  }
  void saveThresholds(IStorage& s) const {
    saveLadder(s, "cal.v1", "cal.k1", v1_, k1_, 5);
    saveLadder(s, "cal.v2", "cal.k2", v2_, k2_, 5);
    saveLadder(s, "cal.vs", "cal.ks", vs_, ks_, 8);
    s.commit();
  }

  void update() {
    raw_[0] = analogRead(cfg::PIN_BTN_CONSOLE_1);
    raw_[1] = analogRead(cfg::PIN_BTN_CONSOLE_2);
    raw_[2] = analogRead(cfg::PIN_BTN_STEERING);
    serviceEncoder();                 // encoder always runs (used to cancel calibration)
    if (cal_) { runCalibration(); return; }  // ladders are captured, not dispatched

    b1_->update(); b2_->update(); sw_->update();
    for (int i = 0; i < 5; ++i) if (b1_->onPress(i) && k1_[i] != Control::None) push(k1_[i]);
    for (int i = 0; i < 5; ++i) if (b2_->onPress(i) && k2_[i] != Control::None) push(k2_[i]);
    for (int i = 0; i < 8; ++i) {
      if (ks_[i] == Control::None) continue;
      // Right+ distinguishes a tap (next track) from a hold (voice assistant);
      // so it fires on RELEASE for a tap, and once when held past the threshold.
      if (ks_[i] == Control::SteerRightPlus) {
        if (sw_->onPressAfter(i, kSteerHoldMs))        push(Control::SteerRightPlusHold);
        else if (sw_->onReleaseBefore(i, kSteerHoldMs)) push(Control::SteerRightPlus);
      } else if (sw_->onPress(i)) {
        push(ks_[i]);
      }
    }
  }

  bool poll(InputEvent& out) override {
    if (q_.empty()) return false;
    out = q_.front(); q_.pop_front();
    return true;
  }
  int rawLadder(int ladder) const override { return (ladder >= 0 && ladder < 3) ? raw_[ladder] : -1; }

  // Inject a control from the browser control/debug UI — same pipeline as the
  // physical buttons, so the whole UI is drivable without the car controls.
  void inject(Control c, int8_t delta = 0) { if (c != Control::None) q_.push_back({c, delta}); }

  // ---- calibration API ----
  bool calibrating() const override { return cal_; }
  bool calibrationRequested() const override { return calibReq_; }
  void startCalibration() override {
    cal_ = true; calStep_ = 0; capState_ = 0;
    for (int i = 0; i < 3; ++i) baseline_[i] = analogRead(ladderPin(i));
    stepStart_ = millis();
  }
  void cancelCalibration() override { cal_ = false; }
  const char* calibPrompt() const override { return cal_ ? kSeq[calStep_ < kTotal ? calStep_ : kTotal - 1].name : (justSaved_ ? "SAVED" : ""); }
  int calibStep()  const override { return calStep_; }
  int calibTotal() const override { return kTotal; }

private:
  // ---- ladder <-> control mapping (defaults from v1; overwritten by calibration) ----
  void setDefaults() {
    static const int  dv1[5] = {330, 389, 1001, 1467, 1883};
    static const int  dv2[5] = {330, 389, 1001, 1467, 1883};
    static const int  dvs[8] = {1622, 1679, 1872, 2152, 2516, 2627, 2967, 3247};
    static const Control dk1[5] = {Control::MenuNav, Control::Nav, Control::MenuReturn, Control::Return, Control::Menu};
    static const Control dk2[5] = {Control::InfoReverse, Control::Info, Control::TrafficReverse, Control::Traffic, Control::None};
    static const Control dks[8] = {Control::SteerLPlusRMinus, Control::SteerLPlusRPlus, Control::SteerLeftPlus,
                                   Control::SteerLMinusRMinus, Control::SteerLMinusRPlus, Control::SteerRightMinus,
                                   Control::SteerLeftMinus, Control::SteerRightPlus};
    memcpy(v1_, dv1, sizeof v1_); memcpy(v2_, dv2, sizeof v2_); memcpy(vs_, dvs, sizeof vs_);
    memcpy(k1_, dk1, sizeof k1_); memcpy(k2_, dk2, sizeof k2_); memcpy(ks_, dks, sizeof ks_);
  }
  static int ladderPin(int l) {
    return l == 0 ? cfg::PIN_BTN_CONSOLE_1 : l == 1 ? cfg::PIN_BTN_CONSOLE_2 : cfg::PIN_BTN_STEERING;
  }
  void build() {
    delete b1_; delete b2_; delete sw_;
    b1_ = new AnalogMultiButton(cfg::PIN_BTN_CONSOLE_1, 5, v1_);
    b2_ = new AnalogMultiButton(cfg::PIN_BTN_CONSOLE_2, 5, v2_);
    sw_ = new AnalogMultiButton(cfg::PIN_BTN_STEERING,  8, vs_);
  }
  void push(Control c, int8_t d = 0) { q_.push_back({c, d}); }

  // ---- calibration capture sequence (covers every ladder slot) ----
  struct Step { uint8_t ladder; Control ctrl; const char* name; };
  static constexpr Step kSeq[18] = {
    {0, Control::Menu, "MENU"}, {0, Control::Return, "RETURN"}, {0, Control::Nav, "NAV"},
    {0, Control::MenuReturn, "MENU+RET"}, {0, Control::MenuNav, "MENU+NAV"},
    {1, Control::Info, "INFO"}, {1, Control::Traffic, "TRAFFIC"},
    {1, Control::TrafficReverse, "TRAF+REV"}, {1, Control::InfoReverse, "INFO+REV"}, {1, Control::None, "REVERSE"},
    {2, Control::SteerLeftPlus, "L+"}, {2, Control::SteerLeftMinus, "L-"},
    {2, Control::SteerRightPlus, "R+"}, {2, Control::SteerRightMinus, "R-"},
    {2, Control::SteerLPlusRPlus, "L+R+"}, {2, Control::SteerLPlusRMinus, "L+R-"},
    {2, Control::SteerLMinusRPlus, "L-R+"}, {2, Control::SteerLMinusRMinus, "L-R-"},
  };
  static constexpr int kTotal = 18;

  void runCalibration() {
    if (calStep_ >= kTotal) { finalizeCalibration(); return; }
    int l = kSeq[calStep_].ladder;
    int r = raw_[l];
    bool pressed = r < baseline_[l] - 250;
    uint32_t now = millis();
    if (capState_ == 0) {                       // waiting for a press
      if (pressed) { capState_ = 1; capStart_ = now; sum_ = 0; cnt_ = 0; }
      else if (now - stepStart_ > 8000) { captured_[calStep_] = -1; advance(); } // timeout -> skip
    } else if (capState_ == 1) {                // stable press -> average
      if (!pressed) { capState_ = 0; }          // released too soon
      else { sum_ += r; ++cnt_;
        if (now - capStart_ > 500) { captured_[calStep_] = (int)(sum_ / cnt_); capState_ = 2; } }
    } else {                                     // wait for release
      if (r > baseline_[l] - 120) advance();
    }
  }
  void advance() { ++calStep_; capState_ = 0; stepStart_ = millis(); }

  void finalizeCalibration() {
    buildLadder(0, 0, 5, v1_, k1_);
    buildLadder(1, 5, 5, v2_, k2_);
    buildLadder(2, 10, 8, vs_, ks_);
    build();
    if (storage_) saveThresholds(*storage_);
    cal_ = false; justSaved_ = true;
  }
  // Rebuild one ladder's ascending values[] + parallel control map from captured
  // readings — but only if every slot was captured (else keep the old values).
  void buildLadder(int ladder, int base, int n, int* outV, Control* outK) {
    for (int i = 0; i < n; ++i) if (captured_[base + i] < 0) return; // incomplete -> keep defaults
    std::pair<int, Control> pairs[8];
    for (int i = 0; i < n; ++i) pairs[i] = {captured_[base + i], kSeq[base + i].ctrl};
    std::sort(pairs, pairs + n, [](auto& a, auto& b) { return a.first < b.first; });
    for (int i = 0; i < n; ++i) { outV[i] = pairs[i].first; outK[i] = pairs[i].second; }
  }

  void loadLadder(const IStorage& s, const char* vk, const char* kk, int* v, Control* k, int n) {
    std::string b;
    if (s.readBlob(vk, b) && (int)b.size() == (int)(n * sizeof(int))) memcpy(v, b.data(), n * sizeof(int));
    if (s.readBlob(kk, b) && (int)b.size() == n) for (int i = 0; i < n; ++i) k[i] = (Control)(uint8_t)b[i];
  }
  void saveLadder(IStorage& s, const char* vk, const char* kk, const int* v, const Control* k, int n) const {
    s.writeBlob(vk, std::string(reinterpret_cast<const char*>(v), n * sizeof(int)));
    std::string kb; for (int i = 0; i < n; ++i) kb.push_back((char)(uint8_t)k[i]);
    s.writeBlob(kk, kb);
  }

  void serviceEncoder() {
    static const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
    uint8_t s = (digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B);
    if (s != lastEnc_) {
      accum_ += tbl[(lastEnc_ << 2) | s];
      lastEnc_ = s;
      if (accum_ >= 4)  { push(Control::EncoderCW,  +1); ++encPos_; accum_ = 0; }
      else if (accum_ <= -4) { push(Control::EncoderCW, -1); --encPos_; accum_ = 0; }
    }
    bool pressed = digitalRead(cfg::PIN_ENC_BUTTON) == LOW;
    uint32_t now = millis();
    if (pressed && !wasPressed_) { pressStart_ = now; holdSent_ = false; }
    if (pressed && !holdSent_ && (now - pressStart_) > kHoldMs) { push(Control::EncoderHold); ++encHolds_; holdSent_ = true; }
    if (!pressed && wasPressed_ && !holdSent_ && (now - pressStart_) < kHoldMs) { push(Control::EncoderClick); ++encClicks_; }
    wasPressed_ = pressed;
  }

public:
  void attachStorage(IStorage* s) { storage_ = s; } // so finalize can persist

  bool encoderDebug(EncoderDebug& out) const override {
    out.pos = encPos_;
    out.a = lastEnc_ & 0x02; out.b = lastEnc_ & 0x01;   // raw quadrature levels
    out.pressed = wasPressed_;
    out.clicks = encClicks_; out.holds = encHolds_;
    return true;
  }

private:
  static constexpr uint32_t kHoldMs = 3000;     // runtime long-press -> open menu
  static constexpr uint32_t kSteerHoldMs = 600; // steering Right+ hold -> voice assistant
  static constexpr uint32_t kBootHoldMs = 700;  // sustained hold at boot -> calibration
  int v1_[5], v2_[5], vs_[8];
  Control k1_[5], k2_[5], ks_[8];
  AnalogMultiButton *b1_ = nullptr, *b2_ = nullptr, *sw_ = nullptr;
  IStorage* storage_ = nullptr;
  int raw_[3] = {0, 0, 0};
  std::deque<InputEvent> q_;
  uint8_t lastEnc_ = 0;
  int accum_ = 0;
  int encPos_ = 0;                      // accumulated detents (encoder debug screen)
  uint16_t encClicks_ = 0, encHolds_ = 0;
  bool wasPressed_ = false, holdSent_ = false;
  uint32_t pressStart_ = 0;
  // calibration state
  bool cal_ = false, calibReq_ = false, justSaved_ = false;
  int calStep_ = 0, capState_ = 0, cnt_ = 0;
  long sum_ = 0;
  int captured_[18] = {0};
  int baseline_[3] = {4095, 4095, 4095};
  uint32_t stepStart_ = 0, capStart_ = 0;
};

} // namespace mmi
