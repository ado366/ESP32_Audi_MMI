// Esp32System.h — ISystem impl: WiFi info + pull-OTA, backed by OtaManager and
// NVS-stored credentials. Lets the Settings menu show AP details and trigger a
// phone-hotspot firmware pull.
#pragma once
#include "../ISystem.h"
#include "../IStorage.h"
#include "OtaManager.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

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
  // Pull-update runs on its own task so the UI loop keeps rendering progress:
  // check GitHub's latest release tag -> (if newer, or forced by a 2nd press
  // after "UP TO DATE") join a saved network -> download with % -> flash ->
  // reboot. Every stage/error is reported via updateStatus().
  bool pullUpdate() override {
    if (busy_) return true;                         // already running
    if (!mtx_) mtx_ = xSemaphoreCreateMutex();
    force_ = upToDate_;                             // 2nd press after UP TO DATE = install anyway
    upToDate_ = false;
    busy_ = true;
    setStatus("STARTING...");
    if (xTaskCreatePinnedToCore(&Esp32System::thunk, "ota", 12288, this, 1, nullptr, 0) != pdPASS) {
      busy_ = false; setStatus("ERROR\nNO MEMORY"); return false;
    }
    return true;
  }
  std::string updateStatus() override {
    std::string r;
    if (mtx_ && xSemaphoreTake(mtx_, pdMS_TO_TICKS(5)) == pdTRUE) { r = status_; xSemaphoreGive(mtx_); }
    return r;
  }
  bool updating() override { return busy_; }

private:
  static void thunk(void* p) { static_cast<Esp32System*>(p)->run(); vTaskDelete(nullptr); }

  void setStatus(const std::string& st) {
    if (mtx_ && xSemaphoreTake(mtx_, portMAX_DELAY) == pdTRUE) { status_ = st; xSemaphoreGive(mtx_); }
  }
  void fail(const std::string& why) { setStatus("FAILED\n" + why + "\nSEL=RETRY"); ota_.pauseRoaming(false); busy_ = false; }

  // GitHub API: tag_name of the latest release ("v2.9.51" -> "2.9.51"), "" on error.
  std::string latestTag() {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    if (!http.begin(client, cfg::OTA_API_URL)) return "";
    http.addHeader("User-Agent", "ESP32-Audi-MMI");   // GitHub API requires a UA
    int code = http.GET();
    std::string body = code == HTTP_CODE_OK ? std::string(http.getString().c_str()) : "";
    http.end();
    size_t p = body.find("\"tag_name\"");
    if (p == std::string::npos) return "";
    p = body.find('"', body.find(':', p) + 1);
    size_t e = body.find('"', p + 1);
    if (p == std::string::npos || e == std::string::npos) return "";
    std::string t = body.substr(p + 1, e - p - 1);
    if (!t.empty() && (t[0] == 'v' || t[0] == 'V')) t.erase(0, 1);
    return t;
  }

  void run() {
    std::string url;
    if (!storage_.getString("ota.url", url) || url.empty()) url = cfg::OTA_URL;
    const std::string have = std::string("HAVE V") + cfg::FW_VERSION;

    // 1) WiFi: reuse an existing station link, else try the saved networks.
    if (WiFi.status() != WL_CONNECTED) {
      std::string hs, hp, ns, np;
      storage_.getString("hotspot.ssid", hs); storage_.getString("hotspot.pass", hp);
      storage_.getString("home.ssid", ns);    storage_.getString("home.pass", np);
      std::vector<std::pair<std::string, std::string>> nets;
      if (!hs.empty()) nets.push_back({hs, hp});
      if (!ns.empty()) nets.push_back({ns, np});
      if (nets.empty()) { fail("NO WIFI SAVED"); return; }
      ota_.pauseRoaming(true);
      WiFi.mode(WIFI_AP_STA);
      bool ok = false;
      for (const auto& n : nets) {
        std::string ss = n.first.substr(0, 15);
        setStatus("JOINING WIFI\n" + ss);
        WiFi.begin(n.first.c_str(), n.second.c_str());
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) vTaskDelay(pdMS_TO_TICKS(200));
        if (WiFi.status() == WL_CONNECTED) { ok = true; break; }
        WiFi.disconnect();
      }
      if (!ok) { fail("NO WIFI FOUND"); return; }
    }

    // 2) Version check against GitHub's latest release.
    setStatus("CHECKING GITHUB\n" + have);
    std::string latest = latestTag();
    if (latest.empty()) setStatus("VERSION UNKNOWN\n" + have);
    else if (latest == cfg::FW_VERSION && !force_) {
      setStatus("UP TO DATE\nLATEST V" + latest + "\n" + have + "\nSEL=REINSTALL");
      upToDate_ = true; ota_.pauseRoaming(false); busy_ = false; return;
    }
    const std::string lat = latest.empty() ? std::string("LATEST ?") : "LATEST V" + latest;

    // 3) Download + flash with live progress.
    std::string err;
    bool ok = ota_.flashWithProgress(url, [&](int pct, size_t done, size_t total) {
      char l[40];
      if (total) std::snprintf(l, sizeof(l), "DOWNLOADING %d%%\n%u/%uKB", pct, (unsigned)(done / 1024), (unsigned)(total / 1024));
      else       std::snprintf(l, sizeof(l), "DOWNLOADING\n%uKB", (unsigned)(done / 1024));
      setStatus(std::string(l) + "\n" + lat + "\n" + have);
    }, err);
    if (!ok) { fail(err); return; }
    setStatus("UPDATE OK\n" + lat + "\nREBOOTING...");
    vTaskDelay(pdMS_TO_TICKS(1500));                // let the cluster show it
    ESP.restart();
  }

  OtaManager& ota_;
  IStorage&   storage_;
  SemaphoreHandle_t mtx_ = nullptr;
  std::string status_;
  volatile bool busy_ = false, force_ = false, upToDate_ = false;
};

} // namespace mmi
