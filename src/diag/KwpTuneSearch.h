// KwpTuneSearch.h — HAL-agnostic coarse-to-fine search for the best KWP1281
// K-line timing on THIS vehicle: the 5-baud init bit period, the inter-byte (W4)
// pause and the inter-frame (W3) block delay. Rather than brute-forcing the full
// grid (~105 combos), it coordinate-descends: a COARSE pass per axis finds the
// working region, then a FINE pass per axis compares ms-level differences around
// the winner. ~28 measurements total.
//
// It is pure and deterministic: the owner measures each candidate on the real
// K-line and feeds back a score (reads achieved in a fixed window; higher =
// better timing), and the search advances. State is a plain POD (save()/load())
// so a long sweep can be persisted and RESUMED across several drives/reboots.
#pragma once
#include <cstdint>

namespace mmi {

struct KwpTiming { int init; int byte; int frame; };

class KwpTuneSearch {
public:
  // Persisted progress. POD so it can be blobbed/int-packed to NVS as-is.
  struct State {
    int16_t stage;      // 0..kStages (== kStages when finished)
    int16_t cand;       // candidate index within the current stage
    int16_t curInit;    // incumbent (best-so-far) timing
    int16_t curByte;
    int16_t curFrame;
    int32_t bestScore;  // best score seen in the current stage (-1 = none yet)
    int16_t bestVal;    // axis value that produced bestScore
    int32_t incScore;   // score measured for the incumbent's own value this stage (-1 = not measured)
    uint8_t active;     // a sweep is in progress
    uint8_t done;       // sweep finished; best() is the winner
  };

  // Begin a fresh search from a baseline (default = the timing that "sometimes"
  // works on this car). The coarse passes will move off it as needed.
  void start(int init = 200, int byte = 5, int frame = 0) {
    st_.stage = 0; st_.cand = 0;
    st_.curInit = (int16_t)init; st_.curByte = (int16_t)byte; st_.curFrame = (int16_t)frame;
    st_.bestScore = -1; st_.bestVal = (int16_t)init;   // stage 0 is the init axis
    st_.incScore = -1;
    st_.active = 1; st_.done = 0;
  }
  void load(const State& s) { st_ = s; }
  State save() const { return st_; }

  bool active() const { return st_.active && !st_.done; }
  bool done() const { return st_.done != 0; }

  // The timing triple to measure right now: the incumbent with the active axis
  // replaced by the current candidate value.
  KwpTiming current() const {
    KwpTiming t{ st_.curInit, st_.curByte, st_.curFrame };
    int v = candValue(st_.stage, st_.cand, st_.curInit, st_.curByte, st_.curFrame);
    switch (axisOf(st_.stage)) { case 0: t.init = v; break; case 1: t.byte = v; break; default: t.frame = v; }
    return t;
  }

  // Feed back the score for current(); advance to the next candidate, and at the
  // end of a stage commit that stage's winner into the incumbent (only if some
  // candidate actually read data, so a hopeless stage leaves the incumbent alone).
  //
  // Incumbent hysteresis: track the plain argmax candidate, but also measure the
  // incumbent's OWN value (the fine stages include offset 0; coarse stages may or
  // may not contain it). At the end of the stage, only MOVE off the incumbent if
  // the best challenger beats the incumbent's score by more than a ~10% noise
  // margin — otherwise stay put. This resolves real differences while refusing to
  // chase measurement noise, and it's why the fine stages measure longer (more
  // reads -> smaller margin -> finer differences become resolvable). If the
  // incumbent value wasn't itself measured (coarse stage), just take the best
  // candidate that actually read something.
  void report(int score) {
    if (!active()) return;
    int v = candValue(st_.stage, st_.cand, st_.curInit, st_.curByte, st_.curFrame);
    if (v == incumbentAxisValue(st_.stage)) st_.incScore = score;   // measured the incumbent itself
    if (score > st_.bestScore) { st_.bestScore = score; st_.bestVal = (int16_t)v; }
    if (++st_.cand >= candCount(st_.stage)) {
      int ref = st_.incScore;
      int margin = ref > 20 ? ref / 10 : (st_.bestScore > 20 ? (int)st_.bestScore / 10 : 2);
      bool improve = ref < 0 ? (st_.bestScore > 0)                 // incumbent not measured -> take best
                             : (st_.bestScore > ref + margin);     // else must clearly beat staying put
      if (improve) {
        switch (axisOf(st_.stage)) {
          case 0: st_.curInit  = st_.bestVal; break;
          case 1: st_.curByte  = st_.bestVal; break;
          default: st_.curFrame = st_.bestVal; break;
        }
      }
      st_.stage++; st_.cand = 0; st_.bestScore = -1; st_.incScore = -1;
      if (st_.stage >= kStages) { st_.done = 1; st_.active = 0; }
    }
  }

  KwpTiming best() const { return { st_.curInit, st_.curByte, st_.curFrame }; }

  // Progress, for on-screen / telemetry display.
  int candIndex() const { int n = 0; for (int s = 0; s < st_.stage; ++s) n += candCount(s); return n + st_.cand; }
  static int totalCands() { int n = 0; for (int s = 0; s < kStages; ++s) n += candCount(s); return n; }
  int stage() const { return st_.stage; }
  // Fine stages compare ms-level differences, so the owner should measure each
  // candidate LONGER (more reads -> smaller noise band -> the tiebreak is real).
  bool inFineStage() const { return st_.stage >= 3; }

private:
  static constexpr int kStages = 6;
  // Axis under test per stage: init/byte/frame coarse (0-2) then fine (3-5).
  static int axisOf(int stage) { static const int a[kStages] = {0, 1, 2, 0, 1, 2}; return a[stage]; }
  static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
  // The incumbent's value on this stage's axis (fixed within a stage; committed
  // only at stage end). Used as the reference point for the tie tiebreak.
  int incumbentAxisValue(int stage) const {
    switch (axisOf(stage)) { case 0: return st_.curInit; case 1: return st_.curByte; default: return st_.curFrame; }
  }

  static int candCount(int stage) {
    switch (stage) {
      case 0: return 5; case 1: return 6; case 2: return 3;   // coarse
      case 3: return 7; case 4: return 7; case 5: return 5;   // fine (include offset 0 = incumbent)
    }
    return 0;
  }
  // Candidate value for (stage,cand). Coarse stages use absolute lists; fine
  // stages step around the incumbent to compare small differences and INCLUDE
  // offset 0 so the incumbent is re-measured (with the longer fine window) and
  // only displaced if a neighbour beats it by the noise margin. init and byte
  // fine steps are true 1ms (±1,2,3); frame steps ±10,20 (block-delay is a
  // tens-of-ms parameter — 1ms there is below the measurement noise).
  static int candValue(int stage, int cand, int curInit, int curByte, int curFrame) {
    static const int initC[5]  = {190, 195, 200, 205, 210};
    static const int byteC[6]  = {0, 4, 7, 15, 25, 50};
    static const int frameC[3] = {0, 40, 85};
    static const int initF[7]  = {-3, -2, -1, 0, 1, 2, 3};
    static const int byteF[7]  = {-3, -2, -1, 0, 1, 2, 3};
    static const int frameF[5] = {-20, -10, 0, 10, 20};
    switch (stage) {
      case 0: return initC[cand];
      case 1: return byteC[cand];
      case 2: return frameC[cand];
      case 3: return clampi(curInit  + initF[cand],  180, 220);
      case 4: return clampi(curByte  + byteF[cand],  0,   60);
      case 5: return clampi(curFrame + frameF[cand], 0,   100);
    }
    return 0;
  }

  State st_{};
};

} // namespace mmi
