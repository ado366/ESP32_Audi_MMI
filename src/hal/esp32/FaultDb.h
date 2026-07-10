// FaultDb.h — SPIFFS-backed full VAG fault-code description table.
//
// The complete table (~4800 codes, ~215KB) is too big for the OTA app partition,
// so it lives as /faults.bin on the SPIFFS data partition (uploadable over WiFi
// via OtaManager /descupload — no reflash). Format:
//   "FLT1" | count:u16 | count x { code:u16, offset:u32 } (sorted by code) | blob
// Lookup is a binary search by file seek (no RAM index), returning the string
// into a static buffer. Registered into dtcDescription() via setDtcLookup().
#pragma once
#include "../../diag/DtcDescriptions.h"
#include <SPIFFS.h>
#include <Arduino.h>

namespace mmi {

class FaultDb {
public:
  // Mount SPIFFS and register the lookup. lookup() returns nullptr when the file
  // isn't there yet, so it's safe to register before /faults.bin is uploaded
  // (no reboot needed after upload).
  static void begin() {
    if (!SPIFFS.begin(true)) return;              // format-on-fail so /descupload works
    setDtcLookup(&FaultDb::lookup);
  }
  static bool loaded() { return SPIFFS.exists("/faults.bin"); }

  static const char* lookup(uint16_t code) {
    File f = SPIFFS.open("/faults.bin", "r");
    if (!f) return nullptr;
    char magic[4]; if (f.read((uint8_t*)magic, 4) != 4 || memcmp(magic, "FLT1", 4)) { f.close(); return nullptr; }
    uint16_t count = 0; f.read((uint8_t*)&count, 2);
    const uint32_t recBase = 6, blobBase = recBase + (uint32_t)count * 6;
    int lo = 0, hi = (int)count - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      f.seek(recBase + (uint32_t)mid * 6);
      uint16_t c = 0; uint32_t off = 0;
      f.read((uint8_t*)&c, 2); f.read((uint8_t*)&off, 4);
      if (c == code) {
        f.seek(blobBase + off);
        static char buf[64];
        int n = f.readBytesUntil('\0', buf, sizeof(buf) - 1);
        buf[n] = 0; f.close();
        return buf;
      }
      if (c < code) lo = mid + 1; else hi = mid - 1;
    }
    f.close();
    return nullptr;
  }
};

} // namespace mmi
