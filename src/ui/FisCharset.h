// FisCharset.h — map UTF-8 text to the FIS cluster's character ROM.
//
// The Audi/VW FIS ROM matches Latin-1 for the common accented letters (á=0xE1,
// Ä=0xC4, …) and carries the Czech/Slovak/Polish carons & consonant-acutes at
// special codes 0x80–0x9F (č=0x82, ř=0x88, š=0x8C, ž=0x92, ć=0x80, ś=0x8A,
// ź=0x90, …). Byte codes verified against domnulvlad/TLBFISLib (extras).
//
// Text is upper-cased (the FIS standard font we use is upper-case), so accented
// letters map to their UPPER-CASE FIS code; letters whose upper-case accent has
// no ROM glyph (ě, ď, ť, ů, ň, ł, …) fall back to the plain ASCII letter.
#pragma once
#include <string>
#include <cstdint>

namespace mmi {

// Latin Extended-A caron/acute letters that DO have an upper-case FIS glyph.
// Both letter cases fold to the single upper-case code.
inline unsigned char fisExtAccent(uint32_t cp) {
  switch (cp) {
    case 0x0106: case 0x0107: return 0x80;  // Ć ć  acute
    case 0x010C: case 0x010D: return 0x82;  // Č č  caron
    case 0x011C: case 0x011D: return 0x84;  // Ĝ ĝ  (G caron glyph)
    case 0x0154: case 0x0155: return 0x86;  // Ŕ ŕ  acute
    case 0x0158: case 0x0159: return 0x88;  // Ř ř  caron
    case 0x015A: case 0x015B: return 0x8A;  // Ś ś  acute
    case 0x0160: case 0x0161: return 0x8C;  // Š š  caron
    case 0x015E: case 0x015F: return 0x8E;  // Ş ş  cedilla
    case 0x0179: case 0x017A: return 0x90;  // Ź ź  acute
    case 0x017D: case 0x017E: return 0x92;  // Ž ž  caron
    default: return 0;
  }
}

// Plain ASCII fallback (upper-case) for accented letters without a ROM glyph.
inline char fisExtAscii(uint32_t cp) {
  switch (cp) {
    case 0x0102: case 0x0103: case 0x0104: case 0x0105: return 'A';  // Ă Ą
    case 0x010E: case 0x010F: return 'D';                            // Ď
    case 0x0118: case 0x0119: case 0x011A: case 0x011B: return 'E';  // Ę Ě
    case 0x0139: case 0x013A: case 0x013D: case 0x013E:
    case 0x0141: case 0x0142: return 'L';                            // Ĺ Ľ Ł
    case 0x0143: case 0x0144: case 0x0147: case 0x0148: return 'N';  // Ń Ň
    case 0x0150: case 0x0151: return 'O';                            // Ő
    case 0x0164: case 0x0165: return 'T';                            // Ť
    case 0x016E: case 0x016F: case 0x0170: case 0x0171: return 'U';  // Ů Ű
    default: return 0;
  }
}

// Convert a UTF-8 string to the (upper-cased) FIS character set. Unsupported
// non-ASCII is dropped.
inline std::string toFisText(const std::string& s) {
  std::string o;
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp; int len;
    if (c < 0x80)                             { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4; }
    else                                      { i += 1; continue; }  // stray byte -> drop
    i += len;

    if (cp < 0x80) {                                    // ASCII
      if (cp >= 'a' && cp <= 'z') cp -= 32;             // upper-case
      if (cp >= 0x20) o.push_back((char)cp);
      continue;
    }
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) {  // Latin-1 letters
      if (cp == 0xDF) { o += "SS"; continue; }                  // ß
      unsigned char up = cp >= 0xE0 ? (unsigned char)(cp - 0x20) : (unsigned char)cp;  // to upper
      o.push_back((char)up);
      continue;
    }
    if (unsigned char a = fisExtAccent(cp)) { o.push_back((char)a); continue; }
    if (char b = fisExtAscii(cp))           { o.push_back(b);       continue; }
    // else: unsupported non-ASCII -> drop
  }
  return o;
}

} // namespace mmi
