// main_esp32.cpp — firmware entry point: runs the real App on hardware.
#include <Arduino.h>
#include "App.h"
#include "hal/esp32/Esp32Display.h"
#include "hal/esp32/Esp32Inputs.h"
#include "hal/esp32/Esp32Bluetooth.h"
#include "hal/esp32/Esp32Storage.h"
#include "hal/esp32/Esp32Diag.h"
#include "hal/esp32/OtaManager.h"
#include "hal/esp32/Esp32System.h"
#include "hal/esp32/Esp32Radio.h"
#include "hal/esp32/FaultDb.h"
#ifdef MMI_PROVISION
#include "Secrets.h"   // real WiFi creds — only in the USB provisioning build
#endif

using namespace mmi;

// Write baked WiFi credentials into NVS once (USB provisioning build only).
// NVS survives OTA, so later credential-free OTA builds keep working.
static void provisionCredentials(Esp32Storage& s) {
#ifdef MMI_PROVISION
  std::string tmp;
  if (!s.getString("hotspot.ssid", tmp)) {
    s.putString("hotspot.ssid", cfg::HOTSPOT_SSID);
    s.putString("hotspot.pass", cfg::HOTSPOT_PASS);
  }
  if (!s.getString("home.ssid", tmp) && std::string(cfg::HOME_SSID).size()) {
    s.putString("home.ssid", cfg::HOME_SSID);
    s.putString("home.pass", cfg::HOME_PASS);
  }
  Serial.println(F("provisioned WiFi credentials to NVS"));
#else
  (void)s;
#endif
}

static Esp32Storage   storage;
static Esp32Display   display;
static Esp32Inputs    inputs;
static Esp32Bluetooth bt;
static Esp32Diag      diag;
static OtaManager     ota;
static Esp32Radio     radio;
static App*           app = nullptr;

void setup() {
  Serial.begin(115200);
  Serial.println(F("ESP32 Audi MMI v2 — boot"));

  storage.begin();
  provisionCredentials(storage);   // USB build writes creds to NVS; OTA build no-op
  display.begin();
  inputs.begin();
  inputs.attachStorage(&storage);
  inputs.loadThresholds(storage);  // apply calibrated button thresholds if saved
  // Enter button-learning ONLY on a deliberate gesture: encoder button held at
  // boot. (Also reachable any time via encoder long-press -> Debug -> Calibrate.)
  bool needCal = inputs.calibrationRequested();
  diag.begin();   // starts the KWP reader task on core 0
  FaultDb::begin();   // mount SPIFFS; register full VAG fault descriptions if uploaded

  // Secured WiFi AP + OTA (credentials from NVS, with safe defaults).
  std::string ssid, pass, otaPass, homeSsid, homePass;
  if (!storage.getString("wifi.ssid", ssid)) ssid = "ESP32_MMI";
  if (!storage.getString("wifi.pass", pass) || pass.size() < 8) pass = "audimmi2024";
  if (!storage.getString("ota.pass",  otaPass)) otaPass = pass;
  storage.getString("home.ssid", homeSsid);   // provisioned via USB flash
  storage.getString("home.pass", homePass);
  ota.beginAp(ssid, pass, "admin", otaPass, homeSsid, homePass);

  // Live-debug hooks (reachable from a phone on the AP at http://192.168.4.1):
  //  /status  -> JSON telemetry,  /bc127 -> raw BC127 command console.
  ota.setStatusProvider([]() {
    const BtStatus& s = bt.status();
    auto san = [](const char* p) {   // keep JSON valid: printable ASCII only
      std::string o; for (; p && *p; ++p) { char c = *p; o += (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') ? c : '.'; }
      return o;
    };
    std::string dev = san(s.activeDeviceName.c_str()), title = san(s.title.c_str());
    std::string rl1 = san(radio.line1()), rl2 = san(radio.line2());
    char b[520];
    snprintf(b, sizeof(b),
      "{\"up_s\":%lu,\"heap\":%u,\"link\":%s,\"dev\":\"%s\",\"play\":%s,\"call\":%d,"
      "\"sco\":%s,\"title\":\"%s\",\"kwp\":%s,\"fisfail\":%lu,\"cd\":%s,\"rl1\":\"%s\",\"rl2\":\"%s\","
      "\"reads\":%lu,\"raw\":[%d,%d,%d],\"ctx\":%d,\"acts\":\"%s\"}",
      (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
      s.linked ? "true" : "false", dev.c_str(), s.playing ? "true" : "false",
      (int)s.call, s.scoOpen ? "true" : "false", title.c_str(),
      diag.isConnected() ? "true" : "false", (unsigned long)display.writeFails(),
      radio.cdMode() ? "true" : "false", rl1.c_str(), rl2.c_str(),
      (unsigned long)diag.readCount(),
      inputs.rawLadder(0), inputs.rawLadder(1), inputs.rawLadder(2),
      app ? (int)app->context() : -1,
      app ? app->actionTrace().c_str() : "");   // recent actions: catch phantom inputs
    return std::string(b);
  });
  ota.setBc127Console([](const std::string& c) { bt.sendCommand(c); },
                      []() { return bt.recentLog(); });
  ota.setKwpLog([]() { return diag.kwpDebug(); });   // KWP connect trace at /kwpdbg
  ota.setKwpProbe([](int rx, int tx) { diag.requestProbe(rx, tx); }); // K-line loopback at /kwpprobe
  ota.setKwpTest([](uint8_t e, uint8_t g) { diag.requestRead(e, g); });  // connect+read at /kwptest
  // Browser control/debug UI at http://192.168.4.1/control — drive the whole UI
  // from a computer (no car buttons needed) and see the live FIS.
  ota.setControlHooks(
    []() {
      return std::string("{\"display\":") + display.toJson() +
             ",\"bt\":" + webBtJson(bt.status()) +
             ",\"ctx\":\"" + webCtxName(app ? app->context() : Context::NowPlaying) + "\"}";
    },
    [](Control c, int d) { inputs.inject(c, (int8_t)d); });

  static Esp32System sys(ota, storage);
  static App theApp(display, inputs, bt, storage, diag);
  app = &theApp;
  app->setSystem(&sys);
  radio.begin();               // start sniffing the head unit's FIS output
  app->setRadio(&radio);       // forward radio text when not in CD mode
  app->begin();
  if (needCal) inputs.startCalibration();  // App shows the calibrate screen while active
  display.flush();                          // push the first frame to the FIS
}

void loop() {
  inputs.update();          // scan encoder + buttons into the event queue
  ota.handle();             // service the OTA web server
  app->tick(millis());      // drive the UI with a real millisecond clock
  display.flush();          // diff the frame -> queue changed regions
  display.service(millis()); // send one queued FIS command, millis-paced (non-blocking)

  // Serial heartbeat (115200) — quick live check over USB.
  static uint32_t lastHb = 0;
  if (millis() - lastHb > 1000) {
    lastHb = millis();
    const BtStatus& s = bt.status();
    Serial.printf("[hb] up=%lus heap=%u wifi=%s ip=%s link=%d dev=%s play=%d call=%d kwp=%d bc_rx=%lu fisfail=%lu ctx=%d\n",
      (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
      WiFi.status() == WL_CONNECTED ? "home" : "ap-only", ota.staIP().c_str(),
      s.linked, s.activeDeviceName.c_str(), s.playing, (int)s.call,
      diag.isConnected(), (unsigned long)bt.rxBytes(), (unsigned long)display.writeFails(),
      app ? (int)app->context() : -1);
  }
}
