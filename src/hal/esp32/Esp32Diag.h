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

namespace mmi {

class Esp32Diag : public IDiag {
public:
  void begin() {
    mtx_ = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(&Esp32Diag::thunk, "kwp", 4096, this, 1, &task_, 0);
  }

  bool isConnected() const override { return connected_; }
  std::string kwpDebug() const { return std::string(kwp_.dbg.c_str()); }  // connect-flow trace
  uint32_t readCount() const { return reads_; }                            // total measuring-block reads
  void requestProbe(int rx = -1, int tx = -1) { probeRx_ = rx; probeTx_ = tx; probeReq_ = true; }
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

private:
  static void thunk(void* p) { static_cast<Esp32Diag*>(p)->taskLoop(); }

  bool ensureConnected(uint8_t addr) {
    if (kwp_.isConnected() && kwp_.getCurrAddr() == addr) { connected_ = true; return true; }
    if (kwp_.isConnected()) kwp_.disconnect();
    connected_ = kwp_.connect(addr, 10400);   // blocking init — off the UI core
    return connected_;
  }

  void taskLoop() {
    for (;;) {
      if (probeReq_) { probeReq_ = false; if (kwp_.isConnected()) kwp_.disconnect(); kwp_.probe(probeRx_, probeTx_); continue; }
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
        } else { vTaskDelay(pdMS_TO_TICKS(500)); }
        continue;
      }

      uint8_t e = reqEcu_, g = reqGroup_;
      if (e == 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
      if (!ensureConnected(e)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

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
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }

  mutable KWP kwp_{static_cast<uint8_t>(cfg::PIN_KWP_RX), static_cast<uint8_t>(cfg::PIN_KWP_TX)};
  SemaphoreHandle_t mtx_ = nullptr;
  TaskHandle_t task_ = nullptr;
  Group latest_;
  std::vector<Dtc> faults_;
  volatile uint8_t reqEcu_ = 0, reqGroup_ = 0, curEcu_ = 0, curGroup_ = 0, faultEcu_ = 0;
  volatile bool connected_ = false, haveData_ = false;
  volatile bool faultReq_ = false, clearReq_ = false, faultsReady_ = false, probeReq_ = false;
  volatile int  probeRx_ = -1, probeTx_ = -1;
  volatile uint32_t reads_ = 0;
};

} // namespace mmi
