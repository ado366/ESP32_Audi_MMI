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
