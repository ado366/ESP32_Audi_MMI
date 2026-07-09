// MenuTree.h — the nested menu definition (data, not logic), per plan §2.
#pragma once
#include "MenuSystem.h"

namespace mmi {

// The single root of the menu tree.
const MenuItem& menuRoot();

// Top-level index of the DIAGNOSTIC submenu (for the Traffic-button shortcut).
uint8_t diagnosticsTopIndex();

} // namespace mmi
