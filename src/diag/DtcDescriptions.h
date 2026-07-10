// DtcDescriptions.h — plaintext for VAG KWP1281 5-digit fault codes.
// Curated STARTER set oriented to a Bosch EDC15 TDI ECU. Verify/extend against
// your VCDS label file for the exact controller — unknown codes fall back to the
// raw number (dtcDescription returns nullptr), so nothing here is guessed.
//
// NB: some codes are ECU-context dependent — e.g. on EDC15 TDI, 00575 is
// "Intake Manifold Pressure" (boost/limp), not the generic "Lambda probe 2".
#pragma once
#include <cstdint>

namespace mmi {

struct DtcDesc { uint16_t code; const char* text; };

// Common EDC15 TDI (and shared VAG) codes.
inline const DtcDesc kDtcTable[] = {
  {   256, "ENGINE TORQUE LIMIT" },
  {   522, "COOLANT TEMP SENSOR G62" },
  {   532, "SUPPLY VOLTAGE B+" },
  {   550, "INJECTION TIMING" },        // commencement of injection regulation
  {   553, "MASS AIR FLOW G70" },
  {   560, "EGR SYSTEM" },
  {   561, "MIXTURE ADAPTATION" },
  {   575, "INTAKE MANIFOLD PRESSURE" },// boost / limp mode on TDI
  {   668, "SUPPLY VOLTAGE TERM 30" },
  {  1259, "FUEL CUTOFF VALVE N109" },
  {  1266, "GLOW PLUG RELAY J52" },
  { 65535, "INTERNAL ECU FAULT" },
  // P-code-style values some tools/ECUs report:
  { 16486, "MAF SIGNAL LOW (P0102)" },
  { 17965, "CHARGE PRESS CTRL (P1557)" },
  { 17964, "CHARGE PRESS CTRL (P1556)" },
};

// Optional external lookup (ESP32 registers a SPIFFS-backed full VAG table). It
// returns a pointer to a static buffer valid until the next call, or nullptr.
using DtcLookupFn = const char* (*)(uint16_t code);
inline DtcLookupFn& dtcLookupFn() { static DtcLookupFn fn = nullptr; return fn; }
inline void setDtcLookup(DtcLookupFn fn) { dtcLookupFn() = fn; }

// Returns the description for a code, or nullptr if unknown. Prefers the external
// (full) table when registered, else the built-in EDC15 starter set.
inline const char* dtcDescription(uint16_t code) {
  if (auto fn = dtcLookupFn()) { if (const char* s = fn(code)) return s; }
  for (const auto& d : kDtcTable) if (d.code == code) return d.text;
  return nullptr;
}

} // namespace mmi
