// Secrets.example.h — template. Copy to Secrets.h (gitignored) and fill in your
// WiFi credentials for pull-OTA. Secrets.h is never committed.
#pragma once

namespace cfg {

// Phone hotspot (tried first for pull-OTA)
constexpr const char* HOTSPOT_SSID = "YOUR_HOTSPOT_SSID";
constexpr const char* HOTSPOT_PASS = "YOUR_HOTSPOT_PASS";

// Home / local network (tried second); leave SSID empty to skip.
constexpr const char* HOME_SSID    = "";
constexpr const char* HOME_PASS    = "";

} // namespace cfg
