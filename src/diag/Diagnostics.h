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

// Pickable KWP1281 modules on a B5 (Airbag 0x15 deliberately excluded — unsafe).
struct Module { uint8_t addr; const char* name; };
static const Module kModules[] = {
  {0x01, "ENGINE"},
  {0x02, "GEARBOX"},
  {0x03, "ABS"},
  {0x08, "CLIMATE"},
  {0x17, "CLUSTER"},
  {0x25, "IMMOBILIZER"},
  {0x35, "CENT CONV"},
  {0x37, "NAVIGATION"},
  {0x45, "INT MONITOR"},
  {0x56, "RADIO"},
};
static constexpr int kModuleCount = sizeof(kModules) / sizeof(kModules[0]);
inline const char* moduleName(uint8_t addr) {
  for (int i = 0; i < kModuleCount; ++i) if (kModules[i].addr == addr) return kModules[i].name;
  return "MODULE";
}
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
  // Per-vehicle KWP1281 timing (Maxi-K Adaptation). Default no-op (emulator).
  virtual void setTiming(int initBitMs, int interByteMs, int blockDelayMs) {}
  // No diag screen is showing: stop polling and drop the K-line connection —
  // otherwise the task keeps reading/reconnecting in the background forever
  // (observed churning during a phone call). Default no-op (emulator).
  virtual void stopReading() {}
};

} // namespace mmi
