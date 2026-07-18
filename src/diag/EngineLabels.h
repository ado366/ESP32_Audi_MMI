// EngineLabels.h — real VCDS measuring-block field names for THIS car's engine
// ECU: 038906019CC, EDC15 1.9 TDI PD (AJM/ATJ/AUY/BVK 115hp family).
// Transcribed from the Ross-Tech 038-906-019 label file (2015 revision,
// user-provided) — NOT the ALH-family layout, though the load-bearing fields
// agree: grp 6/1 = vehicle speed, grp 11/3 = actual boost, grp 1/4 = coolant.
// The KWP formula table can only derive generic names from the formula id
// ("MAF", "LOAD"), which are often wrong for a given block — e.g. group 2
// field 4 is coolant temperature but formula-labels as MAF. Labels are
// shortened to fit the FIS compressed font (~12 chars/row).
#pragma once
#include <cstdint>

namespace mmi {

// Returns the label for a 0-based field index of an engine measuring block, or
// nullptr when unknown (caller falls back to the generic formula label).
inline const char* engineFieldLabel(uint8_t group, int field) {
  struct Row { uint8_t g; const char* f[4]; };
  static const Row k[] = {
    {  1, {"RPM",         "INJ QTY",      "INJ DURATION", "COOLANT"}},
    {  2, {"RPM",         "THROTTLE",     "OPER COND",    "COOLANT"}},
    {  3, {"RPM",         "MAF SPEC",     "MAF ACTUAL",   "EGR DUTY"}},
    {  4, {"RPM",         "INJ START SP", "INJ DUR SPEC", "SYNCHRO ANG"}},
    {  5, {"RPM",         "START INJ Q",  "START SYNCHR", "COOLANT"}},
    {  6, {"SPEED",       "PEDAL SW",     "ACCEL PEDAL",  "CRUISE SW"}},
    {  7, {"FUEL TEMP",   "OIL TEMP",     "INTAKE AIR",   "COOLANT"}},
    {  8, {"RPM",         "IQ REQUEST",   "IQ TORQUE",    "IQ MAF"}},
    {  9, {"RPM",         "IQ CC",        "IQ AT SHIFT",  "IQ MAX"}},
    { 10, {"MAF",         "ATMOS PRESS",  "INTAKE PRESS", "THROTTLE"}},
    { 11, {"RPM",         "BOOST SPEC",   "BOOST ACTUAL", "N75 DUTY"}},
    { 12, {"GLOW STATUS", "PREGLOW TIME", "SUPPLY VOLT",  "COOLANT"}},
    { 13, {"CYL 1 DEV",   "CYL 2 DEV",    "CYL 3 DEV",    "CYL 4 DEV"}},
    { 15, {"RPM",         "INJ QTY ACT",  "FUEL CONSUM",  "INJ QTY SPEC"}},
    { 16, {"GEN LOAD",    "AUX HEAT",     "HEAT ELEMENT", "SUPPLY VOLT"}},
    { 18, {"PD VALVE 1",  "PD VALVE 2",   "PD VALVE 3",   "PD VALVE 4"}},
    {125, {"GEARBOX CAN", "ABS CAN",      "CLUSTER CAN",  "AIRBAG CAN"}},
  };
  if (field < 0 || field > 3) return nullptr;
  for (const Row& r : k)
    if (r.g == group) return r.f[field][0] ? r.f[field] : nullptr;
  return nullptr;
}

} // namespace mmi
