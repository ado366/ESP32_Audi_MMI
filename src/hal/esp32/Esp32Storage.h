// Esp32Storage.h — IStorage backed by ESP32 NVS (Preferences).
// Small blobs (settings, capped phonebook) live in NVS; migrate large data to
// FFat later if the phonebook cap is raised.
#pragma once
#include "../IStorage.h"
#include <Preferences.h>

namespace mmi {

class Esp32Storage : public IStorage {
public:
  void begin() { prefs_.begin("mmi", false); }

  bool getString(const char* key, std::string& out) const override {
    auto& p = const_cast<Preferences&>(prefs_);
    if (!p.isKey(key)) return false;
    out = std::string(p.getString(key, "").c_str());
    return true;
  }
  void putString(const char* key, const std::string& value) override {
    prefs_.putString(key, value.c_str());
  }
  int32_t getInt(const char* key, int32_t fallback) const override {
    auto& p = const_cast<Preferences&>(prefs_);
    return p.getInt(key, fallback);
  }
  void putInt(const char* key, int32_t value) override { prefs_.putInt(key, value); }

  bool readBlob(const char* path, std::string& out) const override {
    auto& p = const_cast<Preferences&>(prefs_);
    if (!p.isKey(path)) return false;
    size_t n = p.getBytesLength(path);
    if (n == 0) { out.clear(); return true; }
    out.resize(n);
    p.getBytes(path, &out[0], n);
    return true;
  }
  void writeBlob(const char* path, const std::string& data) override {
    prefs_.putBytes(path, data.data(), data.size());
  }

  void commit() override { /* NVS writes are committed per put */ }

private:
  Preferences prefs_;
};

} // namespace mmi
