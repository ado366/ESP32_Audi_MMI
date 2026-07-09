// Config.h — pin map and compile-time constants for the ESP32 Audi MMI.
// Pins carried over verbatim from the v1 sketch so existing wiring is unchanged.
#pragma once

namespace cfg {

// Firmware version (shown on the Version screen + OTA page; bump per release).
constexpr const char* FW_VERSION = "2.1.4";

// Default pull-OTA source: the "latest" release asset on GitHub. Overridable via
// the NVS key "ota.url". Publish firmware.bin as a release asset named exactly this.
constexpr const char* OTA_URL = "https://github.com/ado366/ESP32_Audi_MMI/releases/latest/download/firmware.bin";

// Networks pull-OTA tries (in order) to reach the internet. Credentials live in
// the gitignored Secrets.h (cfg::HOTSPOT_SSID/PASS, cfg::HOME_SSID/PASS); NVS
// keys hotspot.ssid/pass and home.ssid/pass override them at runtime.

// Mirror all BC127 UART traffic to the USB debug serial ("[BC127] ...").
// Off by default; the /bc127 web console + on-cluster BC127 screen still work.
constexpr bool DEBUG_BC127_SERIAL = false;


// ---- Rotary encoder (center console) ----
constexpr int PIN_ENC_A      = 22;
constexpr int PIN_ENC_B      = 19;
constexpr int PIN_ENC_BUTTON = 21;

// ---- FIS cluster write bus (3-wire) ----
constexpr int PIN_FIS_CLK  = 27;
constexpr int PIN_FIS_DATA = 14;
constexpr int PIN_FIS_ENA  = 26;

// ---- Radio/head-unit read bus (3-wire, sniffed) ----
constexpr int PIN_RADIO_CLK  = 35;
constexpr int PIN_RADIO_ENA  = 33;
constexpr int PIN_RADIO_DATA = 32;

// ---- OEM radio remote (single-wire REM) ----
constexpr int PIN_REMOTE = 25;

// ---- Analog button ladders ----
constexpr int PIN_BTN_CONSOLE_1 = 39; // Menu / Return / Nav (+ chords)
constexpr int PIN_BTN_CONSOLE_2 = 36; // Info / Traffic (+ reverse signal)
constexpr int PIN_BTN_STEERING  = 34; // L+ / L- / R+ / R- (+ chords)

// ---- KWP1281 K-line ----
constexpr int PIN_KWP_RX = 5;
constexpr int PIN_KWP_TX = 0;

// ---- BC127 Bluetooth (UART1) ----
// Confirmed working 2026-07-09: RX=4, TX=13 (module TX -> GPIO4, GPIO13 -> module RX).
constexpr int PIN_BC127_RX = 4;
constexpr int PIN_BC127_TX = 13;
constexpr int BC127_BAUD   = 115200; // >= 115200 required for PBAP phonebook pull

// ---- CAN (transceiver present, feature inert for now) ----
constexpr int PIN_CAN_TX = 23;
constexpr int PIN_CAN_RX = 18;

// ---- FIS geometry ----
constexpr int FIS_WIDTH  = 64;
constexpr int FIS_HEIGHT = 88;
constexpr int FIS_LINE_CHARS = 8; // characters per top text line

// ---- Feature limits ----
constexpr int MAX_PRESETS         = 9;   // Maxi-K / FIS-Control style favourites
constexpr int PHONEBOOK_MAX_ENTRY = 500; // "memory permitting" cap; trimmed beyond

} // namespace cfg
