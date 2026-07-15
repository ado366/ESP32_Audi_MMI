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
#include "hal/IRadio.h"
#include <string>
#include <vector>

namespace mmi {

class App {
public:
  App(IDisplay& display, IInputs& inputs, IBluetooth& bt, IStorage& storage, IDiag& diag);

  void begin();
  void tick(uint32_t nowMs);  // call from loop()/main loop with a millisecond clock
  Context context() const { return ctx_; }
  bool menuOpen() const { return menuOpen_; }
  // Last few dispatched actions ("<id>@<ms>", newest last) — /status telemetry to
  // catch phantom inputs (spontaneous screen exits / "hangs" with no user input).
  std::string actionTrace() const;

  // Seed points for the host (emulator/firmware) to populate data.
  BluetoothManager& bluetooth() { return btMgr_; }
  Phonebook&        phonebook() { return phonebook_; }
  PresetStore&      presets()   { return presets_; }
  void setSystem(ISystem* sys)  { sys_ = sys; }   // optional platform hook
  void setRadio(IRadio* radio)  { radio_ = radio; } // head-unit sniffer (passthrough)

private:
  // A leaf screen opened from the menu.
  enum class Screen : uint8_t {
    None, SwitchDevice, Phonebook, RecentCalls, MicTest, ButtonMonitor, EncoderMonitor, Bc127Debug,
    WifiInfo, UpdateInfo, OneDevice, NamePreset,
    DiagFavourites, DiagReadGroup, DiagGraph, DiagBoost, DiagFaults, Speedo,
    SelectEcu, // pick the KWP module to talk to
    Adapt,     // per-vehicle KWP timing adjuster (init pulse / inter-byte / inter-frame)
    Charset,   // ROM character explorer (raw byte -> glyph), for mapping accents
    Info   // generic title + text lines (version, confirmations, placeholders)
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
  bool canSwitchPhone() const;   // CD mode + paused + 2 phones -> encoder switches source
  void switchPhone(int dir);
  void mediaStep(int dir);       // next/prev: BT track, tuner seek, or phone switch by source
  void nowPlayingLines(std::string& l1, std::string& l2) const;  // home's top two rows; reused verbatim on gauge tops
  void cycleGauge();             // Traffic: one-touch ring Speedo <-> Favourites (turbo = Info)
  Screen lastGauge_ = Screen::Speedo;   // remembered so Traffic from home resumes your preferred gauge
  void seedDefaultGauges();      // seed useful favourites on first boot (so gauges work out-of-box)
  std::string fitRow(const std::string& label, const std::string& value) const;  // fit label+value to width
  void adaptAdjust(int dir);       // change the selected Adaptation timing field
  void adaptSave();                // persist + apply KWP timing
  void syncPhonebook();            // mirror phonebook_ from bt_.contacts() (PBAP)
  std::vector<std::string> wrapText(const std::string& s, int width, int maxLines) const;  // word-wrap
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
  Phonebook        callHistory_;   // recent calls (PBAP combined history)
  PresetStore      presets_;
  ISystem*         sys_ = nullptr;
  IRadio*          radio_ = nullptr;

  Context  ctx_ = Context::NowPlaying;
  bool     menuOpen_ = false;
  Screen   screen_ = Screen::None;
  int      listIndex_ = 0;
  uint8_t  micGain_ = 8;
  bool     micLoop_ = false;

  // Diagnostics working state
  Group              group_;          // last measuring group read
  std::vector<float> graph_;          // ring of samples for the graph view
  std::vector<float> graph2_;         // second series (dual-trace graph, dotted)
  int                graphVal_ = 0;   // which group value the graph plots (0..3)
  bool               graphDual_ = false; // also plot the NEXT value as a dotted trace
  std::string        readoutStr_;     // boost readout, rate-limited (text updates blink)
  uint32_t           readoutMs_ = 0;
  int                diagPresetIdx_ = 0;
  uint8_t            readEcu_ = ecu::Engine;
  uint8_t            readGroup_ = 2;
  std::vector<Dtc>   faults_;
  bool               faultsLoaded_ = false;
  bool               oneDevice_ = false;     // single-active-device enforcement (persisted)
  bool               speedoTest_ = false;    // speedo bench-test sweep 0..200
  int                adaptInit_ = 200;       // KWP 5-baud bit period (ms), persisted
  int                adaptByte_ = 0;         // KWP inter-byte W4 (ms), persisted
  int                adaptFrame_ = 0;        // KWP inter-frame W3 (ms), persisted
  int                adaptField_ = 0;        // which field the Adapt screen edits (0/1/2)
  uint8_t            charsetRow_ = 0xC0;     // Charset explorer: first code of the shown block
  uint32_t           lastSample_ = 0;
  // Add-to-favourites name picker (Maxi-K style)
  Preset             pendingPreset_;
  char               nameBuf_[9] = {0};
  int                namePos_ = 0;
  int                nameCharIdx_ = 0;

  // Generic Info screen contents (title + up to a few short lines).
  std::string              infoTitle_;
  std::vector<std::string> infoLines_;
  void showInfo(const char* title, std::vector<std::string> lines);

  bool     dirty_ = true;
  uint32_t now_ = 0;
  bool     scrolling_ = false;
  // Action-trace ring for /status (see actionTrace()).
  struct ActRec { uint32_t ms = 0; Action a = Action::None; };
  ActRec   actRing_[8];
  int      actIdx_ = 0;
  // Call UI state: who we dialled (for the outgoing screen), call-start time (for
  // the in-call timer), and the last CallState seen (to detect transitions).
  CallState   prevCall_ = CallState::Idle;
  uint32_t    callStartMs_ = 0;
  uint32_t    lastCallSec_ = 0;
  std::string dialedName_, dialedNumber_;
  // Auto-return to Now-Playing after inactivity in a menu / transient screen.
  uint32_t lastInputMs_ = 0;
  static constexpr uint32_t kHomeTimeoutMs = 20000;
  // Boot splash.
  uint32_t bootMs_ = 0;
  bool     splashDrawn_ = false;
  static constexpr uint32_t kSplashMs = 1600;
  void renderSplash();
  // Auto-switch the head unit to our aux/CD source when music starts playing.
  bool     prevPlaying_ = false;
  bool     wantAux_ = false;
  int      auxAttempts_ = 0;
  uint32_t auxNextMs_ = 0;
  void renderCall();
  std::string callParty() const;   // best name/number to show for the current call
  void dialParty(const std::string& name, const std::string& number);  // dial + remember who
  // Menu/list scroll rate-limit. A full FIS redraw takes ~50ms; scrolling faster
  // than it can clear+repaint makes the cluster drop writes (missing rows, or the
  // clear is lost so labels overlay). Ignore scroll steps that arrive too soon.
  uint32_t lastNav_ = 0;
  static constexpr uint32_t kNavCooldownMs = 150;
  bool navReady() { if (now_ - lastNav_ < kNavCooldownMs) return false; lastNav_ = now_; return true; }
  static constexpr int kWin = 8;
  static constexpr uint8_t kGaugeTop = 24;  // lower HALFSCREEN band origin (matches Esp32Display kHalfTop)
  static constexpr int kGraphW = 64;
};

} // namespace mmi
