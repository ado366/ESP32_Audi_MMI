// BluetoothManager.cpp — single-active-device selection + persistence.
#include "BluetoothManager.h"

namespace mmi {

BluetoothManager::BluetoothManager(IBluetooth& bt, IStorage& storage)
  : bt_(bt), storage_(storage) {}

void BluetoothManager::begin() {
  std::string mac;
  if (storage_.getString("bt.lastMac", mac)) lastMac_ = mac;
}

void BluetoothManager::setPaired(std::vector<PairedDevice> list) {
  paired_ = std::move(list);
  // Seed the recency counter above any restored lastUsed values.
  for (const auto& d : paired_) if (d.lastUsed >= seq_) seq_ = d.lastUsed + 1;
}

PairedDevice* BluetoothManager::find(const std::string& mac) {
  for (auto& d : paired_) if (d.mac == mac) return &d;
  return nullptr;
}
const PairedDevice* BluetoothManager::find(const std::string& mac) const {
  for (const auto& d : paired_) if (d.mac == mac) return &d;
  return nullptr;
}

void BluetoothManager::setAvailable(const std::string& mac, bool available) {
  if (auto* d = find(mac)) d->available = available;
}

std::string BluetoothManager::preferred() const {
  // 1) last-used device, if it is currently available.
  if (!lastMac_.empty()) {
    const PairedDevice* d = find(lastMac_);
    if (d && d->available) return d->mac;
  }
  // 2) otherwise the most-recently-used among available devices.
  const PairedDevice* best = nullptr;
  for (const auto& d : paired_) {
    if (!d.available) continue;
    if (!best || d.lastUsed > best->lastUsed) best = &d;
  }
  return best ? best->mac : std::string();
}

void BluetoothManager::bumpLastUsed(const std::string& mac) {
  if (auto* d = find(mac)) d->lastUsed = seq_++;
}

void BluetoothManager::connectPreferred() {
  std::string mac = preferred();
  if (!mac.empty()) bt_.connectDevice(mac); // IBluetooth drops any other link
}

void BluetoothManager::switchTo(const std::string& mac) {
  if (!find(mac)) return;
  bumpLastUsed(mac);
  bt_.connectDevice(mac);       // single active link enforced downstream
  onConnected(mac);
}

void BluetoothManager::onConnected(const std::string& mac) {
  bumpLastUsed(mac);
  lastMac_ = mac;
  storage_.putString("bt.lastMac", mac);
  storage_.commit();
}

} // namespace mmi
