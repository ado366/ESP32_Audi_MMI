// Presets.h — favourites: user-chosen measuring values with a view + scale.
// Modeled on Maxi-K "programmed blocks" / FIS-Control "Schnellzugriff" (plan §4).
#pragma once
#include "../hal/IStorage.h"
#include "../Config.h"
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

namespace mmi {

enum class View : uint8_t { Unused = 0, TopLine, MultiValue, Graph, Boost };

struct Preset {
  uint8_t ecu = 0x01;
  uint8_t group = 2;
  uint8_t valueIndex = 0;      // which of the group's 4 values
  View    view = View::TopLine;
  float   min = 0.f;
  float   max = 100.f;
  float   guide1 = -1e9f;      // horizontal guide lines for graph (off-scale = hidden)
  float   guide2 = -1e9f;
  char    label[9] = {0};      // <= 8 chars, uppercase
};

class PresetStore {
public:
  int  size() const { return static_cast<int>(presets_.size()); }
  const Preset& at(int i) const { return presets_[i]; }
  Preset&       at(int i)       { return presets_[i]; }
  const std::vector<Preset>& all() const { return presets_; }

  bool add(const Preset& p) {
    if (static_cast<int>(presets_.size()) >= cfg::MAX_PRESETS) return false;
    presets_.push_back(p); return true;
  }
  void removeAt(int i) { if (i >= 0 && i < size()) presets_.erase(presets_.begin() + i); }
  void clear() { presets_.clear(); }

  // Binary (de)serialization to a storage blob. Same-build layout only.
  // If nothing is stored, any already-loaded defaults are left intact.
  void load(const IStorage& s) {
    std::string blob;
    if (!s.readBlob("diag.presets", blob) || blob.empty()) return;
    presets_.clear();
    uint8_t n = static_cast<uint8_t>(blob[0]);
    size_t need = 1 + static_cast<size_t>(n) * sizeof(Preset);
    if (blob.size() < need) return;
    presets_.resize(n);
    for (uint8_t i = 0; i < n; ++i)
      std::memcpy(&presets_[i], blob.data() + 1 + i * sizeof(Preset), sizeof(Preset));
  }
  void save(IStorage& s) const {
    std::string blob;
    blob.push_back(static_cast<char>(presets_.size()));
    for (const auto& p : presets_)
      blob.append(reinterpret_cast<const char*>(&p), sizeof(Preset));
    s.writeBlob("diag.presets", blob);
    s.commit();
  }

private:
  std::vector<Preset> presets_;
};

} // namespace mmi
