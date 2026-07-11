// Esp32Radio.h — IRadio over VAGFISReader. Sniffs the head-unit's 3-wire FIS
// output (radio -> ESP32) so the app can forward the radio's top-line text to the
// cluster when the head unit is NOT in "CD" mode, and take over with Bluetooth
// info when it is. Read logic mirrors the v1 sketch, which worked on this car.
#pragma once
#include "../IRadio.h"
#include "../../Config.h"
#include <VAGFISReader.h>
#include <VAGRadioRemote.h>
#include <Arduino.h>
#include <cstring>

namespace mmi {

class Esp32Radio : public IRadio {
public:
  Esp32Radio()
    : reader_(cfg::PIN_RADIO_CLK, cfg::PIN_RADIO_DATA, cfg::PIN_RADIO_ENA),
      remote_(cfg::PIN_REMOTE, -1) {}   // output-only (no incoming decode)

  void begin() { reader_.begin(); remote_.begin(); lastAck_ = millis(); }

  // ---- control output to the head unit (as in v1) ----
  void volumeUp()   override { remote_.volumeUp(); }
  void volumeDown() override { remote_.volumeDown(); }
  void tuneUp()     override { remote_.up(); }
  void tuneDown()   override { remote_.down(); }
  void sourceMode() override { remote_.mode(); }
  bool hasRemote()  const override { return true; }

  void poll() override {
    if (reader_.hasNewMsg()) {
      uint8_t data[24]; int n = 0;
      int size = reader_.getSize();
      // Skip header/checksum bytes exactly as v1 did, per message type.
      if (reader_.msgIsNavi()) {
        if (reader_.msgIsRadioText())      for (int i = 3; i < size - 1 && n < (int)sizeof(data); i++) data[n++] = reader_.readData(i);
        else if (reader_.msgIsText())      for (int i = 5; i < size - 1 && n < (int)sizeof(data); i++) data[n++] = reader_.readData(i);
        // graphics in navi mode: ignored (we only forward the text lines)
      } else {                              // pure radio mode
        for (int i = 1; i < size - 1 && n < (int)sizeof(data); i++) data[n++] = reader_.readData(i);
      }
      if (n > 0) apply(data, n);
      reader_.clearNewMsgFlag();
    }
    // The cluster normally ACKs the radio; since we sit in between, we must ACK so
    // the head unit keeps sending. v1 used a ~100ms cadence.
    if ((uint32_t)(millis() - lastAck_) > 100) { reader_.ACK(); lastAck_ = millis(); }
  }

  bool cdMode() const override { return cdMode_; }
  const char* line1() const override { return line1_; }
  const char* line2() const override { return line2_; }
  bool consumeChanged() override { bool c = changed_; changed_ = false; return c; }

private:
  void apply(const uint8_t* d, int n) {
    // Any line starting with "CD" means our aux source is selected — this also
    // covers the "CD" error/status messages (e.g. "CD ERR"), so we keep showing
    // Bluetooth info rather than passing the error text through.
    bool cd = (n >= 2 && d[0] == 'C' && d[1] == 'D');
    char l1[9] = {0}, l2[9] = {0};
    for (int i = 0; i < 8 && i < n; i++)      l1[i]     = (char)d[i];   // radio text is already ASCII
    for (int i = 8; i < 16 && i < n; i++)     l2[i - 8] = (char)d[i];
    if (cd != cdMode_ || strncmp(l1, line1_, 8) != 0 || strncmp(l2, line2_, 8) != 0) changed_ = true;
    cdMode_ = cd;
    memcpy(line1_, l1, 9); memcpy(line2_, l2, 9);
  }

  VAGFISReader reader_;
  VAGRadioRemote remote_;
  char line1_[9] = {0}, line2_[9] = {0};
  bool cdMode_ = false, changed_ = false;
  uint32_t lastAck_ = 0;
};

} // namespace mmi
