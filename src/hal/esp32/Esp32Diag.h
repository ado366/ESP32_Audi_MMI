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
    // 8KB stack: the KWP task nests readBlock -> parseGroup (VLA) -> getSensorData
    // (String temporaries) with a SENSOR[16] local; 4KB was uncomfortably tight.
    xTaskCreatePinnedToCore(&Esp32Diag::thunk, "kwp", 8192, this, 1, &task_, 0);
  }

  bool isConnected() const override { return connected_; }
  std::string kwpDebug() const { return std::string(kwp_.dbg.c_str()) + "last block: " + kwp_.lastRaw.c_str() + "\n--- connect history ---\n" + kwp_.connHist.c_str(); }  // connect-flow trace
  uint32_t readCount() const { return reads_; }                            // total measuring-block reads
  void requestProbe(int rx = -1, int tx = -1) { probeRx_ = rx; probeTx_ = tx; probeReq_ = true; }
  // Non-invasive ISO 14230 fast-init probe on the given ECU; result at /kwpdbg.
  void requestFastInit(uint8_t ecu = 0x01) { fastInitEcu_ = ecu; fastInitReq_ = true; }
  void requestRead(uint8_t ecu, uint8_t group) { reqEcu_ = ecu; reqGroup_ = group; }  // debug: trigger connect+read
  // Live module-switch knobs for on-car A/B (/kwpswitch): end-session on/off, idle ms.
  void setSwitchOpts(int sendEnd, int idleMs, int otherIdleMs = -1) { if (sendEnd >= 0) sendEnd_ = sendEnd != 0; if (idleMs >= 0) switchIdleMs_ = idleMs; if (otherIdleMs >= 0) switchIdleOtherMs_ = otherIdleMs; }
  std::string switchOpts() const { char b[64]; snprintf(b, sizeof(b), "end=%d idle=%dms oidle=%dms", sendEnd_ ? 1 : 0, (int)switchIdleMs_, (int)switchIdleOtherMs_); return b; }
  // Engine group scan (debug): reads every engine group 1..255 once and keeps the
  // raw (formula,a,b) triples — to hunt for a field (e.g. oil temp). /kwpscan.
  void requestScan() { scanReq_ = true; }
  std::string scanLog() const {
    std::string r;
    if (mtx_ && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) { r = scanOut_.c_str(); xSemaphoreGive(mtx_); }
    return r;
  }

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
      if (faultsReady_ && faultsFrom_ == ecuAddr) { out = faults_; ok = true; }   // never another module's list
      xSemaphoreGive(mtx_);
    }
    return ok;
  }
  bool clearFaults(uint8_t ecuAddr) override {
    // Drop the cached list now, so the UI's next poll can't re-show the
    // pre-clear faults while the task is still getting to the clear.
    if (mtx_ && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) { faultsReady_ = false; faults_.clear(); xSemaphoreGive(mtx_); }
    faultEcu_ = ecuAddr; clearReq_ = true; return true;
  }
  void setTiming(int initBitMs, int interByteMs, int blockDelayMs) override {
    kwp_.setTiming(initBitMs, interByteMs, blockDelayMs);
  }
  void stopReading() override { reqEcu_ = 0; reqGroup_ = 0; stopReq_ = true; }
  // Revert to holding the base ECU (engine) without disconnecting — called on
  // non-diagnostics screens so the engine session stays alive across the UI.
  void holdBase() override { reqEcu_ = 0; reqGroup_ = 0; }
  // Service mode: disconnect and keep the task completely off the K-line so an
  // external VCDS can drive the bus. Takes priority over everything in the task.
  void setServiceMode(bool on) override { serviceMode_ = on; }
  bool serviceMode() const override { return serviceMode_; }
  // Set which ecu/group the base keep-alive reads (App points this at the speed
  // group so the speedo cache is always warm).
  void setBaseRead(uint8_t ecu, uint8_t group) override { baseEcu_ = ecu; baseGroup_ = group; }
  void setActive(bool a) override {                  // gauge on screen -> fast reads; else keep-alive
    if (a && !active_) { coldFails_ = 0; healthWindowStart_ = 0; }  // fresh health count per gauge-open
    active_ = a;
  }
  uint32_t coldFails() const { return coldFails_; }  // consecutive un-synced cold-connect failures
  // Min free stack (bytes) the KWP task has ever had — a small number means it's
  // near overflow. Surfaced in /status to confirm/rule-out a stack-overflow hang.
  uint32_t taskStackFree() const { return task_ ? uxTaskGetStackHighWaterMark(task_) : 0; }
  bool usingFastInit() const { return kwp_.isKwp2000() || kwp_.isObd2(); }
  const char* protoName() const {                          // live engine protocol, for telemetry
    return kwp_.isObd2() ? "obd2" : kwp_.isKwp2000() ? "kwp2000" : "kwp1281";
  }

  // ---- KWP timing deep sweep (coarse-to-fine, resumable across drives) ----
  // Drives KwpTuneSearch: coordinate-descends init/byte/frame, coarse then fine,
  // measuring each candidate on the live K-line. Once started it persists after
  // every candidate (via takeSearchSnapshot) and resumes across reboots, until the
  // grid completes and the winner is applied. Engine must be reachable.
  void startAutoTune() override {
    search_.start();
    tuneResultReady_ = false; searchApply_ = true; searchDirty_ = true;
  }
  bool autoTuning() const override { return search_.active(); }
  std::string autoTuneStatus() const override {
    char b[72];
    if (search_.active()) {
      KwpTiming t = search_.current();
      std::snprintf(b, sizeof(b), "CAL %d/%d i%d b%d f%d",
                    search_.candIndex() + 1, KwpTuneSearch::totalCands(), t.init, t.byte, t.frame);
    } else if (search_.done()) {
      KwpTiming t = search_.best();
      std::snprintf(b, sizeof(b), "TUNED i%d b%d f%d", t.init, t.byte, t.frame);
    } else b[0] = 0;
    return std::string(b);
  }
  bool takeAutoTuneResult(int& initMs, int& byteMs, int& frameMs) override {
    if (!tuneResultReady_) return false;
    tuneResultReady_ = false;
    KwpTiming t = search_.best();
    initMs = t.init; byteMs = t.byte; frameMs = t.frame;
    return true;
  }
  // Restore a persisted sweep at boot; a snapshot is offered after each candidate.
  void resumeSearch(const KwpTuneSearch::State& s) override {
    search_.load(s);
    if (search_.active()) searchApply_ = true;
  }
  bool takeSearchSnapshot(KwpTuneSearch::State& out) override {
    if (!searchDirty_) return false;
    searchDirty_ = false; out = search_.save(); return true;
  }

private:
  static void thunk(void* p) { static_cast<Esp32Diag*>(p)->taskLoop(); }

  bool ensureConnected(uint8_t addr) {
    if (kwp_.isConnected() && kwp_.getCurrAddr() == addr) { connected_ = true; return true; }
    // Switching modules needs a longer quiet K-line than ISO's 300ms. On-car A/B
    // (2026-10-22, 3 rounds each, end-session OFF): 300ms failed the first
    // attempt every time both ways ("no reply"); to the CLUSTER 700ms+ was 3/3
    // first-try; to the ENGINE 700/1000ms were 0/3, 1500ms 3/3. A failed first
    // try costs ~4.3s + a 2.5s backoff, so the wait pays for itself.
    int idleMs = 300;
    if (kwp_.isConnected() && kwp_.getCurrAddr() != addr)
      idleMs = (addr == kEngine) ? switchIdleMs_ : switchIdleOtherMs_;
    // End-communication (0x06) is OFF by default: with it the next ENGINE connect
    // got a garbled sync ("bad magic B5 AB CF") on the first try, every time.
    if (kwp_.isConnected()) { if (sendEnd_) kwp_.endSession(); else kwp_.disconnect(); }
    // Engine ECU: TRY ISO 14230 fast-init / KWP2000 first (far cheaper connect,
    // no 5-baud timing fragility). If the ECU doesn't answer fast-init after a few
    // tries, lock to KWP1281 for the rest of this boot and fall through to the
    // 5-baud path (+ the auto-calibration). Cluster (0x17) is KWP1281 only.
    if (addr == kEngine && proto_[addr] != kProto1281) {
      // 1) VAG measuring blocks over KWP2000; 2) generic OBD-II PIDs at 0x33 as a
      // net when fast-init works but VAG blocks don't; else count a fast-init miss.
      bool ok = false;
      if (proto_[addr] != kProtoObd2 && kwp_.connectKwp2000(addr)) { proto_[addr] = kProto2000; ok = true; }
      else if (kwp_.connectObd2())                                 { proto_[addr] = kProtoObd2; ok = true; }
      if (ok) { fastFails_[addr] = 0; coldFails_ = 0; connected_ = true; return true; }
      if (++fastFails_[addr] >= kFastInitTries) proto_[addr] = kProto1281;   // give up on fast-init this boot
      else { connected_ = false; return false; }   // let the caller's backoff space out fast-init retries
    }
    // ECUs differ in K-line baud: this car's cluster (17) is 10400 but the
    // EDC15 engine ECU (01) answers at 9600 — at the wrong rate the 0x55 sync
    // smears into 0x95. ONE init per call, alternating baud on failure: an
    // EDC15 goes deaf for seconds after an aborted handshake, so back-to-back
    // attempts always miss. The caller's retry delay provides the quiet gap;
    // a success pins this address's baud for future reconnects.
    int baud = baud9600_[addr] ? 9600 : 10400;
    connected_ = kwp_.connect(addr, baud, idleMs);    // blocking init — off the UI core
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

  // Auto-start the deep sweep: watch actual read THROUGHPUT while the user is on a
  // gauge and, if it's abysmal, kick it off WITHOUT them finding the menu.
  // Throughput (not connect-failure count) is the right signal — it catches BOTH
  // never-connects (~0 reads) AND connects-then-drops-fast (a few reads then
  // nothing). A healthy K-line does ~3-4 reads/s (~50+ per 15s window);
  // < kMinReadsPerWindow means broken. Fires once per boot, and never if a sweep
  // is already running/resumed or has already completed (search_.done()).
  void checkThroughputAutoTune() {
    if (autoTunedThisBoot_ || search_.active() || search_.done() || !active_) return;
    uint32_t now = millis();
    if (healthWindowStart_ == 0) { healthWindowStart_ = now; healthStartReads_ = reads_; return; }
    if (now - healthWindowStart_ < kHealthWindowMs) return;
    if (reads_ - healthStartReads_ < kMinReadsPerWindow) { autoTunedThisBoot_ = true; startAutoTune(); }
    healthWindowStart_ = now; healthStartReads_ = reads_;   // start the next window
  }

  // One iteration of the deep sweep. Apply the search's current candidate timing,
  // (re)connect to the engine, and measure reads for a fixed window that starts
  // only AFTER the connection is up (so scores compare read RATE fairly, not
  // connect luck). A candidate ends when its measurement window elapses, or it's
  // scored 0 if it never connects within kConnCapMs. The score is fed back to the
  // search, progress is marked dirty for persistence, and on completion the winner
  // is applied. Runs whenever the sweep is active (engine on), regardless of screen.
  void runSearchStep() {
    uint32_t now = millis();
    if (searchApply_) {
      KwpTiming t = search_.current();
      kwp_.setTiming(t.init, t.byte, t.frame);
      if (kwp_.isConnected()) kwp_.disconnect();     // fresh connect at this candidate's timing
      candStart_ = now; candConnected_ = false; searchApply_ = false;
    }
    bool wasConn = kwp_.isConnected();
    if (ensureConnected(0x01)) {                      // engine ECU
      if (!candConnected_) { candConnected_ = true; measStart_ = now; measReads_ = reads_; measDrops_ = 0; }
      else if (!wasConn) { ++measDrops_; }            // reconnected mid-window = a drop (stability cost)
      SENSOR s[16]; int n = kwp_.readBlock(0x01, 1, 16, s); if (n > 0) ++reads_;
    } else { backoffCold(); }
    connected_ = kwp_.isConnected();
    // Fine stages measure longer for a bigger, less-noisy read count (see
    // KwpTuneSearch::inFineStage) so ms-level differences are distinguishable.
    uint32_t win = search_.inFineStage() ? kMeasWindowFineMs : kMeasWindowMs;
    bool measured = candConnected_ && (now - measStart_ >= win);
    bool gaveUp   = !candConnected_ && (now - candStart_ >= kConnCapMs);
    if (measured || gaveUp) {
      // Stability-weighted score: reward reads, but PENALISE drops so the sweep
      // prefers the timing that stays connected, not just the highest one-window
      // rate. "Best connection" = high throughput AND few dropouts.
      int reads = candConnected_ ? static_cast<int>(reads_ - measReads_) : 0;
      int score = reads - kDropPenalty * static_cast<int>(measDrops_);
      if (score < 0) score = 0;
      search_.report(score);
      searchDirty_ = true;                            // persist progress after each candidate
      if (search_.done()) {
        KwpTiming t = search_.best();
        kwp_.setTiming(t.init, t.byte, t.frame);      // apply the winner
        tuneResultReady_ = true;                       // App persists + shows result
      } else {
        searchApply_ = true;                           // set up the next candidate
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }

  void taskLoop() {
    for (;;) {
      if (serviceMode_) {                // SERVICE MODE: fully release the K-line for an external VCDS
        if (kwp_.isConnected()) kwp_.disconnect();
        connected_ = false; haveData_ = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
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
          if (clearReq_) { kwp_.clearFaultCodes(); clearReq_ = false; faultsReady_ = false; faultReq_ = true; }  // re-read right after
          else {
            uint16_t codes[16]; uint8_t infos[16];
            int nf = kwp_.readFaultCodes(codes, infos, 16);
            std::vector<Dtc> tmp;
            for (int i = 0; i < nf; ++i) {
              Dtc d; d.code = codes[i]; d.info = infos[i]; d.sporadic = (infos[i] & 0x80) != 0;
              tmp.push_back(d);
            }
            if (xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) {
              faults_ = tmp; faultsFrom_ = fe; faultsReady_ = (nf >= 0); xSemaphoreGive(mtx_);
            }
            faultReq_ = false;
          }
        } else { backoffCold(); }                  // escalating retry gap for the deaf EDC15
        continue;
      }

      if (search_.active()) { runSearchStep(); continue; }   // deep sweep owns the K-line while running

      if (scanReq_) { runScan(); continue; }

      // Default to HOLDING the engine (base ECU) whenever nothing else is being
      // read: connect it at startup and keep it alive on every screen, so a gauge
      // (or re-opening one) is instant. A diagnostics screen reading a different
      // module sets reqEcu_ to switch; leaving it (holdBase) reverts here to engine.
      uint8_t e = reqEcu_ ? reqEcu_ : baseEcu_;
      uint8_t g = reqEcu_ ? reqGroup_ : baseGroup_;
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

  void runScan() {
    String out = "engine group scan (raw formula,a,b)\n";
    for (int g = 1; g <= 255 && !serviceMode_; ++g) {
      if (!ensureConnected(kEngine)) { backoffCold(); if (!ensureConnected(kEngine)) { out += "g" + String(g) + ": no connect\n"; continue; } }
      SENSOR w[16];
      int n = kwp_.readBlock(kEngine, g, 16, w);
      if (n > 0) { ++reads_; out += kwp_.lastRaw.substring(3) + "\n"; }
      else out += "g" + String(g) + ": no data\n";
      if (xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) { scanOut_ = out + "(scanning...)\n"; xSemaphoreGive(mtx_); }
    }
    if (xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) { scanOut_ = out + "(done)\n"; xSemaphoreGive(mtx_); }
    scanReq_ = false;
  }
  volatile bool scanReq_ = false;
  String scanOut_;

  mutable KWP kwp_{static_cast<uint8_t>(cfg::PIN_KWP_RX), static_cast<uint8_t>(cfg::PIN_KWP_TX)};
  SemaphoreHandle_t mtx_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Group latest_;
  std::vector<Dtc> faults_;
  volatile uint8_t reqEcu_ = 0, reqGroup_ = 0, curEcu_ = 0, curGroup_ = 0, faultEcu_ = 0;
  // Base ECU + group held whenever nothing else is requested: the engine, from
  // startup on. The group is the SPEED group (set by App via setBaseRead) so the
  // cache always has fresh speed and opening the speedo is instant — no group-
  // switch read gap ("KWP NO DATA" flash).
  volatile uint8_t baseEcu_ = 0x01;   // engine (EDC15)
  volatile uint8_t baseGroup_ = 6;    // speed group (kSpeedGroup); keep it hot for the speedo
  volatile bool connected_ = false, haveData_ = false, stopReq_ = false;
  volatile bool active_ = false;   // a gauge is on screen (fast poll vs keep-alive)
  volatile bool serviceMode_ = false;   // K-line released for an external VCDS
  volatile uint32_t coldFails_ = 0;        // consecutive un-synced cold-connect failures
  bool autoTunedThisBoot_ = false;         // background sweep auto-fired once already
  uint32_t healthWindowStart_ = 0, healthStartReads_ = 0;   // read-throughput window
  static constexpr uint32_t kHealthWindowMs = 15000;        // window to judge throughput
  static constexpr uint32_t kMinReadsPerWindow = 8;         // < this in a window = broken -> auto-tune
  bool baud9600_[256] = {false};   // per-address: use 9600 (else 10400)
  bool baudConfirmed_[256] = {false};  // per-address: a baud has synced the magic -> stop probing
  // Per-address protocol selection: try fast-init/KWP2000 for the engine first,
  // fall back to KWP1281 if it doesn't answer after a few tries (this boot).
  static constexpr uint8_t kEngine = 0x01;
  volatile int  switchIdleMs_ = 1500;      // quiet K-line before the ENGINE after a module switch
  volatile int  switchIdleOtherMs_ = 700;  // ... before any other module (cluster) after a switch
  volatile bool sendEnd_ = false;          // send KWP1281 end-communication (0x06) on a switch
  enum { kProtoUnknown = 0, kProto1281 = 1, kProto2000 = 2, kProtoObd2 = 3 };
  static constexpr int kFastInitTries = 3;
  uint8_t proto_[256] = {kProtoUnknown};
  uint8_t fastFails_[256] = {0};
  volatile uint8_t faultsFrom_ = 0;   // module the cached faults_ list was read from
  volatile bool faultReq_ = false, clearReq_ = false, faultsReady_ = false, probeReq_ = false;
  volatile bool fastInitReq_ = false;
  volatile uint8_t fastInitEcu_ = 0x01;
  volatile int  probeRx_ = -1, probeTx_ = -1;
  volatile uint32_t reads_ = 0;

  // Auto-tuner: candidate init-pulse / inter-byte / block-delay sets to sweep.
  // Spread covers Maxi-K (init 190-210, inter-byte 4-7) and FIS-Control (50/85).
  // Deep coarse-to-fine timing search (resumable). search_ holds the algorithm +
  // persisted progress; the task measures each candidate and feeds back the score.
  KwpTuneSearch search_;
  volatile bool searchApply_ = false;    // (re)apply timing + reconnect for the current candidate
  volatile bool searchDirty_ = false;    // new progress to persist (App polls takeSearchSnapshot)
  volatile bool tuneResultReady_ = false; // sweep finished; winner ready for the App to persist
  uint32_t candStart_ = 0, measStart_ = 0, measReads_ = 0, measDrops_ = 0;
  bool candConnected_ = false;
  static constexpr uint32_t kMeasWindowMs     = 15000;  // coarse-stage measurement window
  static constexpr uint32_t kMeasWindowFineMs = 25000;  // fine stages measure longer (less noisy scores)
  static constexpr uint32_t kConnCapMs        = 8000;   // score 0 if a candidate can't connect within this
  static constexpr int      kDropPenalty      = 5;      // reads deducted per mid-window reconnect (stability)
};

} // namespace mmi
