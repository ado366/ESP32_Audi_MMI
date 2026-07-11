// Phonebook.cpp — PBAP parsing + number normalization + lookup.
#include "Phonebook.h"
#include <cctype>
#include <cstdint>

namespace mmi {

std::string Phonebook::normalize(const std::string& number) {
  std::string digits;
  for (char c : number) if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
  if (digits.size() > 8) digits = digits.substr(digits.size() - 8); // last 8 for tolerant match
  return digits;
}

bool Phonebook::add(const std::string& name, const std::string& number, size_t maxEntries) {
  if (entries_.size() >= maxEntries) return false;
  // Store the name as-is (UTF-8); the display layer maps it to the FIS charset
  // (accents render on the cluster; see FisCharset.h).
  entries_.push_back({name, number});
  return true;
}

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

void Phonebook::beginPull() {
  inVcard_ = false; haveFn_ = false; vcName_.clear(); vcTel_.clear();
}

// Turn a structured "N" value ("Family;Given;Middle;Prefix;Suffix") into a
// display name "Given Family". Falls back to a space-joined form.
static std::string nameFromN(const std::string& v) {
  std::string parts[5]; int p = 0;
  for (char c : v) { if (c == ';') { if (p < 4) ++p; } else if (p < 5) parts[p].push_back(c); }
  std::string family = trim(parts[0]), given = trim(parts[1]);
  std::string out = given;
  if (!family.empty()) { if (!out.empty()) out += ' '; out += family; }
  return out.empty() ? trim(v) : out;
}

// Strip a "PB_PULL <link> <size> " notification header from the front of a line
// (the module prefixes it to the first line of each chunk).
static std::string stripPullHeader(const std::string& s) {
  if (s.rfind("PB_PULL", 0) != 0) return s;
  size_t p = 7; int skip = 2;                       // skip <link> and <size>
  while (skip-- > 0) {
    while (p < s.size() && s[p] == ' ') ++p;
    while (p < s.size() && s[p] != ' ') ++p;
  }
  while (p < s.size() && s[p] == ' ') ++p;
  return s.substr(p);
}

void Phonebook::feedLine(const std::string& raw, size_t maxEntries) {
  std::string line = stripPullHeader(trim(raw));
  // A chunk boundary can merge the next header into a value; cut it off so it
  // doesn't leak into a name/number (e.g. "...NamePB_PULL 16 483 ...").
  size_t merge = line.find("PB_PULL");
  if (merge != std::string::npos) line = trim(line.substr(0, merge));
  if (line.empty()) return;
  // A vCard may share a serial line with the PB_PULL header ("PB_PULL 16 184
  // BEGIN:VCARD"), so match on the marker anywhere in the line.
  if (line.find("BEGIN:VCARD") != std::string::npos) { inVcard_ = true; haveFn_ = false; vcName_.clear(); vcTel_.clear(); return; }
  if (line.find("END:VCARD")   != std::string::npos) {
    if (inVcard_ && (!vcName_.empty() || !vcTel_.empty())) add(vcName_, vcTel_, maxEntries);
    inVcard_ = false; return;
  }
  if (!inVcard_) return;

  // Property line: KEY[;params]:VALUE  (VALUE may itself contain ':')
  size_t colon = line.find(':');
  if (colon == std::string::npos) return;
  std::string key = line.substr(0, colon);
  std::string val = trim(line.substr(colon + 1));
  size_t semi = key.find(';');
  std::string base = key.substr(0, semi);
  for (auto& c : base) c = (char)std::toupper((unsigned char)c);

  if (base == "FN")       { vcName_ = val; haveFn_ = true; }
  else if (base == "N")   { if (!haveFn_) vcName_ = nameFromN(val); }
  else if (base == "TEL") { if (vcTel_.empty()) vcTel_ = val; }
}

size_t Phonebook::loadFromPbap(const std::string& stream, size_t maxEntries) {
  clear();
  beginPull();
  size_t pos = 0;
  while (pos < stream.size()) {
    size_t nl = stream.find_first_of("\r\n", pos);
    std::string line = stream.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? stream.size() : nl + 1;
    feedLine(line, maxEntries);
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
