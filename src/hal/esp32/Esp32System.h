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
    std::string apip = WiFi.softAPIP().toString().c_str();
    std::string home = ota_.staIP();
    return "AP " + ssid + "\nAP IP " + apip +
           "\nHOME " + (home == "0.0.0.0" ? std::string("-") : home) +
           "\nAUDIMMI.LOCAL\n/control";
  }
  std::string updateInfo() override {
    std::string ip = WiFi.softAPIP().toString().c_str();
    return std::string("FW ") + cfg::FW_VERSION + "\n" + ip + "/update (admin)\nSEL=PULL VIA HOTSPOT\nGITHUB LATEST";
  }
  bool pullUpdate() override {
    // Credentials come ONLY from NVS (written by a USB provisioning flash), so
    // the published OTA binary carries no WiFi passwords.
    std::string url;
    if (!storage_.getString("ota.url", url) || url.empty()) url = cfg::OTA_URL;

    std::string hs, hp, ns, np;
    storage_.getString("hotspot.ssid", hs); storage_.getString("hotspot.pass", hp);
    storage_.getString("home.ssid", ns);    storage_.getString("home.pass", np);

    std::vector<std::pair<std::string, std::string>> nets;
    if (!hs.empty()) nets.push_back({hs, hp});
    if (!ns.empty()) nets.push_back({ns, np});
    if (nets.empty()) return false;       // not provisioned — USB-flash first
    return ota_.pullFromUrl(nets, url);   // reboots on success
  }

private:
  OtaManager& ota_;
  IStorage&   storage_;
};

} // namespace mmi
