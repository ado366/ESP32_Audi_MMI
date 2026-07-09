// Types.h — shared, hardware-independent enums/structs for the UI + input layer.
// No Arduino.h here: this compiles on both the ESP32 and the native emulator.
#pragma once
#include <cstdint>

namespace mmi {

// Physical controls the driver can actuate. Chords are distinct ladder values.
enum class Control : uint8_t {
  None = 0,
  // Center console buttons
  Menu, Return, Nav, Info, Traffic,
  // Console chords (detectable distinct ladder values)
  MenuReturn, MenuNav, TrafficReverse, InfoReverse,
  // Rotary encoder
  EncoderCW, EncoderCCW, EncoderClick, EncoderHold,
  // Steering wheel (repurposed tiptronic)
  SteerLeftPlus, SteerLeftMinus, SteerRightPlus, SteerRightMinus,
  // Steering chords
  SteerLPlusRPlus, SteerLPlusRMinus, SteerLMinusRPlus, SteerLMinusRMinus,
};

// What is on screen right now — selects which binding applies.
enum class Context : uint8_t {
  NowPlaying = 0, // idle / default root screen
  Menu,           // a menu is open
  IncomingCall,   // ringing
  ActiveCall,     // call connected
  Diagnostics,    // a live diagnostics/favourites view
};

// Logical actions the app can perform. The InputRouter maps
// (Control, Context) -> Action; screens/handlers act on Actions.
enum class Action : uint8_t {
  None = 0,
  // Media / radio
  VolumeUp, VolumeDown,
  TrackNext, TrackPrev,
  PlayPause,
  // Menu navigation
  MenuOpenClose, ScrollUp, ScrollDown, Select, Back, RootBack,
  // Call control
  CallAnswer, CallReject, CallEnd,
  // Shortcuts
  JumpDiagnostics, JumpNowPlaying, CycleInfo,
  // Assignable (chords) — resolved via Settings at a higher layer
  Assignable,
};

// One decoded input event handed to the app each tick.
struct InputEvent {
  Control control = Control::None;
  int8_t  encoderDelta = 0; // for EncoderCW/CCW: signed detents this tick
};

} // namespace mmi
