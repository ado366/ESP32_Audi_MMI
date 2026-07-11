// MenuTree.cpp — declarative nested menu tree (plan §2 menu tree).
// Leaves carry a MenuId that App dispatches to a screen/command.
#include "MenuTree.h"

namespace mmi {

#define LEAF(lbl, mid) MenuItem{ lbl, nullptr, 0, MenuId::mid }

// PHONE: the functions a user operates while driving.
static const MenuItem kPhone[] = {
  LEAF("PHONEBOOK",  BtPhonebook),
  LEAF("RECENT",     BtRecentCalls),
  LEAF("SWITCH DEV", BtSwitchDevice),
  LEAF("ACTIVE DEV", BtActiveDevice),
  LEAF("CALLS",      BtCalls),
};

// BLUETOOTH: setup / developer items, done once, not while driving.
static const MenuItem kBluetooth[] = {
  LEAF("ONE DEVICE", BtSingleDevice),
  LEAF("PAIR NEW",   BtPair),
  LEAF("RESET BC127",BtReset),
  LEAF("BC127 SET",  BtSettings),
  // Hidden (unbuilt, low value): AUTOCONNECT (BtAutoConnect).
};

static const MenuItem kDiagnostics[] = {
  LEAF("SELECT ECU", DiagSelectEcu),
  LEAF("SPEEDO",     DiagSpeedo),
  LEAF("FAVOURITES", DiagFavourites),
  LEAF("READ GROUP", DiagReadGroup),
  LEAF("READ FAULTS",DiagReadFaults),
  LEAF("GRAPH VALUE",DiagGraph),
  LEAF("TURBO",      DiagBoost),
  // Hidden (unbuilt, low value): DISP MODE (DiagDisplayMode), VAG-COM (DiagVagcom).
};

static const MenuItem kAdaptation[] = {
  LEAF("INIT PULSE", AdaptInit),
  LEAF("INTER-BYTE", AdaptByte),
  LEAF("INTER-FRAME",AdaptFrame),
};

static const MenuItem kDebug[] = {
  LEAF("BTN MONITOR",DbgButtonMonitor),
  LEAF("CALIBRATE",  DbgCalibrate),
  LEAF("MIC TEST",   DbgMicTest),
  LEAF("BC127",      DbgBc127),
  LEAF("ENCODER",    DbgEncoder),
  LEAF("FIS TEST",   DbgFisTest),
};

static const MenuItem kSettings[] = {
  LEAF("WIFI",       SetWifi),
  LEAF("UPDATE FW",  SetUpdate),
  LEAF("VERSION",    SetVersion),
  // Hidden (unbuilt FIS-Control extras, not v1 features): PARKING (SetParking),
  // UNITS (SetUnits), AUTOSTART (SetAutostart), TOP LINE (SetTopLine), LOGO (SetLogo).
};

#define SUBMENU(lbl, arr) MenuItem{ lbl, arr, static_cast<uint8_t>(sizeof(arr)/sizeof(arr[0])), MenuId::None }

static const MenuItem kMain[] = {
  SUBMENU("PHONE",      kPhone),      // user-facing drive-time functions
  SUBMENU("DIAGNOSTIC", kDiagnostics),
  SUBMENU("BLUETOOTH",  kBluetooth),  // setup / developer
  SUBMENU("ADAPTATION", kAdaptation),
  SUBMENU("DEBUG",      kDebug),
  SUBMENU("SETTINGS",   kSettings),
  LEAF("ECU INFO", EcuInfo),
  LEAF("EXIT",     Exit),
};

static const MenuItem kRoot = SUBMENU("MAIN MENU", kMain);

const MenuItem& menuRoot() { return kRoot; }

// Index of DIAGNOSTIC within the main menu (for the Traffic shortcut), found by
// its first child so it survives menu reordering.
uint8_t diagnosticsTopIndex() {
  for (uint8_t i = 0; i < kRoot.childCount; ++i) {
    const MenuItem& c = kRoot.children[i];
    if (c.childCount > 0 && c.children[0].id == MenuId::DiagSelectEcu) return i;
  }
  return 0;
}

} // namespace mmi
