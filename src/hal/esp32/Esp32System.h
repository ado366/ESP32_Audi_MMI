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
    return std::string("FW ") + cfg::FW_VERSION + "\n" + ip + "/update (admin)\nSEL=PULL VIA HOTSPOT\nGITHUB LATEST";
  }
  bool pullUpdate() override {
    // Try the phone hotspot first, then the home/local network (NVS overrides the
    // Config defaults). No text-entry UI yet, so credentials come from those.
    std::string url;
    if (!storage_.getString("ota.url", url) || url.empty()) url = cfg::OTA_URL;

    std::string hs, hp, ns, np;
    if (!storage_.getString("hotspot.ssid", hs) || hs.empty()) hs = cfg::HOTSPOT_SSID;
    if (!storage_.getString("hotspot.pass", hp) || hp.empty()) hp = cfg::HOTSPOT_PASS;
    if (!storage_.getString("home.ssid", ns) || ns.empty()) ns = cfg::HOME_SSID;
    if (!storage_.getString("home.pass", np) || np.empty()) np = cfg::HOME_PASS;

    std::vector<std::pair<std::string, std::string>> nets;
    nets.push_back({hs, hp});
    if (!ns.empty()) nets.push_back({ns, np});
    return ota_.pullFromUrl(nets, url);   // reboots on success
  }

private:
  OtaManager& ota_;
  IStorage&   storage_;
};

} // namespace mmi
