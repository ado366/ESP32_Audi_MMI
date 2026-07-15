// IInputs.h — source of decoded InputEvents (buttons, encoder, steering).
// esp32 impl reads AnalogMultiButton + ClickEncoder; native impl injects from the browser.
#pragma once
#include "Types.h"

namespace mmi {

class IInputs {
public:
  virtual ~IInputs() = default;

  // Poll hardware / queue. Returns true and fills `out` if an event is ready.
  // Call repeatedly until it returns false to drain all events this tick.
  virtual bool poll(InputEvent& out) = 0;

  // Raw ADC value for a button ladder (0=console1, 1=console2, 2=steering),
  // for the Button-Monitor / calibration screen. -1 if not applicable (native).
  virtual int rawLadder(int ladder) const { (void)ladder; return -1; }

  // Last DECODED control (name + how many decoded so far) for the BTN MON
  // screen — distinguishes "ADC moves but no press decodes" (thresholds wrong,
  // needs calibration) from "decodes but the action goes nowhere".
  virtual const char* lastControlName() const { return "-"; }
  virtual uint16_t    controlCount() const { return 0; }

  // Live encoder diagnostics for the ENCODER debug screen. Returns false when
  // not available (native emulator).
  struct EncoderDebug {
    int      pos = 0;                  // accumulated detents since boot (+CW/-CCW)
    bool     a = false, b = false;     // raw quadrature pin levels
    bool     pressed = false;          // button currently down
    uint16_t clicks = 0, holds = 0;    // decoded click / long-press counts
  };
  virtual bool encoderDebug(EncoderDebug& out) const { (void)out; return false; }

  // --- Button-learning (calibration) hooks (no-ops on native) ---
  virtual bool        calibrating() const { return false; }
  virtual void        startCalibration() {}
  virtual void        cancelCalibration() {}
  virtual const char* calibPrompt() const { return ""; } // e.g. "PRESS MENU"
  virtual int         calibStep() const { return 0; }
  virtual int         calibTotal() const { return 0; }
  // True at boot if the "enter calibration" gesture is held (encoder button).
  virtual bool        calibrationRequested() const { return false; }
};

} // namespace mmi
