// FisCharset.h — map UTF-8 text to THIS Audi B5 cluster's character ROM.
//
// The ROM is NOT Latin-1: the accented letters live at cluster-specific codes,
// read directly off the cluster with the charset explorer (DEBUG -> FIS TEST).
// Verified Czech/Slovak set below. Text is upper-cased (the standard font we use
// looks best upper-case); letters without a ROM glyph fall back to plain ASCII.
//
// If a glyph looks wrong, re-read it in the explorer and update the table here.
#pragma once
#include <string>
#include <cstdint>

namespace mmi {

// Cluster ROM code for an accented letter (0 = not in this table). Both letter
// cases fold to the one code the cluster provides.
inline unsigned char fisRomCode(uint32_t cp) {
  switch (cp) {
    case 0x00E1: case 0x00C1: return 0xC0;  // á Á
    case 0x00E9: case 0x00C9: return 0xC2;  // é É
    case 0x00ED: case 0x00CD: return 0xC4;  // í Í
    case 0x00F3: case 0x00D3: return 0xC5;  // ó Ó
    case 0x00FA: case 0x00DA: return 0xC8;  // ú Ú
    case 0x00FD: case 0x00DD: return 0xE5;  // ý Ý
    case 0x010D: case 0x010C: return 0xCB;  // č Č
    case 0x0159: case 0x0158: return 0xCA;  // ř Ř
    case 0x0161: case 0x0160: return 0xCC;  // š Š
    case 0x017E: case 0x017D: return 0xCD;  // ž Ž
    case 0x011B: case 0x011A: return 0xE2;  // ě Ě  (approx glyph)
    case 0x0148: case 0x0147: return 0x8A;  // ň Ň
    case 0x010F: case 0x010E: return 0xDE;  // ď Ď  (ROM has lowercase only)
    case 0x016F: case 0x016E: return 0x98;  // ů Ů
    default: return 0;
  }
}

// Plain ASCII base (upper-case) for accented letters NOT in the ROM table, so
// German/Polish/etc. names degrade to readable ASCII instead of wrong glyphs.
inline char fisFoldAscii(uint32_t cp) {
  switch (cp) {
    case 0x00C0: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
    case 0x00E0: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
    case 0x0102: case 0x0103: case 0x0104: case 0x0105: return 'A';
    case 0x00C7: case 0x00E7: case 0x0106: case 0x0107: return 'C';
    case 0x00C8: case 0x00CA: case 0x00CB: case 0x00E8: case 0x00EA: case 0x00EB:
    case 0x0118: case 0x0119: return 'E';
    case 0x00CC: case 0x00CE: case 0x00CF: case 0x00EC: case 0x00EE: case 0x00EF: return 'I';
    case 0x00D1: case 0x00F1: case 0x0143: case 0x0144: return 'N';
    case 0x00D2: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
    case 0x00F2: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
    case 0x0150: case 0x0151: return 'O';
    case 0x00D9: case 0x00DB: case 0x00DC: case 0x00F9: case 0x00FB: case 0x00FC:
    case 0x0170: case 0x0171: return 'U';
    case 0x0164: case 0x0165: return 'T';   // ť Ť — no ROM glyph
    case 0x0141: case 0x0142: case 0x0139: case 0x013A: case 0x013D: case 0x013E: return 'L';
    case 0x015A: case 0x015B: return 'S';
    case 0x0179: case 0x017A: case 0x017B: case 0x017C: return 'Z';
    case 0x0155: case 0x0154: return 'R';
    default: return 0;
  }
}

// Convert a UTF-8 string to the (upper-cased) FIS character set for this cluster.
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
    if (unsigned char r = fisRomCode(cp)) { o.push_back((char)r); continue; }  // ROM accent
    if (char b = fisFoldAscii(cp))        { o.push_back(b);       continue; }  // fold to ASCII
    if (cp == 0x00DF) { o += "SS"; continue; }                                 // ß
    // else: unsupported -> drop
  }
  return o;
}

} // namespace mmi
