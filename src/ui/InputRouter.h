// InputRouter.h — the single, data-driven button-binding table (plan §2c).
// Maps (Control, Context) -> Action. Pure logic, no hardware; unit-tested on native.
#pragma once
#include "../hal/Types.h"

namespace mmi {

class InputRouter {
public:
  // Resolve a control in the current context to a logical action.
  // Returns Action::None when the control is unbound in that context.
  static Action resolve(Control c, Context ctx);
};

} // namespace mmi
