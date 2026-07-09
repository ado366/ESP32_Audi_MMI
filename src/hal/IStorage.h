// IStorage.h — key/value + blob persistence.
// esp32 impl uses NVS (+ FFat for phonebook/logs); native impl uses a JSON file.
#pragma once
#include <cstdint>
#include <string>

namespace mmi {

class IStorage {
public:
  virtual ~IStorage() = default;

  virtual bool getString(const char* key, std::string& out) const = 0;
  virtual void putString(const char* key, const std::string& value) = 0;

  virtual int32_t getInt(const char* key, int32_t fallback) const = 0;
  virtual void putInt(const char* key, int32_t value) = 0;

  // Blob store (phonebook cache, fault-description table, logs).
  virtual bool readBlob(const char* path, std::string& out) const = 0;
  virtual void writeBlob(const char* path, const std::string& data) = 0;

  virtual void commit() = 0;
};

} // namespace mmi
