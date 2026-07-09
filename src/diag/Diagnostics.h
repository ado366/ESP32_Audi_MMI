// Diagnostics.h — hardware-independent diagnostics model + source interface.
// The real KWP1281 reader (ESP32) and the fake ECU (emulator) both implement
// IDiag, so the diagnostics screens/graphs are testable without the car.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mmi {

// VAG ECU addresses (KWP1281), decimal in the UI but hex here.
namespace ecu {
constexpr uint8_t Engine    = 0x01;
constexpr uint8_t Gearbox   = 0x02;
constexpr uint8_t Abs       = 0x03;
constexpr uint8_t Dashboard = 0x17;
}

// One decoded measuring value.
struct Measurement {
  std::string label;
  float       value = 0.f;
  std::string unit;
  std::string text;   // for non-numeric readings (e.g. "WARM"); empty if numeric
  bool numeric() const { return text.empty(); }
};

// One stored fault code.
struct Dtc {
  uint16_t    code = 0;
  uint8_t     info = 0;      // secondary code
  bool        sporadic = false;
  std::string desc;          // plaintext, when available
};

// A measuring group = up to 4 values read together.
struct Group { Measurement values[4]; int count = 0; };

class IDiag {
public:
  virtual ~IDiag() = default;
  virtual bool isConnected() const = 0;
  // Read a measuring group from an ECU; returns false on comms failure.
  virtual bool readGroup(uint8_t ecuAddr, uint8_t group, Group& out) = 0;
  // Read / clear stored fault codes.
  virtual bool readFaults(uint8_t ecuAddr, std::vector<Dtc>& out) = 0;
  virtual bool clearFaults(uint8_t ecuAddr) = 0;
};

} // namespace mmi
