// Phonebook.h — PBAP phonebook cache + caller-ID name resolution (plan §3c).
// Pure logic (no hardware): parses the BC127 PBAP stream, stores a capped
// name/number table, and resolves an incoming number to a contact name.
#pragma once
#include "../hal/IBluetooth.h" // for mmi::Contact
#include <string>
#include <vector>

namespace mmi {

class Phonebook {
public:
  void clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }
  const std::vector<Contact>& entries() const { return entries_; }

  // Add one contact (respects the cap). Returns false if capped out.
  bool add(const std::string& name, const std::string& number, size_t maxEntries);

  // Parse a raw BC127 PBAP pull stream:
  //   PBAP_PB NAME: <name>\r PBAP_PB TEL: <number>\r ... PBAP_PB OK
  // Returns the number of contacts loaded (<= maxEntries).
  size_t loadFromPbap(const std::string& stream, size_t maxEntries);

  // Resolve a dialled/incoming number to a name; "" if unknown.
  std::string lookup(const std::string& number) const;

  // Normalize a number for tolerant matching: digits only, last 8 kept.
  static std::string normalize(const std::string& number);

private:
  std::vector<Contact> entries_;
};

} // namespace mmi
