// FakeDiag.h — synthetic ECU for the emulator. Produces plausible, time-varying
// measuring values so the diagnostics screens, display modes and graphs can be
// developed without the car.
#pragma once
#include "../../diag/Diagnostics.h"
#include <cmath>

namespace mmi {

class FakeDiag : public IDiag {
public:
  bool isConnected() const override { return true; }

  bool readGroup(uint8_t ecuAddr, uint8_t group, Group& out) override {
    ++t_;
    float s = std::sin(t_ * 0.15f), s2 = std::sin(t_ * 0.07f + 1.f);
    out.count = 0;
    auto add = [&](const char* label, float v, const char* unit) {
      if (out.count < 4) { out.values[out.count++] = Measurement{label, v, unit, ""}; }
    };
    if (ecuAddr == ecu::Engine && group == 2) {
      add("RPM",  2000 + 1500 * s, "rpm");
      add("LOAD", 45 + 30 * s2,    "%");
      add("INJ",  3.2f + 1.1f * s, "ms");
      add("MAF",  120 + 60 * s2,   "g/s");
    } else if (ecuAddr == ecu::Engine && group == 115) {
      add("RPM",   2000 + 1500 * s, "rpm");
      add("LOAD",  50 + 35 * s2,    "%");
      add("BOOSTD",1.4f + 0.3f * s2,"bar");
      add("BOOSTA",1.4f + 0.3f * s, "bar");
    } else if (ecuAddr == ecu::Dashboard && group == 2) {
      add("ODO",   231480,        "km");
      add("FUEL",  38 + 2 * s,     "l");
      add("TAMB",  25 + 1 * s2,    "C");
      add("COOL",  88 + 4 * s,     "C");
    } else {
      add("V1", 100 * s, " ");
      add("V2", 50 * s2, " ");
    }
    return true;
  }

  bool readFaults(uint8_t, std::vector<Dtc>& out) override {
    if (faultsCleared_) { out.clear(); return true; }
    out = {                                  // EDC15 TDI-style codes (resolved via DtcDescriptions)
      {575, 0x00, false, ""},                // Intake Manifold Pressure
      {553, 0x80, true,  ""},                // Mass Air Flow (sporadic)
    };
    return true;
  }
  bool clearFaults(uint8_t) override { faultsCleared_ = true; return true; }
  bool faultsCleared() const { return faultsCleared_; }

private:
  uint32_t t_ = 0;
  bool faultsCleared_ = false;
};

} // namespace mmi
