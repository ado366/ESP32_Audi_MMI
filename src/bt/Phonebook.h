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

  // Incremental vCard feed for the BC127 PB_PULL stream, which arrives one serial
  // line at a time:
  //   PB_PULL 16 184 BEGIN:VCARD / VERSION:2.1 / FN;CHARSET=UTF-8:Alice /
  //   N;...:Smith;Alice;;; / TEL;TYPE=CELL:+41791234567 / END:VCARD  ... OK
  // beginPull() resets the per-vCard parser (call once before feeding a pull).
  void beginPull();
  void feedLine(const std::string& line, size_t maxEntries);

  // Convenience: parse a whole PB_PULL stream at once (splits on CR/LF then
  // feeds each line). Clears first. Returns the number of contacts loaded.
  size_t loadFromPbap(const std::string& stream, size_t maxEntries);

  // Sort entries alphabetically by name (case-insensitive, first name first).
  void sortByName();

  // Resolve a dialled/incoming number to a name; "" if unknown.
  std::string lookup(const std::string& number) const;

  // Normalize a number for tolerant matching: digits only, last 8 kept.
  static std::string normalize(const std::string& number);

private:
  std::vector<Contact> entries_;
  // Per-vCard accumulation state (incremental parse).
  std::string vcName_, vcTel_;
  bool        inVcard_ = false;
  bool        haveFn_  = false;   // FN seen -> don't overwrite with N
  int         qpCont_  = 0;       // quoted-printable soft-break continuation: 0=no, 1=name, 2=tel
};

} // namespace mmi
