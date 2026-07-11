// Phonebook.cpp — PBAP parsing + number normalization + lookup.
#include "Phonebook.h"
#include <cctype>
#include <cstdint>
#include <algorithm>

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
  inVcard_ = false; haveFn_ = false; qpCont_ = 0; vcName_.clear(); vcTel_.clear();
}

// Decode quoted-printable (=XX hex bytes) as used by many phones' vCards.
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)std::toupper((unsigned char)c);
  return (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}
static std::string qpDecode(const std::string& s) {
  std::string o; size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '=' && i + 2 < s.size() && hexv(s[i+1]) >= 0 && hexv(s[i+2]) >= 0) {
      o.push_back((char)((hexv(s[i+1]) << 4) | hexv(s[i+2]))); i += 3;
    } else { o.push_back(s[i]); ++i; }
  }
  return o;
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
    // Some entries have only a number (no FN/N) -> show the number instead of a
    // blank row. And some (esp. call-log entries) carry the number in FN with no
    // separate TEL -> use the name as the number source so it stays dialable
    // (dial() strips it to digits, so a real name harmlessly yields nothing).
    // Skip entries with neither name nor number.
    if (inVcard_ && (!vcName_.empty() || !vcTel_.empty()))
      add(vcName_.empty() ? vcTel_ : vcName_,
          vcTel_.empty()  ? vcName_ : vcTel_, maxEntries);
    inVcard_ = false; qpCont_ = 0; return;
  }
  if (!inVcard_) return;

  // Quoted-printable soft-break: a QP value ending in '=' continues on the next
  // raw line. Append the decoded continuation to the field being built.
  if (qpCont_) {
    bool more = !line.empty() && line.back() == '=';
    std::string dec = qpDecode(more ? line.substr(0, line.size() - 1) : line);
    if (qpCont_ == 1) vcName_ += dec; else vcTel_ += dec;
    if (!more) qpCont_ = 0;
    return;
  }

  // Property line: KEY[;params]:VALUE  (VALUE may itself contain ':')
  size_t colon = line.find(':');
  if (colon == std::string::npos) return;
  std::string key = line.substr(0, colon);
  std::string val = trim(line.substr(colon + 1));
  std::string keyUp = key; for (auto& c : keyUp) c = (char)std::toupper((unsigned char)c);
  std::string base = keyUp.substr(0, keyUp.find(';'));
  bool qp = keyUp.find("QUOTED-PRINTABLE") != std::string::npos;
  bool more = qp && !val.empty() && val.back() == '=';
  if (qp) val = qpDecode(more ? val.substr(0, val.size() - 1) : val);

  if (base == "FN")       { vcName_ = val; haveFn_ = true; if (more) qpCont_ = 1; }
  else if (base == "N")   { if (!haveFn_) { vcName_ = more ? val : nameFromN(val); if (more) qpCont_ = 1; } }
  else if (base == "TEL") { if (vcTel_.empty()) { vcTel_ = val; if (more) qpCont_ = 2; } }
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

void Phonebook::sortByName() {
  // Named contacts A-Z first; number-only entries (name is a phone number) last.
  auto isNum = [](const std::string& s) { return !s.empty() && (std::isdigit((unsigned char)s[0]) || s[0] == '+'); };
  std::sort(entries_.begin(), entries_.end(), [&](const Contact& a, const Contact& b) {
    bool an = isNum(a.name), bn = isNum(b.name);
    if (an != bn) return bn;                       // a is named, b is a number -> a first
    const std::string &x = a.name, &y = b.name;
    for (size_t i = 0; i < x.size() && i < y.size(); ++i) {
      int cx = std::toupper((unsigned char)x[i]), cy = std::toupper((unsigned char)y[i]);
      if (cx != cy) return cx < cy;
    }
    return x.size() < y.size();
  });
}

std::string Phonebook::lookup(const std::string& number) const {
  std::string key = normalize(number);
  if (key.empty()) return "";
  for (const auto& c : entries_)
    if (normalize(c.number) == key) return c.name;
  return "";
}

} // namespace mmi
