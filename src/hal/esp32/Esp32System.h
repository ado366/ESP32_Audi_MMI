// Esp32System.h — ISystem impl: WiFi info + pull-OTA, backed by OtaManager and
// NVS-stored credentials. Lets the Settings menu show AP details and trigger a
// phone-hotspot firmware pull.
#pragma once
#include "../ISystem.h"
#include "../IStorage.h"
#include "OtaManager.h"
#include <WiFi.h>

namespace mmi {

class Esp32System : public ISystem {
public:
  Esp32System(OtaManager& ota, IStorage& storage) : ota_(ota), storage_(storage) {}

  std::string wifiInfo() override {
    std::string ssid; if (!storage_.getString("wifi.ssid", ssid)) ssid = "ESP32_MMI";
    std::string ip = WiFi.softAPIP().toString().c_str();
    return "AP " + ssid + "\nIP " + ip + "\n/status\n/bc127\n/update";
  }
  std::string updateInfo() override {
    std::string ip = WiFi.softAPIP().toString().c_str();
    std::string url; storage_.getString("ota.url", url);
    return ip + "/update (admin)\nPULL VIA HOTSPOT\n" + (url.empty() ? "NO URL SET" : url);
  }
  bool pullUpdate() override {
    // Default to the user's phone hotspot (no text-entry UI yet); NVS overrides.
    std::string ssid, pass, url;
    if (!storage_.getString("hotspot.ssid", ssid) || ssid.empty()) ssid = "OnePlus 9 PRO_8492";
    if (!storage_.getString("hotspot.pass", pass) || pass.empty()) pass = "velkakunda";
    if (!storage_.getString("ota.url", url) || url.empty()) return false; // need a published .bin URL
    return ota_.pullFromUrl(ssid, pass, url);   // reboots on success
  }

private:
  OtaManager& ota_;
  IStorage&   storage_;
};

} // namespace mmi
