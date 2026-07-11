// App.cpp — core UI loop, hardware-independent.
#include "App.h"
#include "Config.h"
#include "ui/InputRouter.h"
#include "ui/MenuTree.h"
#include "ui/GraphRenderer.h"
#include "diag/DtcDescriptions.h"
#include "diag/DtcElaboration.h"
#include "diag/SpeedoRenderer.h"
#include <cstdio>

namespace mmi {

App::App(IDisplay& display, IInputs& inputs, IBluetooth& bt, IStorage& storage, IDiag& diag)
  : display_(display), inputs_(inputs), bt_(bt), storage_(storage), diag_(diag),
    menu_(menuRoot()), btMgr_(bt, storage) {}

static const char* kCharset = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -+/.";
static constexpr int kCharsetLen = 41;

static std::string fmt(const Measurement& m) {
  if (!m.numeric()) return m.text;
  char b[24];
  float a = m.value < 0 ? -m.value : m.value;
  std::snprintf(b, sizeof(b), a >= 100 ? "%.0f%s" : "%.1f%s", m.value, m.unit.c_str());
  return b;
}

void App::begin() {
  bt_.begin();
  btMgr_.begin();
  presets_.load(storage_);
  oneDevice_ = storage_.getInt("bt.single", 0) != 0;  // default OFF
  bt_.setSingleDevice(oneDevice_);
  adaptInit_  = storage_.getInt("kwp.init",  200);    // per-vehicle KWP timing
  adaptByte_  = storage_.getInt("kwp.byte",  0);
  adaptFrame_ = storage_.getInt("kwp.frame", 0);
  diag_.setTiming(adaptInit_, adaptByte_, adaptFrame_);
  bt_.onStatusChanged([this]() { dirty_ = true; });
  menu_.onSelect([this](MenuId id) { onMenuSelect(id); });
  dirty_ = true;
  render();
}

Context App::deriveContext() const {
  switch (bt_.status().call) {
    case CallState::Incoming: return Context::IncomingCall;
    case CallState::Outgoing:
    case CallState::Active:   return Context::ActiveCall;
    default: break;
  }
  if (isDiagScreen())          return Context::Diagnostics;
  if (screen_ != Screen::None) return Context::Menu;
  return menuOpen_ ? Context::Menu : Context::NowPlaying;
}

// While parked on Now-Playing, in CD mode, with playback paused, the encoder
// switches which connected phone is the active one (so you can pick the source
// before pressing play). Only meaningful with 2+ phones connected at once.
bool App::canSwitchPhone() const {
  if (ctx_ != Context::NowPlaying) return false;
  if (bt_.status().playing) return false;               // only while paused
  if (radio_ && !radio_->cdMode()) return false;        // only in CD mode
  int connected = 0;
  for (const auto& d : bt_.pairedDevices()) if (d.connected) ++connected;
  return connected >= 2;
}

void App::switchPhone(int dir) {
  std::vector<BtDevice> conn;
  for (const auto& d : bt_.pairedDevices()) if (d.connected) conn.push_back(d);
  if (conn.size() < 2) return;
  int cur = -1;
  for (int i = 0; i < (int)conn.size(); ++i) if (conn[i].mac == bt_.status().activeDeviceMac) cur = i;
  int next = cur < 0 ? 0 : (cur + dir + (int)conn.size()) % (int)conn.size();
  bt_.connectDevice(conn[next].mac);   // already connected -> just makes it the active target
  btMgr_.onConnected(conn[next].mac);  // persist as last-used
  dirty_ = true;
}

// A "next/prev" gesture (Right steering button or encoder rotate on Now-Playing)
// means different things depending on the source:
//   - paused in CD mode with 2 phones -> pick the source phone (existing bonus);
//   - the OEM radio/tuner is the active source -> seek the tuner up/down;
//   - otherwise (our BT/aux source) -> change the Bluetooth track.
void App::mediaStep(int dir) {
  if (canSwitchPhone()) { switchPhone(dir); return; }
  if (radio_ && radio_->hasRemote() && !radio_->cdMode()) {
    dir > 0 ? radio_->tuneUp() : radio_->tuneDown();
  } else {
    dir > 0 ? bt_.trackNext() : bt_.trackPrev();
  }
}

bool App::isDiagScreen() const {
  return screen_ == Screen::DiagFavourites || screen_ == Screen::DiagReadGroup ||
         screen_ == Screen::DiagGraph || screen_ == Screen::DiagFaults ||
         screen_ == Screen::Speedo;
}

void App::tick(uint32_t nowMs) {
  now_ = nowMs;
  bt_.poll();
  if (radio_) { radio_->poll(); if (radio_->consumeChanged()) dirty_ = true; }

  // Keep the phonebook (browse list + caller-ID) mirrored from the PBAP download
  // as contacts stream in. Cheap size check; rebuild only when it changes.
  if (bt_.contactCount() != phonebook_.size()) { syncPhonebook(); dirty_ = true; }
  if (bt_.callHistoryCount() != callHistory_.size()) {   // recent-calls list
    callHistory_.clear();
    for (const auto& c : bt_.callHistory()) callHistory_.add(c.name, c.number, 200);
    dirty_ = true;
  }

  // Calibration takes over the screen; only the encoder long-press cancels it.
  if (inputs_.calibrating()) {
    InputEvent e;
    while (inputs_.poll(e)) if (e.control == Control::EncoderHold) inputs_.cancelCalibration();
    render();
    return;
  }

  ctx_ = deriveContext();

  // Rate-limit list/menu scrolling: each full-screen redraw takes ~50ms on the
  // FIS, and scrolling faster makes the cluster drop writes (missing rows / the
  // clear is lost so labels overlay). Scroll steps that arrive within the cooldown
  // are dropped; every other action passes straight through.
  auto dispatch = [&](Action a) {
    if ((a == Action::ScrollUp || a == Action::ScrollDown) && !navReady()) return;
    handle(a);
  };

  InputEvent ev;
  while (inputs_.poll(ev)) {
    if (ev.control == Control::EncoderCW || ev.control == Control::EncoderCCW) {
      int steps = ev.encoderDelta >= 0 ? ev.encoderDelta : -ev.encoderDelta;
      Control dir = ev.encoderDelta >= 0 ? Control::EncoderCW : Control::EncoderCCW;
      for (int i = 0; i < steps; ++i) dispatch(InputRouter::resolve(dir, ctx_));
    } else {
      dispatch(InputRouter::resolve(ev.control, ctx_));
    }
    ctx_ = deriveContext();
  }

  // Live diagnostics polling. Groups refresh fast; faults are read (async on
  // hardware) at a slower cadence.
  if (isDiagScreen()) {
    uint32_t interval = (screen_ == Screen::DiagFaults) ? 400u
                      : (screen_ == Screen::Speedo)     ? 250u    // FIS bitmap send is slow; don't outpace it
                      : 150u;
    if (now_ - lastSample_ > interval) {
      lastSample_ = now_;
      if (screen_ == Screen::DiagFaults) { if (diag_.readFaults(readEcu_, faults_)) faultsLoaded_ = true; }
      else sampleDiag();
      dirty_ = true;
    }
  }
  if (screen_ == Screen::ButtonMonitor || screen_ == Screen::Bc127Debug ||
      screen_ == Screen::DiagFaults || screen_ == Screen::Phonebook ||
      screen_ == Screen::RecentCalls) dirty_ = true; // live (marquee long names)
  // Speedo renders on its 150ms sample (isDiagScreen); the display redraws the
  // big bitmap only when the km/h value actually changes, so no per-tick churn.

  if (scrolling_) dirty_ = true;
  if (dirty_) render();
}

std::string App::marquee(const std::string& s, int width) const {
  if (static_cast<int>(s.size()) <= width) return s;
  // Read-friendly scroll: pause on the start, slide one char at a time to the end,
  // pause on the end, then reset. Feels far less jumpy than a continuous wrap.
  const int stepMs = 260, pause = 4;                 // pause = held steps at each end
  int extra = static_cast<int>(s.size()) - width;    // chars to scroll past
  int cycle = extra + pause * 2;
  int t = static_cast<int>((now_ / stepMs) % static_cast<uint32_t>(cycle));
  int off = t - pause;                               // hold at 0 during the first `pause` steps
  if (off < 0) off = 0; else if (off > extra) off = extra;
  return s.substr(off, width);
}

// ---- input handling ----

void App::handle(Action a) {
  if (screen_ != Screen::None && handleScreen(a)) return;

  switch (a) {
    case Action::MenuOpenClose:
      menuOpen_ = !menuOpen_;
      if (menuOpen_) menu_.reset(); else screen_ = Screen::None;
      dirty_ = true;
      break;
    case Action::JumpDiagnostics:
      menuOpen_ = true; screen_ = Screen::None;
      menu_.openTopLevel(diagnosticsTopIndex());
      dirty_ = true;
      break;
    case Action::JumpNowPlaying:
      menuOpen_ = false; screen_ = Screen::None;
      dirty_ = true;
      break;
    case Action::JumpSpeedo:
      menuOpen_ = false; openScreen(Screen::Speedo);   // Info button quick-recall
      break;
    case Action::ScrollDown: if (menuOpen_) { menu_.scrollDown(); dirty_ = true; } else if (canSwitchPhone()) switchPhone(+1); break;
    case Action::ScrollUp:   if (menuOpen_) { menu_.scrollUp();   dirty_ = true; } else if (canSwitchPhone()) switchPhone(-1); break;
    case Action::Select:     if (menuOpen_) { menu_.select();     dirty_ = true; } break;
    case Action::Back:
      if (menuOpen_) {
        if (menu_.atRoot()) menuOpen_ = false; else menu_.back();
        dirty_ = true;
      }
      break;
    case Action::RootBack:
      if (menuOpen_) { menu_.toRoot(); dirty_ = true; }
      break;
    // Volume always drives the head unit (it's the amplifier); fall back to the
    // BC127's own volume only when no radio remote is wired (e.g. the emulator).
    case Action::VolumeUp:   if (radio_ && radio_->hasRemote()) radio_->volumeUp();   else bt_.volumeUp();   break;
    case Action::VolumeDown: if (radio_ && radio_->hasRemote()) radio_->volumeDown(); else bt_.volumeDown(); break;
    case Action::TrackNext:  mediaStep(+1); break;
    case Action::TrackPrev:  mediaStep(-1); break;
    case Action::RadioSource: if (radio_ && radio_->hasRemote()) radio_->sourceMode(); break;
    case Action::PlayPause:  bt_.playPause();  break;
    case Action::CallAnswer: bt_.callAnswer(); dirty_ = true; break;
    case Action::CallReject: bt_.callReject(); dirty_ = true; break;
    case Action::CallEnd:    bt_.callEnd();    dirty_ = true; break;
    default: break;
  }
}

int App::screenItemCount() const {
  switch (screen_) {
    case Screen::SwitchDevice:   return static_cast<int>(bt_.pairedDevices().size());
    case Screen::Phonebook:      return static_cast<int>(phonebook_.size());
    case Screen::RecentCalls:    return static_cast<int>(callHistory_.size());
    case Screen::DiagFavourites: return presets_.size();
    case Screen::DiagFaults:     return static_cast<int>(faults_.size());
    case Screen::SelectEcu:      return ecu::kModuleCount;
    default: return 0;
  }
}

bool App::handleScreen(Action a) {
  if (screen_ == Screen::MicTest) {
    switch (a) {
      case Action::ScrollDown: if (micGain_ < 15) micGain_++; bt_.setMicGain(micGain_); dirty_ = true; return true;
      case Action::ScrollUp:   if (micGain_ > 0)  micGain_--; bt_.setMicGain(micGain_); dirty_ = true; return true;
      case Action::Select:     micLoop_ = !micLoop_; bt_.micLoopback(micLoop_); dirty_ = true; return true;
      case Action::Back:       micLoop_ = false; bt_.micLoopback(false); screen_ = Screen::None; dirty_ = true; return true;
      default: return false;
    }
  }
  if (screen_ == Screen::Info) {
    if (a == Action::Back || a == Action::Select) { screen_ = Screen::None; dirty_ = true; }
    return true; // consume nav
  }
  if (screen_ == Screen::Speedo) {
    if (a == Action::Back) { screen_ = Screen::None; speedoTest_ = false; dirty_ = true; }
    // Bench sweep disabled. Re-enable to check digit drawing without the car:
    // else if (a == Action::Select) { speedoTest_ = !speedoTest_; dirty_ = true; }  // toggle 0-200 sweep
    return true; // consume nav
  }
  if (screen_ == Screen::Adapt) {
    switch (a) {
      case Action::ScrollDown: adaptAdjust(+1); return true;   // CW = increase
      case Action::ScrollUp:   adaptAdjust(-1); return true;
      case Action::Select:
      case Action::Back:       adaptSave(); screen_ = Screen::None; dirty_ = true; return true;
      default: return true;   // consume nav so it doesn't leak to the menu
    }
  }
  if (screen_ == Screen::Charset) {
    if (a == Action::ScrollDown && charsetRow_ < 0xF0) { charsetRow_ += 0x10; dirty_ = true; }
    else if (a == Action::ScrollUp && charsetRow_ > 0x00) { charsetRow_ -= 0x10; dirty_ = true; }
    else if (a == Action::Back) { screen_ = Screen::None; dirty_ = true; }
    return true;
  }
  if (screen_ == Screen::ButtonMonitor || screen_ == Screen::Bc127Debug ||
      screen_ == Screen::WifiInfo || screen_ == Screen::UpdateInfo || screen_ == Screen::OneDevice) {
    if (a == Action::Back) { screen_ = Screen::None; dirty_ = true; }
    else if (a == Action::Select && screen_ == Screen::UpdateInfo && sys_) sys_->pullUpdate();
    else if (a == Action::Select && screen_ == Screen::OneDevice) {
      oneDevice_ = !oneDevice_;
      storage_.putInt("bt.single", oneDevice_ ? 1 : 0); storage_.commit();
      bt_.setSingleDevice(oneDevice_);
      dirty_ = true;
    }
    return true; // consume nav so it doesn't leak to the menu
  }
  if (screen_ == Screen::DiagFavourites) {
    int n = presets_.size();
    switch (a) {
      case Action::ScrollDown: if (n) { diagPresetIdx_ = (diagPresetIdx_ + 1) % n; graph_.clear(); dirty_ = true; } return true;
      case Action::ScrollUp:   if (n) { diagPresetIdx_ = (diagPresetIdx_ + n - 1) % n; graph_.clear(); dirty_ = true; } return true;
      case Action::Select:     // cycle this preset's view (TopLine->MultiValue->Graph->Boost) and save
        if (n) { Preset& p = presets_.at(diagPresetIdx_);
                 p.view = static_cast<View>(((int)p.view % 4) + 1); // 1..4
                 presets_.save(storage_); graph_.clear(); dirty_ = true; }
        return true;
      case Action::RootBack:   // delete current preset
        if (n) { presets_.removeAt(diagPresetIdx_); presets_.save(storage_);
                 if (diagPresetIdx_ >= presets_.size() && diagPresetIdx_ > 0) diagPresetIdx_--; dirty_ = true; }
        return true;
      case Action::Back:       screen_ = Screen::None; dirty_ = true; return true;
      default: return false;
    }
  }
  if (screen_ == Screen::DiagReadGroup || screen_ == Screen::DiagGraph) {
    switch (a) {
      case Action::ScrollDown: readGroup_++; group_.count = 0; graph_.clear(); dirty_ = true; return true;
      case Action::ScrollUp:   if (readGroup_ > 1) readGroup_--; group_.count = 0; graph_.clear(); dirty_ = true; return true;
      case Action::Select:     startAddFavourite(); return true;   // add current group as a favourite
      case Action::Back:       screen_ = Screen::None; dirty_ = true; return true;
      default: return false;
    }
  }
  if (screen_ == Screen::NamePreset) {
    switch (a) {
      case Action::ScrollDown: nameCharIdx_ = (nameCharIdx_ + 1) % kCharsetLen; dirty_ = true; return true;
      case Action::ScrollUp:   nameCharIdx_ = (nameCharIdx_ + kCharsetLen - 1) % kCharsetLen; dirty_ = true; return true;
      case Action::Select:     // place the current character, advance
        if (namePos_ < 8) { nameBuf_[namePos_++] = kCharset[nameCharIdx_]; nameCharIdx_ = 0; }
        if (namePos_ >= 8) finalizeName();
        dirty_ = true; return true;
      case Action::RootBack:   finalizeName(); return true;         // finish early (pad with spaces)
      case Action::Back:       screen_ = Screen::None; dirty_ = true; return true; // cancel
      default: return false;
    }
  }
  if (screen_ == Screen::DiagFaults) {
    int n = static_cast<int>(faults_.size());     // items = faults + a "CLEAR ALL" row
    switch (a) {
      case Action::ScrollDown: if (faultsLoaded_) { listIndex_ = (listIndex_ + 1) % (n + 1); dirty_ = true; } return true;
      case Action::ScrollUp:   if (faultsLoaded_) { listIndex_ = (listIndex_ + n) % (n + 1); dirty_ = true; } return true;
      case Action::Select:     // clicking the CLEAR ALL row clears; on a fault, no-op
        if (faultsLoaded_ && listIndex_ == n) { diag_.clearFaults(readEcu_); faultsLoaded_ = false; faults_.clear(); listIndex_ = 0; dirty_ = true; }
        return true;
      case Action::Back:       screen_ = Screen::None; dirty_ = true; return true;
      default: return false;
    }
  }
  // List screens (SwitchDevice / Phonebook)
  int n = screenItemCount();
  switch (a) {
    case Action::ScrollDown: if (n) { listIndex_ = (listIndex_ + 1) % n; dirty_ = true; } return true;
    case Action::ScrollUp:   if (n) { listIndex_ = (listIndex_ + n - 1) % n; dirty_ = true; } return true;
    case Action::Select:     screenSelect(); return true;
    case Action::Back:       screen_ = Screen::None; dirty_ = true; return true;
    default: return false;
  }
}

void App::screenSelect() {
  if (screen_ == Screen::SwitchDevice) {
    auto p = bt_.pairedDevices();
    if (listIndex_ < static_cast<int>(p.size())) {
      bt_.connectDevice(p[listIndex_].mac);   // single link enforced downstream
      btMgr_.onConnected(p[listIndex_].mac);   // persist last-used
    }
    screen_ = Screen::None;
  } else if (screen_ == Screen::Phonebook) {
    const auto& e = phonebook_.entries();
    if (listIndex_ < static_cast<int>(e.size())) bt_.dial(e[listIndex_].number);
    screen_ = Screen::None;
  } else if (screen_ == Screen::RecentCalls) {
    const auto& e = callHistory_.entries();
    if (listIndex_ < static_cast<int>(e.size())) bt_.dial(e[listIndex_].number);   // redial
    screen_ = Screen::None;
  } else if (screen_ == Screen::SelectEcu) {
    if (listIndex_ >= 0 && listIndex_ < ecu::kModuleCount) readEcu_ = ecu::kModules[listIndex_].addr;
    group_.count = 0; faultsLoaded_ = false; faults_.clear();   // stale data belongs to the old module
    screen_ = Screen::None;
  }
  dirty_ = true;
}

void App::startAddFavourite() {
  pendingPreset_ = Preset{};
  pendingPreset_.ecu = readEcu_;
  pendingPreset_.group = readGroup_;
  pendingPreset_.valueIndex = 0;
  pendingPreset_.view = View::MultiValue;
  pendingPreset_.min = 0; pendingPreset_.max = 100;
  for (int i = 0; i < 9; ++i) nameBuf_[i] = 0;
  namePos_ = 0; nameCharIdx_ = 0;
  openScreen(Screen::NamePreset);
}

void App::finalizeName() {
  while (namePos_ < 8) nameBuf_[namePos_++] = ' ';
  nameBuf_[8] = 0;
  // trim trailing spaces for the stored label
  int end = 8; while (end > 0 && nameBuf_[end - 1] == ' ') --end;
  for (int i = 0; i < 9; ++i) pendingPreset_.label[i] = 0;
  for (int i = 0; i < end; ++i) pendingPreset_.label[i] = nameBuf_[i];
  presets_.add(pendingPreset_);
  presets_.save(storage_);
  screen_ = Screen::None;
  dirty_ = true;
}

void App::openScreen(Screen s) {
  screen_ = s; listIndex_ = 0; graph_.clear(); lastSample_ = 0; group_.count = 0;  // no stale values
  if (s == Screen::DiagFaults) { faultsLoaded_ = false; faults_.clear(); diag_.readFaults(readEcu_, faults_); }
  dirty_ = true;
}

void App::onMenuSelect(MenuId id) {
  const BtStatus& s = bt_.status();
  switch (id) {
    // ---- Phone / Bluetooth ----
    case MenuId::BtSwitchDevice: bt_.refreshDevices(); openScreen(Screen::SwitchDevice); break;
    case MenuId::BtPhonebook:    openScreen(Screen::Phonebook); bt_.pullPhonebook(); break;   // PBAP contacts
    case MenuId::BtRecentCalls:  openScreen(Screen::RecentCalls); bt_.pullCallHistory(); break; // PBAP call history
    case MenuId::BtActiveDevice:
      showInfo("ACTIVE DEV", { s.linked ? (s.activeDeviceName.empty() ? "PHONE" : s.activeDeviceName) : "NO PHONE",
                               s.activeDeviceMac.empty() ? "" : s.activeDeviceMac });
      break;
    case MenuId::BtSingleDevice: openScreen(Screen::OneDevice);      break;
    case MenuId::BtPair:         bt_.sendCommand("BT_STATE ON ON");
                                 showInfo("PAIR NEW", {"DISCOVERABLE", "PAIR FROM PHONE", "THEN SWITCH DEV"}); break;
    case MenuId::BtReset:        bt_.sendCommand("RESET"); showInfo("BC127", {"RESETTING..."}); break;
    case MenuId::BtSettings:     openScreen(Screen::Bc127Debug);     break;  // raw command console
    case MenuId::BtCalls:
      showInfo("CALLS", { s.call == CallState::Idle ? "NO ACTIVE CALL" : "IN CALL",
                          "ENC CLICK=ANSWER", "ENC HOLD=END" });
      break;

    // ---- Diagnostics ----
    case MenuId::DiagSelectEcu:
      openScreen(Screen::SelectEcu);
      for (int i = 0; i < ecu::kModuleCount; ++i) if (ecu::kModules[i].addr == readEcu_) listIndex_ = i;
      break;
    case MenuId::DiagSpeedo:     openScreen(Screen::Speedo);         break;
    case MenuId::DiagFavourites: openScreen(Screen::DiagFavourites); break;
    case MenuId::DiagReadGroup:  readGroup_ = 2; openScreen(Screen::DiagReadGroup); break;
    case MenuId::DiagGraph:      readGroup_ = 2; openScreen(Screen::DiagGraph);     break;
    case MenuId::DiagReadFaults: openScreen(Screen::DiagFaults);     break;

    // ---- Adaptation (per-vehicle KWP timing) ----
    case MenuId::AdaptInit:      adaptField_ = 0; openScreen(Screen::Adapt); break;
    case MenuId::AdaptByte:      adaptField_ = 1; openScreen(Screen::Adapt); break;
    case MenuId::AdaptFrame:     adaptField_ = 2; openScreen(Screen::Adapt); break;

    // ---- Debug ----
    case MenuId::DbgMicTest:      openScreen(Screen::MicTest);        break;
    case MenuId::DbgButtonMonitor:openScreen(Screen::ButtonMonitor);  break;
    case MenuId::DbgEncoder:      openScreen(Screen::ButtonMonitor);  break; // shows encoder + ladders
    case MenuId::DbgBc127:        openScreen(Screen::Bc127Debug);     break;
    case MenuId::DbgCalibrate:    inputs_.startCalibration();         break;
    case MenuId::DbgFisTest:      charsetRow_ = 0xC0; openScreen(Screen::Charset); break;  // ROM charset explorer

    // ---- Settings ----
    case MenuId::SetWifi:        openScreen(Screen::WifiInfo);        break;
    case MenuId::SetUpdate:      openScreen(Screen::UpdateInfo);      break;
    case MenuId::SetVersion:     showInfo("VERSION", { std::string("FW ") + cfg::FW_VERSION, "ESP32 AUDI MMI" }); break;

    case MenuId::Exit:           menuOpen_ = false;                   break;

    // ---- Not yet implemented: show a clear placeholder, not a dead key ----
    default:
      showInfo("NOT READY", {"NOT IMPLEMENTED", "YET"});
      break;
  }
  dirty_ = true;
}

void App::showInfo(const char* title, std::vector<std::string> lines) {
  infoTitle_ = title; infoLines_ = std::move(lines);
  screen_ = Screen::Info; dirty_ = true;
}

// Adaptation timing editor. Field 0=init pulse, 1=inter-byte, 2=inter-frame.
void App::adaptAdjust(int dir) {
  int* v; int lo, hi, step;
  switch (adaptField_) {
    case 0:  v = &adaptInit_;  lo = 180; hi = 220; step = 1; break;   // 5-baud bit period
    case 1:  v = &adaptByte_;  lo = 0;   hi = 25;  step = 1; break;   // inter-byte W4
    default: v = &adaptFrame_; lo = 0;   hi = 100; step = 5; break;   // inter-frame W3
  }
  *v += dir * step;
  if (*v < lo) *v = lo; if (*v > hi) *v = hi;
  diag_.setTiming(adaptInit_, adaptByte_, adaptFrame_);               // live-apply while adjusting
  dirty_ = true;
}

void App::adaptSave() {
  storage_.putInt("kwp.init",  adaptInit_);
  storage_.putInt("kwp.byte",  adaptByte_);
  storage_.putInt("kwp.frame", adaptFrame_);
  storage_.commit();
  diag_.setTiming(adaptInit_, adaptByte_, adaptFrame_);
}

// Mirror the App phonebook (used for the browse list + caller-ID lookup) from the
// BT layer's PBAP download. Called when the contact count changes.
void App::syncPhonebook() {
  phonebook_.clear();
  for (const auto& c : bt_.contacts()) phonebook_.add(c.name, c.number, 500);
  phonebook_.sortByName();          // alphabetical by first name
}

// ---- diagnostics sampling + rendering ----

void App::sampleDiag() {
  uint8_t e = readEcu_, g = readGroup_;
  int vi = 0;
  if (screen_ == Screen::DiagFavourites && presets_.size() > 0) {
    const Preset& p = presets_.at(diagPresetIdx_);
    e = p.ecu; g = p.group; vi = p.valueIndex;
  } else if (screen_ == Screen::Speedo) {
    e = ecu::Dashboard; g = 1; vi = 0;              // speed = cluster group 1, value 1
  }
  if (!diag_.readGroup(e, g, group_)) return;
  if (vi < group_.count) {
    graph_.push_back(group_.values[vi].value);
    if (static_cast<int>(graph_.size()) > kGraphW) graph_.erase(graph_.begin());
  }
}

void App::renderDiag() {
  char l[24];

  if (screen_ == Screen::Speedo) {
    display_.beginFullScreen(true);
    int spd;
    if (speedoTest_) {                               // sweep 0..200..0 for on-bench checking
      int p = static_cast<int>((now_ / 50) % 402u);
      spd = p < 201 ? p : 401 - p;
    } else {
      spd = group_.count > 0 ? static_cast<int>(group_.values[0].value + 0.5f) : 0;
    }
    display_.drawText(0, 12, kFontCentered, "KM/H");     // top 1/3
    auto bmp = SpeedoRenderer::render(spd, 64, 20);
    display_.drawBitmap(0, 46, 64, 20, bmp.data());      // bottom 2/3, 7-seg digits (~half size)
    return;
  }

  if (screen_ == Screen::DiagFaults) {
    display_.beginFullScreen(true);
    int n = static_cast<int>(faults_.size());
    display_.drawText(0, 0, kFontCentered, "FAULTS");
    if (!faultsLoaded_) { display_.drawText(0, 10, kFontCentered, "READING..."); return; }
    if (faults_.empty()) { display_.drawText(0, 10, kFontCentered, "NONE FOUND"); return; }

    // Header line 2 = position (replaces a scrollbar bitmap, which forced a full
    // redraw/flash on every scroll — this is plain text, so only the row updates).
    int sel = listIndex_ < n ? listIndex_ + 1 : n;
    std::snprintf(l, sizeof(l), "FOUND %d/%d", sel, n);
    display_.drawText(0, 10, kFontCentered, l);

    // Fault-code list, starting at y=25, kept above the ~2/3 split line.
    const int total = n + 1;                          // + CLEAR ALL row
    const int visible = 3, listTop = 25, rowH = 10;
    int start = listIndex_ - visible / 2;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible < 0 ? 0 : total - visible;
    for (int r = 0; r < visible && start + r < total; ++r) {
      int i = start + r;
      if (i < n) std::snprintf(l, sizeof(l), " %05u%s", faults_[i].code, faults_[i].sporadic ? " *" : "");
      else       std::snprintf(l, sizeof(l), " CLEAR ALL");
      display_.drawText(0, static_cast<uint8_t>(listTop + r * rowH),
                        i == listIndex_ ? (kFontCompressedLeft | kFontHighlight) : kFontCompressedLeft, l);
    }
    // Selected fault's description + elaboration below the split. ALWAYS emit 3
    // rows (blank where unused) so the frame structure is identical across faults
    // — otherwise the op count changes on scroll and forces a full redraw (flash).
    // Row 3 marquees the remainder so the full "what - how" text stays readable.
    std::string dl0 = " ", dl1 = " ", dl2 = " ";
    if (listIndex_ < n) {
      const char* desc = dtcDescription(faults_[listIndex_].code);
      const char* elab = dtcElaboration(faults_[listIndex_].info);
      std::string full = desc ? desc : "NO DESCRIPTION";
      if (elab) { full += " - "; full += elab; }          // what (- how it fails)
      auto lines = wrapText(full, 15, 8);
      if (lines.size() >= 1) dl0 = lines[0];
      if (lines.size() >= 2) dl1 = lines[1];
      if (lines.size() == 3) dl2 = lines[2];
      else if (lines.size() > 3) {
        std::string rest = lines[2];
        for (size_t i = 3; i < lines.size(); ++i) rest += " " + lines[i];
        dl2 = marquee(rest, 15);
      }
    }
    display_.drawText(0, 64, kFontCompressedLeft, dl0.c_str());
    display_.drawText(0, 72, kFontCompressedLeft, dl1.c_str());
    display_.drawText(0, 80, kFontCompressedLeft, dl2.c_str());
    return;
  }

  // Favourites use the preset's view; Read-group is multi-value; Graph is a graph.
  View view = View::MultiValue;
  const char* header = "READ GRP";
  int valueIndex = 0;
  if (screen_ == Screen::DiagFavourites && presets_.size() > 0) {
    const Preset& p = presets_.at(diagPresetIdx_);
    view = p.view; valueIndex = p.valueIndex;
    header = p.label[0] ? p.label : "FAV";
  } else if (screen_ == Screen::DiagGraph) {
    view = View::Graph;
  }

  if (view == View::TopLine) {
    Measurement m = valueIndex < group_.count ? group_.values[valueIndex] : Measurement{};
    display_.showTopLines(header, fmt(m).c_str());
    return;
  }

  if (view == View::Boost) {
    float mn = 0, mx = 2.5f;
    if (screen_ == Screen::DiagFavourites && presets_.size() > 0) {
      const Preset& p = presets_.at(diagPresetIdx_); mn = p.min; mx = p.max;
    }
    Measurement m = valueIndex < group_.count ? group_.values[valueIndex] : Measurement{};
    float frac = (mx > mn) ? (m.value - mn) / (mx - mn) : 0.f;
    auto bar = GraphRenderer::renderBar(frac, 60, 16);
    display_.beginFullScreen(true);
    std::snprintf(l, sizeof(l), "%s %s", header, fmt(m).c_str());
    display_.drawText(0, 0, kFontCompressedLeft, l);
    display_.drawText(0, 14, kFontCompressedLeft, "TURBO");
    display_.drawBitmap(2, 26, 60, 16, bar.data());
    std::snprintf(l, sizeof(l), "%.1f", mn); display_.drawText(0, 48, kFontCompressedLeft, l);
    std::snprintf(l, sizeof(l), "%.1f BAR", mx); display_.drawText(30, 48, kFontCompressedLeft, l);
    return;
  }

  if (view == View::Graph) {
    Preset def; float mn = 0, mx = 5000, g1 = -1e9f, g2 = -1e9f;
    if (screen_ == Screen::DiagFavourites && presets_.size() > 0) {
      const Preset& p = presets_.at(diagPresetIdx_); mn = p.min; mx = p.max; g1 = p.guide1; g2 = p.guide2;
    }
    auto bmp = GraphRenderer::render(graph_, mn, mx, kGraphW, 48, g1, g2);
    display_.beginFullScreen(true);
    Measurement m = valueIndex < group_.count ? group_.values[valueIndex] : Measurement{};
    std::snprintf(l, sizeof(l), "%s %s", header, fmt(m).c_str());
    display_.drawText(0, 0, kFontCompressedLeft, l);
    display_.drawBitmap(0, 16, kGraphW, 48, bmp.data());
    return;
  }

  // MultiValue: header (module + group), then description/value on separate rows.
  display_.beginFullScreen(true);
  if (screen_ == Screen::DiagReadGroup) {
    display_.drawText(0, 0, kFontCentered, ecu::moduleName(readEcu_));         // which module
    std::snprintf(l, sizeof(l), "GROUP %u", static_cast<unsigned>(readGroup_));
    display_.drawText(0, 9, kFontCompressedCenter, l);                         // compressed fits 2-3 digits
  } else {
    display_.drawText(0, 0, kFontCentered, header);                           // favourite label
  }
  // No fresh data for this group (just switched, empty block, or read failed) —
  // show NO DATA rather than the previous group's stale values.
  if (group_.count == 0) { display_.drawText(0, 44, kFontCentered, "NO DATA"); return; }
  const int kDiagTop = 25, kLineH = 8;   // 7px glyph + 1px gap between every line
  for (int i = 0; i < group_.count && i < 4; ++i) {
    int y = kDiagTop + i * kLineH * 2;
    if (y + kLineH + 7 > 88) break;
    std::string val = fmt(group_.values[i]);
    int vx = 64 - static_cast<int>(val.size()) * 22 / 5;  // right-align flush (~4.4px/compressed char)
    if (vx < 0) vx = 0;
    display_.drawText(0, static_cast<uint8_t>(y),          kFontCompressedLeft, group_.values[i].label.c_str()); // label (left)
    display_.drawText(static_cast<uint8_t>(vx), static_cast<uint8_t>(y + kLineH), kFontCompressedLeft, val.c_str()); // value (right)
  }
}

// Fit "label value" into the compressed-font row width: keep the value in full
// (it's the point), truncate the label to fill the rest; if the value alone is
// too wide, marquee it. ~15 compressed chars fit across the 64px screen.
// Word-wrap a string into up to maxLines rows of at most `width` chars each.
// Long words are hard-split; text past maxLines is dropped (with a trailing "…").
std::vector<std::string> App::wrapText(const std::string& s, int width, int maxLines) const {
  std::vector<std::string> out;
  std::string cur;
  size_t i = 0;
  while (i < s.size() && (int)out.size() < maxLines) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i; while (j < s.size() && s[j] != ' ') ++j;
    std::string word = s.substr(i, j - i); i = j;
    if (word.empty()) continue;
    while ((int)word.size() > width) {                 // hard-split over-long word
      if (cur.empty()) { out.push_back(word.substr(0, width)); word = word.substr(width); }
      else { out.push_back(cur); cur.clear(); }
      if ((int)out.size() >= maxLines) break;
    }
    if ((int)out.size() >= maxLines) break;
    if (cur.empty()) cur = word;
    else if ((int)(cur.size() + 1 + word.size()) <= width) cur += " " + word;
    else { out.push_back(cur); cur = word; }
  }
  if (!cur.empty() && (int)out.size() < maxLines) out.push_back(cur);
  if (i < s.size() && !out.empty()) {                  // more text than fits -> mark it
    std::string& last = out.back();
    if ((int)last.size() > width - 2) last = last.substr(0, width - 2);
    last += "..";
  }
  return out;
}

std::string App::fitRow(const std::string& label, const std::string& value) const {
  const int kCols = 15;
  if ((int)value.size() >= kCols) return marquee(value, kCols);
  int labMax = kCols - (int)value.size() - 1;
  std::string lab = (int)label.size() > labMax ? label.substr(0, labMax) : label;
  return lab + " " + value;
}

void App::renderScreen() {
  if (isDiagScreen()) { renderDiag(); return; }

  display_.beginFullScreen(true);
  if (screen_ == Screen::Info) {
    display_.drawText(0, 0, kFontCentered, infoTitle_.c_str());
    for (size_t i = 0; i < infoLines_.size() && i < 6; ++i)
      if (!infoLines_[i].empty())
        display_.drawText(0, static_cast<uint8_t>(20 + i * 10), kFontCompressedLeft, infoLines_[i].c_str());
    return;
  }
  if (screen_ == Screen::MicTest) {
    const BtStatus& st = bt_.status();
    display_.drawText(0, 0, kFontCentered, "MIC TEST");
    char l[24];
    std::snprintf(l, sizeof(l), "SCO  %s", st.scoOpen ? "ON" : "OFF");
    display_.drawText(0, 20, kFontCompressedLeft, l);
    std::snprintf(l, sizeof(l), "GAIN %d", micGain_);
    display_.drawText(0, 32, kFontCompressedLeft, l);
    display_.drawText(0, 48, kFontCompressedLeft, micLoop_ ? "SEL=STOP LOOP" : "SEL=LOOPBACK");
    display_.drawText(0, 60, kFontCompressedLeft, "ROT=GAIN");
    return;
  }
  if (screen_ == Screen::Adapt) {
    const char* name = adaptField_ == 0 ? "INIT PULSE"
                     : adaptField_ == 1 ? "INTER-BYTE" : "INTER-FRAME";
    int val = adaptField_ == 0 ? adaptInit_ : adaptField_ == 1 ? adaptByte_ : adaptFrame_;
    display_.drawText(0, 0, kFontCentered, name);
    char l[24];
    std::snprintf(l, sizeof(l), "%d MS", val);
    display_.drawText(0, 28, kFontCentered, l);
    display_.drawText(0, 60, kFontCompressedLeft, "ROT=ADJUST");
    display_.drawText(0, 72, kFontCompressedLeft, "SEL=SAVE");
    return;
  }
  if (screen_ == Screen::Charset) {
    char hdr[16];
    std::snprintf(hdr, sizeof(hdr), "ROM %02X-%02X", charsetRow_, charsetRow_ + 15);
    display_.drawText(0, 0, kFontCentered, hdr);
    // ONE op per line (the FIS clears the whole 64px row per text op, so multiple
    // ops on the same y would erase each other). Pack "XX g" cells into one raw
    // string — the hex label chars are ASCII and pass through unmapped too.
    for (int line = 0; line < 8; ++line) {
      std::string s;
      for (int k = 0; k < 2; ++k) {
        uint8_t code = static_cast<uint8_t>(charsetRow_ + line * 2 + k);
        char lbl[4]; std::snprintf(lbl, sizeof(lbl), "%02X", code);
        s += lbl; s += ' '; s += static_cast<char>(code); s += "  ";
      }
      display_.drawTextRaw(0, static_cast<uint8_t>(10 + line * 9), kFontCompressedLeft, s.c_str());
    }
    return;
  }
  if (screen_ == Screen::ButtonMonitor) {
    display_.drawText(0, 0, kFontCentered, "BTN MON");
    char l[24];
    std::snprintf(l, sizeof(l), "CON1 %d", inputs_.rawLadder(0)); display_.drawText(0, 20, kFontCompressedLeft, l);
    std::snprintf(l, sizeof(l), "CON2 %d", inputs_.rawLadder(1)); display_.drawText(0, 32, kFontCompressedLeft, l);
    std::snprintf(l, sizeof(l), "STEER %d", inputs_.rawLadder(2)); display_.drawText(0, 44, kFontCompressedLeft, l);
    return;
  }
  if (screen_ == Screen::Bc127Debug) {
    const BtStatus& st = bt_.status();
    display_.drawText(0, 0, kFontCentered, "BC127");
    char l[24];
    std::snprintf(l, sizeof(l), "LINK %s", st.linked ? st.activeDeviceName.c_str() : "-");
    display_.drawText(0, 14, kFontCompressedLeft, l);
    std::snprintf(l, sizeof(l), "PLAY %d CALL %d", st.playing ? 1 : 0, (int)st.call);
    display_.drawText(0, 24, kFontCompressedLeft, l);
    // Last 4 lines of BC127 traffic (newest at the bottom).
    std::string log = bt_.debugLog();
    std::string ring[4]; int total = 0;
    size_t pos = 0;
    while (pos < log.size()) {
      size_t nlp = log.find('\n', pos);
      std::string ln = log.substr(pos, nlp == std::string::npos ? std::string::npos : nlp - pos);
      if (!ln.empty()) ring[total++ % 4] = ln;
      if (nlp == std::string::npos) break;
      pos = nlp + 1;
    }
    int show = total < 4 ? total : 4;
    for (int i = 0; i < show; ++i) {
      const std::string& ln = ring[(total - show + i) % 4];
      display_.drawText(0, static_cast<uint8_t>(36 + i * 8), kFontCompressedLeft, ln.c_str());
    }
    return;
  }
  if (screen_ == Screen::NamePreset) {
    display_.drawText(0, 0, kFontCentered, "NAME");
    // Build the name-so-far with the character being chosen shown at the cursor.
    char shown[10];
    for (int i = 0; i < 8; ++i)
      shown[i] = (i < namePos_) ? nameBuf_[i] : (i == namePos_ ? kCharset[nameCharIdx_] : ' ');
    shown[8] = 0;
    display_.drawText(4, 20, kFontCentered, shown);
    // caret under the current position
    char caret[10]; for (int i = 0; i < 8; ++i) caret[i] = (i == namePos_) ? '^' : ' '; caret[8] = 0;
    display_.drawText(4, 30, kFontCentered, caret);
    display_.drawText(0, 50, kFontCompressedLeft, "ROT=CHAR CLK=OK");
    display_.drawText(0, 60, kFontCompressedLeft, "HOLD=DONE RET=X");
    return;
  }
  if (screen_ == Screen::OneDevice) {
    display_.drawText(0, 0, kFontCentered, "ONE DEVICE");
    display_.drawText(0, 22, kFontCompressedLeft, oneDevice_ ? "STATE  ENABLED" : "STATE  DISABLED");
    display_.drawText(0, 40, kFontCompressedLeft, "KEEP LAST DEVICE");
    display_.drawText(0, 60, kFontCompressedLeft, "SEL=TOGGLE");
    return;
  }
  if (screen_ == Screen::WifiInfo || screen_ == Screen::UpdateInfo) {
    bool upd = screen_ == Screen::UpdateInfo;
    display_.drawText(0, 0, kFontCentered, upd ? "UPDATE" : "WIFI");
    std::string info = !sys_ ? std::string("N/A")
                     : (upd ? sys_->updateInfo() : sys_->wifiInfo());
    int row = 0; size_t pos = 0;
    while (pos <= info.size() && row < 6) {
      size_t nl = info.find('\n', pos);
      std::string ln = info.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      display_.drawText(0, static_cast<uint8_t>(16 + row * 9), kFontCompressedLeft, ln.c_str());
      ++row;
      if (nl == std::string::npos) break;
      pos = nl + 1;
    }
    if (upd && sys_) display_.drawText(0, 70, kFontCompressedLeft, "SEL=PULL UPDATE");
    return;
  }
  // Phonebook shows WHICH phone the contacts belong to; RecentCalls is labelled.
  bool isPb = screen_ == Screen::Phonebook, isRc = screen_ == Screen::RecentCalls;
  std::string hdr = screen_ == Screen::SwitchDevice ? "SWITCH DEV"
                  : screen_ == Screen::SelectEcu ? "SELECT ECU"
                  : isRc ? "RECENT" : "PHONEBOOK";
  if (isPb) {
    std::string src = bt_.contactsSource();
    if (src.empty()) src = bt_.status().activeDeviceName;
    if (!src.empty()) hdr = src.substr(0, 10);   // e.g. "ONEPLUS 9"
  }
  display_.drawText(0, 0, kFontCentered, hdr.c_str());
  auto devs = bt_.pairedDevices();
  int n = screen_ == Screen::SwitchDevice ? static_cast<int>(devs.size()) : screenItemCount();
  if (screen_ == Screen::SwitchDevice && n == 0) { display_.drawText(0, 24, kFontCompressedLeft, "SCANNING..."); return; }
  if ((isPb || isRc) && n == 0) {
    display_.drawText(0, 24, kFontCompressedLeft, bt_.status().linked ? "SYNCING..." : "NO PHONE");
    display_.drawText(0, 40, kFontCompressedLeft, "ALLOW ON PHONE");
    return;
  }
  const int visible = 8;
  int start = listIndex_ - visible / 2;
  if (start < 0) start = 0;
  if (start > n - visible) start = n - visible < 0 ? 0 : n - visible;
  for (int row = 0; row < visible && start + row < n; ++row) {
    int i = start + row;
    char line[24];
    if (screen_ == Screen::SwitchDevice) {
      const auto& d = devs[i];
      const char* nm = !d.name.empty() ? d.name.c_str()
                     : d.mac.size() >= 6 ? d.mac.c_str() + d.mac.size() - 6 : d.mac.c_str();
      bool active = d.connected || d.mac == bt_.status().activeDeviceMac;
      std::snprintf(line, sizeof(line), "%c%s", active ? '*' : ' ', nm);   // * = connected
    } else if (screen_ == Screen::SelectEcu) {
      const auto& m = ecu::kModules[i];
      std::snprintf(line, sizeof(line), "%c%s", m.addr == readEcu_ ? '*' : ' ', m.name);  // * = current
    } else {  // Phonebook / RecentCalls
      const Phonebook& pb = isRc ? callHistory_ : phonebook_;
      std::string nm = pb.entries()[i].name;
      if (i == listIndex_ && static_cast<int>(nm.size()) > 13) nm = marquee(nm, 13);  // scroll long names
      std::snprintf(line, sizeof(line), " %s", nm.c_str());
    }
    // Selected row gets the inverse highlight bar instead of a '>' marker.
    display_.drawText(0, static_cast<uint8_t>(16 + row * 8),
                      i == listIndex_ ? (kFontCompressedLeft | kFontHighlight) : kFontCompressedLeft, line);
  }
}

void App::renderCalibrate() {
  char l[24];
  display_.beginFullScreen(true);
  display_.drawText(0, 0, kFontCentered, "CALIBRATE");
  std::snprintf(l, sizeof(l), "PRESS %s", inputs_.calibPrompt());
  display_.drawText(0, 18, kFontCompressedLeft, l);
  std::snprintf(l, sizeof(l), "STEP %d/%d", inputs_.calibStep() + 1, inputs_.calibTotal());
  display_.drawText(0, 34, kFontCompressedLeft, l);
  display_.drawText(0, 60, kFontCompressedLeft, "HOLD ENC=CANCEL");
}

void App::render() {
  dirty_ = false;
  const BtStatus& st = bt_.status();

  if (inputs_.calibrating()) { renderCalibrate(); return; }

  if (ctx_ == Context::IncomingCall) {
    std::string name = st.callerName;
    if (name.empty() && !st.callerNumber.empty()) name = phonebook_.lookup(st.callerNumber);
    std::string who = !name.empty() ? name
                    : !st.callerNumber.empty() ? st.callerNumber : std::string("UNKNOWN");
    display_.beginFullScreen(true);
    display_.drawText(0, 12, kFontCentered, "INCOMING");
    display_.drawText(0, 40, kFontCentered, who.c_str());
    return;
  }
  if (ctx_ == Context::ActiveCall) {
    display_.beginFullScreen(true);
    display_.drawText(0, 24, kFontCentered, "IN CALL");
    return;
  }
  if (screen_ != Screen::None) { renderScreen(); return; }
  if (menuOpen_) { menu_.render(display_); return; }

  // Radio passthrough: unless the head unit is in "CD" mode (our aux source is
  // selected), forward its own top-line text straight to the cluster. Menu,
  // diagnostics and calls above always override this.
  if (radio_ && !radio_->cdMode()) {
    scrolling_ = false;
    display_.showTopLines(radio_->line1(), radio_->line2());
    return;
  }

  if (st.playing && !st.title.empty()) {
    scrolling_ = st.title.size() > kWin || st.artist.size() > kWin;
    display_.showTopLines(marquee(st.title).c_str(), marquee(st.artist).c_str());
  } else {
    std::string dev = st.linked ? (st.activeDeviceName.empty() ? "PHONE" : st.activeDeviceName)
                                : "NO PHONE";
    scrolling_ = dev.size() > kWin;   // scroll long phone names
    // Short lines (<8) are auto-centred by the FIS; keep PLAYING/PAUSED un-padded.
    display_.showTopLines(st.playing ? "PLAYING" : "PAUSED", marquee(dev).c_str());
  }
}

} // namespace mmi
