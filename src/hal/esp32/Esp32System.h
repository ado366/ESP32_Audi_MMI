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

  // The display fits ~15 compressed chars per row, so every line here must stay
  // short: labels and IPs go on SEPARATE lines (a "HOME 192.168.0.219" one-liner
  // was 18 chars and got cut off on the cluster).
  std::string wifiInfo() override {
    std::string out;
    if (WiFi.status() == WL_CONNECTED) {
      // Joined a network as a client — say WHICH KIND by matching the SSID
      // against the provisioned credentials.
      std::string ss = WiFi.SSID().c_str();
      std::string hs, ns;
      storage_.getString("hotspot.ssid", hs);
      storage_.getString("home.ssid", ns);
      const char* kind = (!hs.empty() && ss == hs) ? "ON HOTSPOT"
                       : (!ns.empty() && ss == ns) ? "ON HOME NET"
                                                   : "ON WIFI";
      if (ss.size() > 15) ss.resize(15);
      out += std::string(kind) + "\n" + ss + "\n " +
             WiFi.localIP().toString().c_str() + "\n";
    } else {
      out += "NOT JOINED\n";
    }
    std::string ap; if (!storage_.getString("wifi.ssid", ap)) ap = "ESP32_MMI";
    if (ap.size() > 12) ap.resize(12);
    char cl[20]; std::snprintf(cl, sizeof(cl), "AP %s +%d", ap.c_str(), WiFi.softAPgetStationNum());
    out += std::string(cl) + "\n " + WiFi.softAPIP().toString().c_str();
    return out;
  }
  std::string updateInfo() override {
    // FW version + where /update is reachable (client IP when joined, always
    // the AP IP), then the pull hint. Each line <= 15 chars.
    std::string out = std::string("FW ") + cfg::FW_VERSION + "\nUPLOAD /UPDATE\n";
    if (WiFi.status() == WL_CONNECTED)
      out += std::string(" ") + WiFi.localIP().toString().c_str() + "\n";
    out += std::string(" ") + WiFi.softAPIP().toString().c_str();
    return out;
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
