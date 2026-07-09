// ScriptedInputs.h — native IInputs driven by a queued list of events.
// Used by the smoke driver and tests; the browser server feeds this later.
#pragma once
#include "../IInputs.h"
#include <deque>

namespace mmi {

class ScriptedInputs : public IInputs {
public:
  void push(Control c, int8_t delta = 0) { q_.push_back({c, delta}); }

  bool poll(InputEvent& out) override {
    if (q_.empty()) return false;
    out = q_.front();
    q_.pop_front();
    return true;
  }

private:
  std::deque<InputEvent> q_;
};

} // namespace mmi
