// OtaManager.h — secured WiFi + dual-path OTA (plan §6), no external deps.
// Uses the ESP32 core's WiFi / WebServer / Update / HTTPClient.
//  - Access point is password-protected; the browser upload page needs basic auth.
//  - pullFromUrl() joins the phone hotspot (STA) and flashes a published .bin.
#pragma once
#include "../../Config.h"
#include "../../ui/WebUi.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <string>
#include <functional>
#include <vector>
#include <utility>

namespace mmi {

class OtaManager {
public:
  void beginAp(const std::string& ssid, const std::string& pass,
               const std::string& user, const std::string& otaPass,
               const std::string& staSsid = "", const std::string& staPass = "") {
    user_ = user; otaPass_ = otaPass;
    // AP always up (192.168.4.1); also join the home network so the UI is
    // reachable from a computer on that network (in the car the join just fails).
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ssid.c_str(), pass.c_str());   // WPA2; pass must be >= 8 chars
    if (!staSsid.empty()) WiFi.begin(staSsid.c_str(), staPass.c_str());
    MDNS.begin("audimmi");                     // http://audimmi.local
    MDNS.addService("http", "tcp", 80);

    server_.on("/", HTTP_GET, [this]() {
      if (!authed()) return;
      server_.send(200, "text/html",
        "<h3>Audi MMI</h3><ul>"
        "<li><a href='/control'>Control UI (remote buttons + display)</a></li>"
        "<li><a href='/update'>Update firmware</a></li>"
        "<li><a href='/status'>Live status (JSON)</a></li>"
        "<li><a href='/bc127'>BC127 console</a></li></ul>");
    });
    // Browser control UI: page + display-frame state + input injection.
    server_.on("/control", HTTP_GET, [this]() { server_.send(200, "text/html", kWebUiPage); });
    server_.on("/state",   HTTP_GET, [this]() { server_.send(200, "application/json", ctrlState_ ? ctrlState_().c_str() : "{}"); });
    server_.on("/input",   HTTP_POST, [this]() {
      // Handle both form-encoded (parsed into args) and text/plain bodies.
      std::string cs, ds;
      if (server_.hasArg("c")) { cs = server_.arg("c").c_str(); ds = server_.arg("d").c_str(); }
      else if (server_.hasArg("plain")) {
        std::string body = server_.arg("plain").c_str();
        cs = webField(body, "c"); ds = webField(body, "d");
      }
      Control c = webControlFromName(cs);
      int d = atoi(ds.c_str());
      if (ctrlSink_ && c != Control::None) ctrlSink_(c, d);
      server_.send(200, "text/plain", "ok");
    });
    // Live status JSON (behind the AP password; safe telemetry only).
    server_.on("/status", HTTP_GET, [this]() {
      server_.send(200, "application/json", statusFn_ ? statusFn_().c_str() : "{}");
    });
    // KWP connect-flow trace (plain text) for on-car diagnostics debugging.
    server_.on("/kwpdbg", HTTP_GET, [this]() {
      server_.send(200, "text/plain", kwpLog_ ? kwpLog_().c_str() : "");
    });
    // Trigger a K-line loopback self-test; result appears at /kwpdbg. Optional
    // ?rx=<pin>&tx=<pin> tests other candidate pins (default = configured 5/0).
    server_.on("/kwpprobe", HTTP_GET, [this]() {
      int rx = server_.hasArg("rx") ? server_.arg("rx").toInt() : -1;
      int tx = server_.hasArg("tx") ? server_.arg("tx").toInt() : -1;
      if (kwpProbe_) kwpProbe_(rx, tx);
      server_.send(200, "text/plain", "probing K-line; read /kwpdbg in ~1s");
    });
    // Trigger a KWP connect+read of ?ecu=<hex>&grp=<n> (debug); trace at /kwpdbg.
    server_.on("/kwptest", HTTP_GET, [this]() {
      int ecu = strtol(server_.arg("ecu").c_str(), nullptr, 16);
      int grp = server_.hasArg("grp") ? server_.arg("grp").toInt() : 1;
      if (kwpTest_) kwpTest_((uint8_t)ecu, (uint8_t)grp);
      server_.send(200, "text/plain", "connecting; read /kwpdbg");
    });
    // BC127 raw command console: send a command, watch recent module traffic.
    server_.on("/bc127", HTTP_GET, [this]() {
      if (!authed()) return;
      std::string h = "<h3>BC127 console</h3>"
        "<form method='POST' action='/bc127'>"
        "<input name='cmd' size='28' autofocus placeholder='e.g. STATUS'>"
        "<input type='submit' value='Send'></form>"
        "<pre style='background:#111;color:#0f0;padding:8px'>";
      h += bcLog_ ? bcLog_() : "";
      h += "</pre><meta http-equiv='refresh' content='2'>";
      server_.send(200, "text/html", h.c_str());
    });
    server_.on("/bc127", HTTP_POST, [this]() {
      if (!authed()) return;
      if (bcSend_ && server_.hasArg("cmd")) bcSend_(std::string(server_.arg("cmd").c_str()));
      server_.sendHeader("Location", "/bc127");
      server_.send(303);
    });
    server_.on("/update", HTTP_GET, [this]() {
      if (!authed()) return;
      server_.send(200, "text/html",
        "<form method='POST' action='/update' enctype='multipart/form-data'>"
        "<input type='file' name='fw'><input type='submit' value='Flash'></form>");
    });
    server_.on("/update", HTTP_POST,
      [this]() {                                   // on complete
        if (!authed()) return;
        server_.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK, rebooting");
        delay(400); ESP.restart();
      },
      [this]() { handleUpload(); });               // on each chunk

    server_.begin();
    apUp_ = true;
  }

  // Debug hooks: live status JSON + a raw BC127 command console.
  void setStatusProvider(std::function<std::string()> f) { statusFn_ = std::move(f); }
  void setBc127Console(std::function<void(const std::string&)> send,
                       std::function<std::string()> log) { bcSend_ = std::move(send); bcLog_ = std::move(log); }
  void setKwpLog(std::function<std::string()> f) { kwpLog_ = std::move(f); }
  void setKwpProbe(std::function<void(int,int)> f) { kwpProbe_ = std::move(f); }
  void setKwpTest(std::function<void(uint8_t,uint8_t)> f) { kwpTest_ = std::move(f); }
  // Browser control/debug UI: state = display frame + BT json; sink injects inputs.
  void setControlHooks(std::function<std::string()> state,
                       std::function<void(Control, int)> sink) { ctrlState_ = std::move(state); ctrlSink_ = std::move(sink); }

  void handle() { if (apUp_) server_.handleClient(); }
  // Home-network IP once joined ("0.0.0.0" if not connected).
  std::string staIP() const { return WiFi.localIP().toString().c_str(); }

  // Try each (ssid,pass) network in turn; on the first that connects, flash the
  // .bin from `url`. Returns false if none connect or the download fails (on
  // success it reboots and does not return).
  bool pullFromUrl(const std::vector<std::pair<std::string, std::string>>& nets, const std::string& url) {
    WiFi.mode(WIFI_AP_STA);
    for (const auto& n : nets) {
      if (n.first.empty()) continue;
      WiFi.begin(n.first.c_str(), n.second.c_str());
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(200);
      if (WiFi.status() == WL_CONNECTED) return flash(url);
      WiFi.disconnect();
    }
    return false;
  }
  // Backward-compatible single-network form.
  bool pullFromUrl(const std::string& ssid, const std::string& pass, const std::string& url) {
    return pullFromUrl({{ssid, pass}}, url);
  }

private:
  // Download `url` and flash. Handles GitHub's HTTPS + 302 redirect to its CDN.
  bool flash(const std::string& url) {
    WiFiClientSecure client; client.setInsecure();   // GitHub release CDN is HTTPS
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(client, url.c_str())) return false;
    int code = http.GET();
    if (code != HTTP_CODE_OK) { http.end(); return false; }
    int len = http.getSize();
    if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) { http.end(); return false; }
    size_t written = Update.writeStream(http.getStream());
    bool ok = Update.end(true) && (len <= 0 || (int)written == len);
    http.end();
    if (ok) { delay(400); ESP.restart(); }
    return ok;
  }

  bool authed() {
    if (server_.authenticate(user_.c_str(), otaPass_.c_str())) return true;
    server_.requestAuthentication();
    return false;
  }
  void handleUpload() {
    HTTPUpload& up = server_.upload();
    if (up.status == UPLOAD_FILE_START)       { Update.begin(UPDATE_SIZE_UNKNOWN); }
    else if (up.status == UPLOAD_FILE_WRITE)  { Update.write(up.buf, up.currentSize); }
    else if (up.status == UPLOAD_FILE_END)    { Update.end(true); }
  }

  WebServer   server_{80};
  std::string user_, otaPass_;
  bool        apUp_ = false;
  std::function<std::string()> statusFn_;
  std::function<void(const std::string&)> bcSend_;
  std::function<std::string()> bcLog_;
  std::function<std::string()> kwpLog_;
  std::function<void(int,int)> kwpProbe_;
  std::function<void(uint8_t,uint8_t)> kwpTest_;
  std::function<std::string()> ctrlState_;
  std::function<void(Control, int)> ctrlSink_;
};

} // namespace mmi
