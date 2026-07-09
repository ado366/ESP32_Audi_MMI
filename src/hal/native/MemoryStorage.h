// MemoryStorage.h — native IStorage backed by in-memory maps.
// A JSON-file-backed variant replaces this when settings persistence lands.
#pragma once
#include "../IStorage.h"
#include <map>

namespace mmi {

class MemoryStorage : public IStorage {
public:
  bool getString(const char* key, std::string& out) const override {
    auto it = strs_.find(key);
    if (it == strs_.end()) return false;
    out = it->second; return true;
  }
  void putString(const char* key, const std::string& value) override { strs_[key] = value; }

  int32_t getInt(const char* key, int32_t fallback) const override {
    auto it = ints_.find(key);
    return it == ints_.end() ? fallback : it->second;
  }
  void putInt(const char* key, int32_t value) override { ints_[key] = value; }

  bool readBlob(const char* path, std::string& out) const override {
    auto it = blobs_.find(path);
    if (it == blobs_.end()) return false;
    out = it->second; return true;
  }
  void writeBlob(const char* path, const std::string& data) override { blobs_[path] = data; }

  void commit() override {}

private:
  std::map<std::string, std::string> strs_;
  std::map<std::string, int32_t>     ints_;
  std::map<std::string, std::string> blobs_;
};

} // namespace mmi
