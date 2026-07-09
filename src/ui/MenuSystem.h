// MenuSystem.h — generic nested-tree menu with rotary/button navigation.
// Pure logic + rendering via IDisplay; no hardware, unit-tested on native.
#pragma once
#include "../hal/IDisplay.h"
#include <cstdint>
#include <functional>

namespace mmi {

// Stable identifiers for leaf actions, so App can dispatch to screens/commands
// without the menu layer knowing what they do.
enum class MenuId : uint16_t {
  None = 0,
  // Bluetooth
  BtActiveDevice, BtSwitchDevice, BtPair, BtPhonebook, BtAutoConnect, BtReset, BtSettings, BtCalls, BtSingleDevice,
  // Diagnostics
  DiagFavourites, DiagReadGroup, DiagReadFaults, DiagGraph, DiagDisplayMode, DiagVagcom,
  // Adaptation
  AdaptInit, AdaptByte, AdaptFrame,
  // Debug
  DbgButtonMonitor, DbgCalibrate, DbgMicTest, DbgBc127, DbgEncoder, DbgFisTest,
  // Settings
  SetWifi, SetUpdate, SetParking, SetUnits, SetAutostart, SetTopLine, SetLogo, SetVersion,
  // Top-level leaves
  EcuInfo, Exit,
};

struct MenuItem {
  const char*     label;
  const MenuItem* children;   // nullptr for a leaf
  uint8_t         childCount;
  MenuId          id;         // meaningful for leaves
};

class MenuSystem {
public:
  explicit MenuSystem(const MenuItem& root);

  void reset();               // back to root, index 0
  void scrollDown();
  void scrollUp();
  void back();                // pop one level; returns to root at top level
  void toRoot();
  // Select current item: descends if it has children, else fires onSelect(id).
  void select();
  // Jump straight to a child of root by index (e.g. Traffic -> Diagnostics).
  void openTopLevel(uint8_t index);

  bool atRoot() const { return depth_ == 0; }
  const MenuItem& current() const;      // the highlighted item
  const MenuItem& level() const;        // the current level's parent
  uint8_t index() const { return index_[depth_]; }

  void onSelect(std::function<void(MenuId)> cb) { onSelect_ = std::move(cb); }
  void render(IDisplay& d) const;       // draws the current level

private:
  static constexpr int kMaxDepth = 6;
  const MenuItem*  stack_[kMaxDepth];   // stack_[d] = parent at depth d
  uint8_t          index_[kMaxDepth];   // highlighted index at depth d
  int              depth_ = 0;
  std::function<void(MenuId)> onSelect_;
};

} // namespace mmi
