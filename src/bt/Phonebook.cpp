// Phonebook.cpp — PBAP parsing + number normalization + lookup.
#include "Phonebook.h"
#include <cctype>

namespace mmi {

std::string Phonebook::normalize(const std::string& number) {
  std::string digits;
  for (char c : number) if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
  if (digits.size() > 8) digits = digits.substr(digits.size() - 8); // last 8 for tolerant match
  return digits;
}

bool Phonebook::add(const std::string& name, const std::string& number, size_t maxEntries) {
  if (entries_.size() >= maxEntries) return false;
  entries_.push_back({name, number});
  return true;
}

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

size_t Phonebook::loadFromPbap(const std::string& stream, size_t maxEntries) {
  clear();
  std::string pendingName;
  bool haveName = false;
  size_t pos = 0;
  while (pos < stream.size()) {
    size_t nl = stream.find('\r', pos);
    if (nl == std::string::npos) nl = stream.find('\n', pos);
    std::string line = trim(stream.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
    pos = (nl == std::string::npos) ? stream.size() : nl + 1;
    if (line.empty()) continue;
    if (line == "PBAP_PB OK") break;

    const char* NAME = "PBAP_PB NAME:";
    const char* TEL  = "PBAP_PB TEL:";
    if (line.rfind(NAME, 0) == 0) {
      pendingName = trim(line.substr(std::string(NAME).size()));
      haveName = true;
    } else if (line.rfind(TEL, 0) == 0) {
      std::string number = trim(line.substr(std::string(TEL).size()));
      if (!add(haveName ? pendingName : std::string(), number, maxEntries)) break; // cap reached
      haveName = false;
      pendingName.clear();
    }
  }
  return entries_.size();
}

std::string Phonebook::lookup(const std::string& number) const {
  std::string key = normalize(number);
  if (key.empty()) return "";
  for (const auto& c : entries_)
    if (normalize(c.number) == key) return c.name;
  return "";
}

} // namespace mmi
