// BluetoothManager.h — single-active-device policy over the BC127 (plan §3).
// Enforces exactly one live link, prioritises the last-used device, and falls
// back to any other available paired device. Decision logic is pure and
// unit-tested; connect/disconnect is delegated to IBluetooth.
#pragma once
#include "../hal/IBluetooth.h"
#include "../hal/IStorage.h"
#include <string>
#include <vector>

namespace mmi {

struct PairedDevice {
  std::string mac;
  std::string name;
  uint32_t    lastUsed = 0;   // higher = more recent
  bool        available = false; // in range / discoverable now
};

class BluetoothManager {
public:
  BluetoothManager(IBluetooth& bt, IStorage& storage);

  void begin();                                   // load last-used mac from storage
  void setPaired(std::vector<PairedDevice> list); // known/paired devices
  void setAvailable(const std::string& mac, bool available);

  // Pick the device to connect: the last-used one if it's available, otherwise
  // the most-recently-used among the available ones; "" if none available.
  std::string preferred() const;

  // Connect the preferred device (enforces single active link via IBluetooth).
  void connectPreferred();

  // Manual "switch device": connect this mac and mark it most-recently-used.
  void switchTo(const std::string& mac);

  // Call when a link actually opens: persist it as last-used.
  void onConnected(const std::string& mac);

  const std::vector<PairedDevice>& paired() const { return paired_; }
  std::string lastUsedMac() const { return lastMac_; }

private:
  PairedDevice* find(const std::string& mac);
  const PairedDevice* find(const std::string& mac) const;
  void bumpLastUsed(const std::string& mac);

  IBluetooth& bt_;
  IStorage&   storage_;
  std::vector<PairedDevice> paired_;
  std::string lastMac_;
  uint32_t    seq_ = 1;       // monotonic recency counter
};

} // namespace mmi
