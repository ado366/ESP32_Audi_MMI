// EngineLabels.h — real VCDS measuring-block field names for THIS car's engine
// ECU (038906019CC, EDC15 1.9 TDI; layout verified against the 038-906-018
// family label file — see the VagCom Labels folder). The KWP formula table can
// only derive generic names from the formula id ("MAF", "LOAD"), which are
// often wrong for a given block — e.g. group 2 field 4 is coolant temperature
// but formula-labels as MAF. Labels are shortened to fit the FIS compressed
// font (~15 chars/row).
#pragma once
#include <cstdint>

namespace mmi {

// Returns the label for a 0-based field index of an engine measuring block, or
// nullptr when unknown (caller falls back to the generic formula label).
inline const char* engineFieldLabel(uint8_t group, int field) {
  struct Row { uint8_t g; const char* f[4]; };
  static const Row k[] = {
    { 1, {"RPM",          "INJ QTY",      "PISTON DISPL", "COOLANT"}},
    { 2, {"RPM",          "THROTTLE",     "OPER COND",    "COOLANT"}},
    { 3, {"RPM",          "MAF SPEC",     "MAF ACTUAL",   "EGR DUTY"}},
    { 4, {"RPM",          "INJ START SP", "INJ START AC", "COLD START"}},
    { 5, {"RPM",          "START INJ Q",  "INJ START",    "COOLANT"}},
    { 6, {"SPEED",        "BRAKE PEDAL",  "CRUISE",       "CRUISE FIT"}},
    { 7, {"FUEL TEMP",    "N/A",          "INTAKE AIR",   "COOLANT"}},
    { 8, {"RPM",          "INJ QTY 2",    "INJ QTY 3",    "INJ QTY 4"}},
    { 9, {"RPM",          "INJ QTY CC",   "INJ LIMIT",    ""}},
    {10, {"MAF",          "BARO PRESS",   "MANIF PRESS",  "THROTTLE"}},
    {11, {"RPM",          "BOOST SPEC",   "BOOST ACTUAL", "N75 DUTY"}},
    {12, {"GLOW STATUS",  "PREGLOW TIME", "SUPPLY VOLT",  "COOLANT"}},
    {13, {"CYL 1 DEV",    "CYL 2 DEV",    "CYL 3 DEV",    "CYL 4 DEV"}},
    {15, {"RPM",          "INJ QTY ACT",  "FUEL CONSUM",  "INJ QTY REQ"}},
    {16, {"GEN LOAD",     "AUX HEAT",     "ACTIVATION",   "SUPPLY VOLT"}},
    {19, {"PISTON START", "PISTON STOP",  "N/A",          "N/A"}},
  };
  if (field < 0 || field > 3) return nullptr;
  for (const Row& r : k)
    if (r.g == group) return r.f[field][0] ? r.f[field] : nullptr;
  return nullptr;
}

} // namespace mmi
