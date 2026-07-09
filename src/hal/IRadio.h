// IRadio.h — the head-unit display sniffer. On real hardware this reads the
// radio's 3-wire output (VAGFISReader); in the emulator it's scriptable.
//
// The head unit normally drives the FIS directly. The ESP32 sits in between, so
// it must forward the radio's text to the FIS when the radio is the active
// source, and only take over (with Bluetooth info) when the head unit is in "CD"
// mode (i.e. our aftermarket source is selected).
#pragma once

namespace mmi {

class IRadio {
public:
  virtual ~IRadio() = default;
  virtual void poll() {}
  // True when the head unit shows "CD" (our BT source active) -> ESP32 takes over.
  // False -> radio/tuner active -> forward the radio's text. Default true so a
  // build with no radio wired just shows the Bluetooth screen.
  virtual bool cdMode() const { return true; }
  virtual const char* line1() const { return ""; } // forwarded radio text
  virtual const char* line2() const { return ""; }
  // Returns true once after the text or mode changed (so the UI re-renders).
  virtual bool consumeChanged() { return false; }
};

} // namespace mmi
