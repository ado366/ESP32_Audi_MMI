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

using namespace mmi;

static Esp32Storage   storage;
static Esp32Display   display;
static Esp32Inputs    inputs;
static Esp32Bluetooth bt;
static Esp32Diag      diag;
static OtaManager     ota;
static App*           app = nullptr;

void setup() {
  Serial.begin(115200);
  Serial.println(F("ESP32 Audi MMI v2 — boot"));

  storage.begin();
  display.begin();
  inputs.begin();
  inputs.attachStorage(&storage);
  inputs.loadThresholds(storage);  // apply calibrated button thresholds if saved
  // Enter button-learning ONLY on a deliberate gesture: encoder button held at
  // boot. (Also reachable any time via encoder long-press -> Debug -> Calibrate.)
  bool needCal = inputs.calibrationRequested();
  diag.begin();   // starts the KWP reader task on core 0

  // Secured WiFi AP + OTA (credentials from NVS, with safe defaults).
  std::string ssid, pass, otaPass;
  if (!storage.getString("wifi.ssid", ssid)) ssid = "ESP32_MMI";
  if (!storage.getString("wifi.pass", pass) || pass.size() < 8) pass = "audimmi2024";
  if (!storage.getString("ota.pass",  otaPass)) otaPass = pass;
  ota.beginAp(ssid, pass, "admin", otaPass);

  // Live-debug hooks (reachable from a phone on the AP at http://192.168.4.1):
  //  /status  -> JSON telemetry,  /bc127 -> raw BC127 command console.
  ota.setStatusProvider([]() {
    const BtStatus& s = bt.status();
    char b[320];
    snprintf(b, sizeof(b),
      "{\"up_s\":%lu,\"heap\":%u,\"link\":%s,\"dev\":\"%s\",\"play\":%s,\"call\":%d,"
      "\"sco\":%s,\"title\":\"%s\",\"kwp\":%s,\"raw\":[%d,%d,%d],\"ctx\":%d}",
      (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
      s.linked ? "true" : "false", s.activeDeviceName.c_str(), s.playing ? "true" : "false",
      (int)s.call, s.scoOpen ? "true" : "false", s.title.c_str(),
      diag.isConnected() ? "true" : "false",
      inputs.rawLadder(0), inputs.rawLadder(1), inputs.rawLadder(2),
      app ? (int)app->context() : -1);
    return std::string(b);
  });
  ota.setBc127Console([](const std::string& c) { bt.sendCommand(c); },
                      []() { return bt.recentLog(); });
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
  app->begin();
  if (needCal) inputs.startCalibration();  // App shows the calibrate screen while active
}

void loop() {
  inputs.update();          // scan encoder + buttons into the event queue
  ota.handle();             // service the OTA web server
  app->tick(millis());      // drive the UI with a real millisecond clock

  // Serial heartbeat (115200) — quick live check over USB.
  static uint32_t lastHb = 0;
  if (millis() - lastHb > 1000) {
    lastHb = millis();
    const BtStatus& s = bt.status();
    Serial.printf("[hb] up=%lus heap=%u link=%d dev=%s play=%d call=%d kwp=%d bc_rx=%lu raw=%d/%d/%d ctx=%d\n",
      (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
      s.linked, s.activeDeviceName.c_str(), s.playing, (int)s.call,
      diag.isConnected(), (unsigned long)bt.rxBytes(),
      inputs.rawLadder(0), inputs.rawLadder(1), inputs.rawLadder(2),
      app ? (int)app->context() : -1);
  }
}
