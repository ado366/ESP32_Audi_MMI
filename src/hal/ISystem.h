// ISystem.h — optional platform hook for actions the hardware-agnostic App
// can't do itself (WiFi info, firmware update). Null on native; the ESP32
// wires it to OtaManager + storage.
#pragma once
#include <string>

namespace mmi {

class ISystem {
public:
  virtual ~ISystem() = default;

  // Human-readable AP details for the WiFi info screen (use '\n' for lines).
  virtual std::string wifiInfo() = 0;
  // Where/how to upload firmware (browser page + auth), for the Update screen.
  virtual std::string updateInfo() = 0;
  // Trigger a pull-OTA (join phone hotspot, download+flash). Returns false if it
  // couldn't start (e.g. no hotspot/URL configured). On success the device reboots.
  virtual bool pullUpdate() = 0;
};

} // namespace mmi
