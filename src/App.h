// App.h — the hardware-independent application core.
// Holds UI state and drives the HAL interfaces. Same object runs on the ESP32
// firmware and the native emulator; only the injected HAL impls differ.
#pragma once
#include "hal/IDisplay.h"
#include "hal/IInputs.h"
#include "hal/IBluetooth.h"
#include "hal/IStorage.h"
#include "hal/Types.h"
#include "ui/MenuSystem.h"
#include "bt/BluetoothManager.h"
#include "bt/Phonebook.h"
#include "diag/Diagnostics.h"
#include "diag/Presets.h"
#include "hal/ISystem.h"

namespace mmi {

class App {
public:
  App(IDisplay& display, IInputs& inputs, IBluetooth& bt, IStorage& storage, IDiag& diag);

  void begin();
  void tick(uint32_t nowMs);  // call from loop()/main loop with a millisecond clock
  Context context() const { return ctx_; }
  bool menuOpen() const { return menuOpen_; }

  // Seed points for the host (emulator/firmware) to populate data.
  BluetoothManager& bluetooth() { return btMgr_; }
  Phonebook&        phonebook() { return phonebook_; }
  PresetStore&      presets()   { return presets_; }
  void setSystem(ISystem* sys)  { sys_ = sys; }   // optional platform hook

private:
  // A leaf screen opened from the menu.
  enum class Screen : uint8_t {
    None, SwitchDevice, Phonebook, MicTest, ButtonMonitor, Bc127Debug,
    WifiInfo, UpdateInfo, OneDevice, NamePreset,
    DiagFavourites, DiagReadGroup, DiagGraph, DiagFaults
  };

  void handle(Action a);
  bool handleScreen(Action a);   // returns true if the action was consumed
  void onMenuSelect(MenuId id);
  void openScreen(Screen s);
  void screenSelect();
  void startAddFavourite();
  void finalizeName();
  void render();
  void renderScreen();
  void renderDiag();
  void renderCalibrate();
  void sampleDiag();             // periodic measuring-value read while a diag screen is open
  bool isDiagScreen() const;
  Context deriveContext() const;
  int  screenItemCount() const;
  std::string marquee(const std::string& s, int width = kWin) const;

  IDisplay&        display_;
  IInputs&         inputs_;
  IBluetooth&      bt_;
  IStorage&        storage_;
  IDiag&           diag_;
  MenuSystem       menu_;
  BluetoothManager btMgr_;
  Phonebook        phonebook_;
  PresetStore      presets_;
  ISystem*         sys_ = nullptr;

  Context  ctx_ = Context::NowPlaying;
  bool     menuOpen_ = false;
  Screen   screen_ = Screen::None;
  int      listIndex_ = 0;
  uint8_t  micGain_ = 8;
  bool     micLoop_ = false;

  // Diagnostics working state
  Group              group_;          // last measuring group read
  std::vector<float> graph_;          // ring of samples for the graph view
  int                diagPresetIdx_ = 0;
  uint8_t            readEcu_ = ecu::Engine;
  uint8_t            readGroup_ = 2;
  std::vector<Dtc>   faults_;
  bool               faultsLoaded_ = false;
  bool               oneDevice_ = false;     // single-active-device enforcement (persisted)
  uint32_t           lastSample_ = 0;
  // Add-to-favourites name picker (Maxi-K style)
  Preset             pendingPreset_;
  char               nameBuf_[9] = {0};
  int                namePos_ = 0;
  int                nameCharIdx_ = 0;

  bool     dirty_ = true;
  uint32_t now_ = 0;
  bool     scrolling_ = false;
  static constexpr int kWin = 8;
  static constexpr int kGraphW = 64;
};

} // namespace mmi
