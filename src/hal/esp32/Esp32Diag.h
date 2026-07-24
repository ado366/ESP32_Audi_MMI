// Esp32Diag.h — IDiag backed by the ported KWP1281 reader, driven from a
// dedicated FreeRTOS task on core 0 so the ~2s 5-baud init and blocking block
// reads never stall the UI loop (core 1). readGroup() is non-blocking: it sets
// the requested ECU/group and returns the latest cached values.
#pragma once
#include "../../diag/Diagnostics.h"
#include "../../Config.h"
#include "KWP.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cstdlib>
#include <cstdio>
#include <string>

namespace mmi {

class Esp32Diag : public IDiag {
public:
  void begin() {
    mtx_ = xSemaphoreCreateMutex();
    baud9600_[0x01] = true;   // engine ECU (EDC15) is 9600 on this car -> start correct, no 10400 probe
    xTaskCreatePinnedToCore(&Esp32Diag::thunk, "kwp", 4096, this, 1, &task_, 0);
  }

  bool isConnected() const override { return connected_; }
  std::string kwpDebug() const { return std::string(kwp_.dbg.c_str()); }  // connect-flow trace
  uint32_t readCount() const { return reads_; }                            // total measuring-block reads
  void requestProbe(int rx = -1, int tx = -1) { probeRx_ = rx; probeTx_ = tx; probeReq_ = true; }
  // Non-invasive ISO 14230 fast-init probe on the given ECU; result at /kwpdbg.
  void requestFastInit(uint8_t ecu = 0x01) { fastInitEcu_ = ecu; fastInitReq_ = true; }
  void requestRead(uint8_t ecu, uint8_t group) { reqEcu_ = ecu; reqGroup_ = group; }  // debug: trigger connect+read

  bool readGroup(uint8_t ecuAddr, uint8_t group, Group& out) override {
    reqEcu_ = ecuAddr; reqGroup_ = group;      // hand the request to the task
    bool ok = false;
    if (mtx_ && xSemaphoreTake(mtx_, 0) == pdTRUE) {
      if (haveData_ && curEcu_ == ecuAddr && curGroup_ == group) { out = latest_; ok = true; }
      xSemaphoreGive(mtx_);
    }
    return ok;
  }

  bool readFaults(uint8_t ecuAddr, std::vector<Dtc>& out) override {
    faultEcu_ = ecuAddr; faultReq_ = true;       // task performs it off the UI core
    bool ok = false;
    if (mtx_ && xSemaphoreTake(mtx_, 0) == pdTRUE) {
      if (faultsReady_) { out = faults_; ok = true; }
      xSemaphoreGive(mtx_);
    }
    return ok;
  }
  bool clearFaults(uint8_t ecuAddr) override { faultEcu_ = ecuAddr; clearReq_ = true; return true; }
  void setTiming(int initBitMs, int interByteMs, int blockDelayMs) override {
    kwp_.setTiming(initBitMs, interByteMs, blockDelayMs);
  }
  void stopReading() override { reqEcu_ = 0; reqGroup_ = 0; stopReq_ = true; }
  void setActive(bool a) override {                  // gauge on screen -> fast reads; else keep-alive
    if (a && !active_) { coldFails_ = 0; healthWindowStart_ = 0; }  // fresh health count per gauge-open
    active_ = a;
  }
  uint32_t coldFails() const { return coldFails_; }  // consecutive un-synced cold-connect failures

  // ---- KWP timing auto-tuner ----
  void startAutoTune() override {
    if (tuning_) return;
    tuneIdx_ = 0; tuneApply_ = true; tuneDone_ = false; tuneResultReady_ = false;
    tuneBestIdx_ = -1; tuneBestScore_ = -1;
    for (int i = 0; i < kNumCands; ++i) tuneScore_[i] = -1;
    tuning_ = true;
  }
  bool autoTuning() const override { return tuning_; }
  std::string autoTuneStatus() const override {
    char b[72];
    if (tuning_) {
      int i = tuneIdx_ < kNumCands ? tuneIdx_ : kNumCands - 1;
      const Cand& c = kCands[i];
      std::snprintf(b, sizeof(b), "TUNE %d/%d %lu/%lurd i%d b%d", i + 1, kNumCands,
                    (unsigned long)(reads_ - tuneStartReads_), (unsigned long)kTuneTargetReads, c.init, c.byte);
    } else if (tuneBestIdx_ >= 0) {
      const Cand& c = kCands[tuneBestIdx_];
      std::snprintf(b, sizeof(b), "BEST i%d b%d f%d %drd", c.init, c.byte, c.frame, tuneBestScore_);
    } else b[0] = 0;
    return std::string(b);
  }
  bool takeAutoTuneResult(int& initMs, int& byteMs, int& frameMs) override {
    if (!tuneResultReady_) return false;
    tuneResultReady_ = false;
    const Cand& c = kCands[tuneBestIdx_ >= 0 ? tuneBestIdx_ : 0];
    initMs = c.init; byteMs = c.byte; frameMs = c.frame;
    return true;
  }

private:
  static void thunk(void* p) { static_cast<Esp32Diag*>(p)->taskLoop(); }

  bool ensureConnected(uint8_t addr) {
    if (kwp_.isConnected() && kwp_.getCurrAddr() == addr) { connected_ = true; return true; }
    if (kwp_.isConnected()) kwp_.disconnect();
    // ECUs differ in K-line baud: this car's cluster (17) is 10400 but the
    // EDC15 engine ECU (01) answers at 9600 — at the wrong rate the 0x55 sync
    // smears into 0x95. ONE init per call, alternating baud on failure: an
    // EDC15 goes deaf for seconds after an aborted handshake, so back-to-back
    // attempts always miss. The caller's retry delay provides the quiet gap;
    // a success pins this address's baud for future reconnects.
    int baud = baud9600_[addr] ? 9600 : 10400;
    connected_ = kwp_.connect(addr, baud);    // blocking init — off the UI core
    // Baud handling that survives a noisy K-line (engine running):
    //  * The moment the sync byte (0x55 01 8A) matches at some baud, that baud is
    //    PROVEN for this address — lock it and never probe the other rate again.
    //  * Only alternate while still SEARCHING (never confirmed). Once locked, a
    //    failed connect is just noise (a garbled magic or connect block) — retry
    //    at the same rate instead of flapping 9600<->10400 and wasting every other
    //    attempt on the wrong baud (which is why it rarely connected while driving).
    if (kwp_.magicOk) baudConfirmed_[addr] = true;
    else if (!connected_ && !baudConfirmed_[addr]) baud9600_[addr] = !baud9600_[addr];
    // Cold-connect health: count consecutive failures that never even synced the
    // magic (the "can't establish" case). A confirmed-baud noise failure (magicOk)
    // isn't a cold fail. Drives the escalating backoff + the auto-tune trigger.
    if (connected_) coldFails_ = 0;
    else if (!kwp_.magicOk) { if (coldFails_ < 60000) coldFails_++; }
    return connected_;
  }

  // Wait after a failed connect. Baud locked -> quick noise retry. Otherwise the
  // EDC15 needs quiet time after an aborted handshake, and MORE of it the longer
  // it's been failing, so escalate 2.5s -> ~6s as cold failures pile up.
  void backoffCold() {
    if (kwp_.magicOk) { vTaskDelay(pdMS_TO_TICKS(400)); return; }
    uint32_t extra = coldFails_ > 4 ? 4 : coldFails_;
    vTaskDelay(pdMS_TO_TICKS(2500 + extra * 900));
  }

  // Auto "auto-tune": watch actual read THROUGHPUT while the user is looking at a
  // gauge and, if it's abysmal, kick off the timing sweep in the background WITHOUT
  // them having to find the menu item. Throughput (not connect-failure count) is
  // the right signal because it catches BOTH failure modes: never-connects (~0
  // reads) AND connects-then-drops-fast (a few reads then nothing). A healthy
  // K-line does ~3-4 reads/s (~50+ per 15s window); < kMinReadsPerWindow means
  // it's effectively broken. Gauges already show NO DATA when this fires, so the
  // sweep costs nothing (and it early-exits once a timing works — see runTuneStep)
  // and the winner persists for future drives. Once per boot; window resets
  // whenever a gauge (re)opens (see setActive).
  void checkThroughputAutoTune() {
    if (autoTunedThisBoot_ || tuning_ || !active_) return;
    uint32_t now = millis();
    if (healthWindowStart_ == 0) { healthWindowStart_ = now; healthStartReads_ = reads_; return; }
    if (now - healthWindowStart_ < kHealthWindowMs) return;
    if (reads_ - healthStartReads_ < kMinReadsPerWindow) { autoTunedThisBoot_ = true; startAutoTune(); }
    healthWindowStart_ = now; healthStartReads_ = reads_;   // start the next window
  }

  // One iteration of the timing sweep: apply the current candidate, connect to the
  // engine and read grp1, counting reads. A candidate ends when it either PASSES
  // (reaches kTuneTargetReads — a timing that sustains reads on this K-line) or is
  // ruled out (dead-fast: almost nothing after kTuneDeadMs; or capped at
  // kTuneCapMs). We EARLY-EXIT the whole sweep on the first pass rather than
  // grinding all 9 — for an abysmal connection, any timing that holds 50 reads is
  // the win, and the candidate list is ordered best-guess first. Bounds the sweep
  // to ~15-30s typically instead of a flat 3 minutes.
  void runTuneStep() {
    uint32_t now = millis();
    if (tuneApply_) {
      const Cand& c = kCands[tuneIdx_];
      kwp_.setTiming(c.init, c.byte, c.frame);
      if (kwp_.isConnected()) kwp_.disconnect();   // force a fresh connect at this candidate's timing
      tuneWindowStart_ = now; tuneStartReads_ = reads_; tuneApply_ = false;
    }
    if (ensureConnected(0x01)) {                    // engine ECU
      SENSOR s[16]; int n = kwp_.readBlock(0x01, 1, 16, s); if (n > 0) ++reads_;
    } else { vTaskDelay(pdMS_TO_TICKS(kwp_.magicOk ? 400 : 2500)); }
    connected_ = kwp_.isConnected();
    uint32_t windowReads = reads_ - tuneStartReads_;
    uint32_t elapsed = now - tuneWindowStart_;
    bool passed   = windowReads >= kTuneTargetReads;                             // sustains reads -> good
    bool deadFast = elapsed >= kTuneDeadMs && windowReads < kTuneDeadReads;      // basically nothing -> skip
    bool capped   = elapsed >= kTuneCapMs;                                       // marginal -> give up
    if (passed || deadFast || capped) {
      tuneScore_[tuneIdx_] = static_cast<int>(windowReads);
      if (passed) { finishTune(); return; }         // early-exit: first working timing wins
      if (++tuneIdx_ >= kNumCands) finishTune();
      else tuneApply_ = true;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  void finishTune() {
    int best = -1, bestScore = -1;
    for (int i = 0; i < kNumCands; ++i) if (tuneScore_[i] > bestScore) { bestScore = tuneScore_[i]; best = i; }
    tuneBestIdx_ = best; tuneBestScore_ = bestScore;
    const Cand& c = kCands[best >= 0 ? best : 0];
    kwp_.setTiming(c.init, c.byte, c.frame);        // apply the winner immediately
    tuning_ = false; tuneDone_ = true; tuneResultReady_ = true;
  }

  void taskLoop() {
    for (;;) {
      if (stopReq_) {                    // leave the K-line idle off diag screens
        stopReq_ = false;
        if (kwp_.isConnected()) kwp_.disconnect();
        connected_ = false; haveData_ = false;
      }
      if (probeReq_) { probeReq_ = false; if (kwp_.isConnected()) kwp_.disconnect(); kwp_.probe(probeRx_, probeTx_); continue; }
      if (fastInitReq_) { fastInitReq_ = false; if (kwp_.isConnected()) kwp_.disconnect();
                          kwp_.fastInitProbe(fastInitEcu_); connected_ = false; continue; }
      // Fault clear / read take priority and target their own ECU.
      if (clearReq_ || faultReq_) {
        uint8_t fe = faultEcu_;
        if (ensureConnected(fe)) {
          if (clearReq_) { kwp_.clearFaultCodes(); clearReq_ = false; faultsReady_ = false; }
          else {
            uint16_t codes[16]; uint8_t infos[16];
            int nf = kwp_.readFaultCodes(codes, infos, 16);
            std::vector<Dtc> tmp;
            for (int i = 0; i < nf; ++i) {
              Dtc d; d.code = codes[i]; d.info = infos[i]; d.sporadic = (infos[i] & 0x80) != 0;
              tmp.push_back(d);
            }
            if (xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
              faults_ = tmp; faultsReady_ = (nf >= 0); xSemaphoreGive(mtx_);
            }
            faultReq_ = false;
          }
        } else { backoffCold(); }                  // escalating retry gap for the deaf EDC15
        continue;
      }

      if (tuning_) { runTuneStep(); continue; }   // background timing sweep owns the K-line

      uint8_t e = reqEcu_, g = reqGroup_;
      if (e == 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
      // A 5-baud init that aborts leaves the EDC15 deaf for ~seconds, so a failed
      // connect normally waits 2.5s. But if the magic already synced (baud right,
      // noise killed a later stage) the init did NOT abort — retry fast.
      if (!ensureConnected(e)) { checkThroughputAutoTune(); backoffCold(); continue; }

      SENSOR s[16];
      int n = kwp_.readBlock(e, g, 16, s);
      if (n > 0) ++reads_;                          // measured live-read counter
      Group tmp; tmp.count = 0;
      for (int i = 0; i < n && tmp.count < 4; ++i) {
        Measurement m;
        m.label = std::string(s[i].desc.c_str());
        m.unit  = std::string(s[i].units.c_str());
        std::string val(s[i].value.c_str());
        if (m.unit.empty()) m.text = val; else m.value = atof(val.c_str());
        tmp.values[tmp.count++] = m;
      }
      if (xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
        latest_ = tmp; curEcu_ = e; curGroup_ = g; haveData_ = (n > 0);
        xSemaphoreGive(mtx_);
      }
      connected_ = kwp_.isConnected();
      checkThroughputAutoTune();     // abysmal read rate while a gauge is up -> auto-run the sweep
      // Fast poll while a gauge is displayed; a slow keep-alive otherwise, which
      // still reads often enough to HOLD the KWP session (ECU drops it after ~1-2s
      // of silence) so re-opening a gauge doesn't pay another cold reconnect.
      vTaskDelay(pdMS_TO_TICKS(active_ ? 30 : 700));
    }
  }

  mutable KWP kwp_{static_cast<uint8_t>(cfg::PIN_KWP_RX), static_cast<uint8_t>(cfg::PIN_KWP_TX)};
  SemaphoreHandle_t mtx_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Group latest_;
  std::vector<Dtc> faults_;
  volatile uint8_t reqEcu_ = 0, reqGroup_ = 0, curEcu_ = 0, curGroup_ = 0, faultEcu_ = 0;
  volatile bool connected_ = false, haveData_ = false, stopReq_ = false;
  volatile bool active_ = false;   // a gauge is on screen (fast poll vs keep-alive)
  volatile uint32_t coldFails_ = 0;        // consecutive un-synced cold-connect failures
  bool autoTunedThisBoot_ = false;         // background sweep auto-fired once already
  uint32_t healthWindowStart_ = 0, healthStartReads_ = 0;   // read-throughput window
  static constexpr uint32_t kHealthWindowMs = 15000;        // window to judge throughput
  static constexpr uint32_t kMinReadsPerWindow = 8;         // < this in a window = broken -> auto-tune
  bool baud9600_[256] = {false};   // per-address: use 9600 (else 10400)
  bool baudConfirmed_[256] = {false};  // per-address: a baud has synced the magic -> stop probing
  volatile bool faultReq_ = false, clearReq_ = false, faultsReady_ = false, probeReq_ = false;
  volatile bool fastInitReq_ = false;
  volatile uint8_t fastInitEcu_ = 0x01;
  volatile int  probeRx_ = -1, probeTx_ = -1;
  volatile uint32_t reads_ = 0;

  // Auto-tuner: candidate init-pulse / inter-byte / block-delay sets to sweep.
  // Spread covers Maxi-K (init 190-210, inter-byte 4-7) and FIS-Control (50/85).
  struct Cand { int init, byte, frame; };
  static constexpr Cand kCands[] = {
    {200,  5,  0}, {200, 10,  0}, {195,  5,  0}, {205,  5,  0}, {190,  4,  0},
    {210,  7,  0}, {200, 25,  0}, {200, 50, 85}, {200,  4, 40},
  };
  static constexpr int kNumCands = sizeof(kCands) / sizeof(kCands[0]);
  static constexpr uint32_t kTuneTargetReads = 50;   // reads that mark a candidate as working -> pass
  static constexpr uint32_t kTuneCapMs   = 15000;    // max time on a marginal candidate before giving up
  static constexpr uint32_t kTuneDeadMs  = 6000;     // abandon a candidate this fast if it's...
  static constexpr uint32_t kTuneDeadReads = 3;      // ...read almost nothing (clearly wrong timing)
  volatile bool tuning_ = false, tuneApply_ = false, tuneDone_ = false, tuneResultReady_ = false;
  int tuneIdx_ = 0, tuneBestIdx_ = -1, tuneBestScore_ = -1;
  int tuneScore_[kNumCands] = {0};
  uint32_t tuneWindowStart_ = 0, tuneStartReads_ = 0;
};

} // namespace mmi
