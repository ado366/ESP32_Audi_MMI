// Diagnostics.h — hardware-independent diagnostics model + source interface.
// The real KWP1281 reader (ESP32) and the fake ECU (emulator) both implement
// IDiag, so the diagnostics screens/graphs are testable without the car.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "KwpTuneSearch.h"

namespace mmi {

// VAG ECU addresses (KWP1281), decimal in the UI but hex here.
namespace ecu {
constexpr uint8_t Engine    = 0x01;
constexpr uint8_t Gearbox   = 0x02;
constexpr uint8_t Abs       = 0x03;
constexpr uint8_t Airbag    = 0x15;
constexpr uint8_t Dashboard = 0x17;

// Pickable KWP1281 modules on a B5. AIRBAG fault clearing asks for a second
// press (App DiagFaults) — clearing it is deliberate, never a stray click.
struct Module { uint8_t addr; const char* name; };
static const Module kModules[] = {
  {0x01, "ENGINE"},
  {0x02, "GEARBOX"},
  {0x03, "ABS"},
  {0x08, "CLIMATE"},
  {0x15, "AIRBAG"},
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
  // Service mode: fully release the K-line (disconnect and stay off it) so an
  // external tester (VCDS) can reach every module without bus contention.
  virtual void setServiceMode(bool on) {}
  virtual bool serviceMode() const { return false; }
  // Revert to holding the base ECU (engine) without disconnecting — used on
  // non-diagnostics screens to keep the engine session alive across the whole UI.
  virtual void holdBase() {}
  // Point the base keep-alive at a given ecu/group (App sets the speed group so
  // the speedo cache is always warm — opening it is instant, no group-switch gap).
  virtual void setBaseRead(uint8_t ecu, uint8_t group) {}
  // "Connect once and forget": true while a gauge is on screen (poll fast), false
  // otherwise (slow keep-alive that HOLDS the engine session instead of dropping
  // it, so re-opening a gauge is instant rather than a ~15-20s cold reconnect).
  // Default no-op (emulator).
  virtual void setActive(bool active) {}

  // ---- KWP timing auto-tuner ----
  // Sweep a set of init-pulse / inter-byte / block-delay candidates on the live
  // K-line, scoring each by reads-per-window, and pick the most reliable. Runs in
  // the background task; the engine must be running. Default no-ops (emulator).
  virtual void startAutoTune() {}
  virtual bool autoTuning() const { return false; }
  virtual std::string autoTuneStatus() const { return ""; }   // one-line progress for display/telemetry
  // True exactly once when a run finishes, filling the winning timing (so the
  // caller can apply + persist it). Subsequent calls return false.
  virtual bool takeAutoTuneResult(int& initMs, int& byteMs, int& frameMs) { return false; }

  // ---- resumable deep sweep persistence (see KwpTuneSearch) ----
  // The deep sweep spans several drives, so its progress is saved to NVS by the
  // App layer. On boot the App restores it via resumeSearch(); after each
  // candidate the source offers a fresh snapshot to persist via takeSearchSnapshot
  // (true when there's new state). Default no-ops (emulator / no persistence).
  virtual void resumeSearch(const KwpTuneSearch::State&) {}
  virtual bool takeSearchSnapshot(KwpTuneSearch::State&) { return false; }
};

} // namespace mmi
