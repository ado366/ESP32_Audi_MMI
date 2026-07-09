// IBluetooth.h — abstraction over the BC127 module.
// esp32 impl talks UART1; native impl is the scriptable fake BC127 in the emulator.
#pragma once
#include <cstdint>
#include <string>
#include <functional>

namespace mmi {

enum class CallState : uint8_t { Idle, Incoming, Outgoing, Active };

struct BtStatus {
  bool        linked = false;
  std::string activeDeviceName;
  std::string activeDeviceMac;
  bool        playing = false;
  std::string title;        // AVRCP track metadata
  std::string artist;
  CallState   call = CallState::Idle;
  std::string callerNumber;
  std::string callerName;   // resolved via phonebook when available
  bool        scoOpen = false; // HFP audio path up (used by mic test)
};

// One phonebook entry (PBAP).
struct Contact { std::string name; std::string number; };

class IBluetooth {
public:
  virtual ~IBluetooth() = default;

  virtual void begin() = 0;
  virtual void poll() = 0;              // pump incoming events -> status/callbacks
  virtual const BtStatus& status() const = 0;

  // Send a raw BC127 command line (without trailing CR).
  virtual void sendCommand(const std::string& line) = 0;

  // Media
  virtual void playPause() = 0;
  virtual void trackNext() = 0;
  virtual void trackPrev() = 0;
  virtual void volumeUp() = 0;
  virtual void volumeDown() = 0;

  // Call control
  virtual void callAnswer() = 0;
  virtual void callReject() = 0;
  virtual void callEnd() = 0;
  virtual void dial(const std::string& number) = 0;

  // Single-active-device management
  virtual void connectDevice(const std::string& mac) = 0; // disconnects others
  virtual void disconnectActive() = 0;
  // Enforce only one connected device (closes extras, last-used kept). Default off.
  virtual void setSingleDevice(bool enabled) { (void)enabled; }

  // Mic test: open SCO + mic-to-speaker loopback/sidetone; set gain 0..15.
  virtual void micLoopback(bool on) = 0;
  virtual void setMicGain(uint8_t gain) = 0;

  // Fired when the status materially changes (link/call/metadata).
  virtual void onStatusChanged(std::function<void()> cb) = 0;

  // Recent module traffic (sent/received lines) for the BC127 debug view.
  virtual std::string debugLog() const { return ""; }
};

} // namespace mmi
