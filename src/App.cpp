// App.cpp — core UI loop, hardware-independent.
#include "App.h"
#include "ui/InputRouter.h"
#include "ui/MenuTree.h"
#include "ui/GraphRenderer.h"
#include "diag/DtcDescriptions.h"
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

bool App::isDiagScreen() const {
  return screen_ == Screen::DiagFavourites || screen_ == Screen::DiagReadGroup ||
         screen_ == Screen::DiagGraph || screen_ == Screen::DiagFaults;
}

void App::tick(uint32_t nowMs) {
  now_ = nowMs;
  bt_.poll();

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
    uint32_t interval = (screen_ == Screen::DiagFaults) ? 400u : 150u;
    if (now_ - lastSample_ > interval) {
      lastSample_ = now_;
      if (screen_ == Screen::DiagFaults) { if (diag_.readFaults(readEcu_, faults_)) faultsLoaded_ = true; }
      else sampleDiag();
      dirty_ = true;
    }
  }
  if (screen_ == Screen::ButtonMonitor || screen_ == Screen::Bc127Debug ||
      screen_ == Screen::DiagFaults) dirty_ = true; // live (fault desc marquee)

  if (scrolling_) dirty_ = true;
  if (dirty_) render();
}

std::string App::marquee(const std::string& s, int width) const {
  if (static_cast<int>(s.size()) <= width) return s;
  std::string padded = s + "    ";
  int span = static_cast<int>(padded.size());
  int step = static_cast<int>((now_ / 350) % static_cast<uint32_t>(span));
  std::string out;
  for (int i = 0; i < width; ++i) out += padded[(step + i) % span];
  return out;
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
    case Action::ScrollDown: if (menuOpen_) { menu_.scrollDown(); dirty_ = true; } break;
    case Action::ScrollUp:   if (menuOpen_) { menu_.scrollUp();   dirty_ = true; } break;
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
    case Action::VolumeUp:   bt_.volumeUp();   break;
    case Action::VolumeDown: bt_.volumeDown(); break;
    case Action::TrackNext:  bt_.trackNext();  break;
    case Action::TrackPrev:  bt_.trackPrev();  break;
    case Action::PlayPause:  bt_.playPause();  break;
    case Action::CallAnswer: bt_.callAnswer(); dirty_ = true; break;
    case Action::CallReject: bt_.callReject(); dirty_ = true; break;
    case Action::CallEnd:    bt_.callEnd();    dirty_ = true; break;
    default: break;
  }
}

int App::screenItemCount() const {
  switch (screen_) {
    case Screen::SwitchDevice:   return static_cast<int>(btMgr_.paired().size());
    case Screen::Phonebook:      return static_cast<int>(phonebook_.size());
    case Screen::DiagFavourites: return presets_.size();
    case Screen::DiagFaults:     return static_cast<int>(faults_.size());
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
      case Action::ScrollDown: readGroup_++; graph_.clear(); dirty_ = true; return true;
      case Action::ScrollUp:   if (readGroup_ > 1) readGroup_--; graph_.clear(); dirty_ = true; return true;
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
    const auto& p = btMgr_.paired();
    if (listIndex_ < static_cast<int>(p.size())) btMgr_.switchTo(p[listIndex_].mac);
    screen_ = Screen::None;
  } else if (screen_ == Screen::Phonebook) {
    const auto& e = phonebook_.entries();
    if (listIndex_ < static_cast<int>(e.size())) bt_.dial(e[listIndex_].number);
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
  screen_ = s; listIndex_ = 0; graph_.clear(); lastSample_ = 0;
  if (s == Screen::DiagFaults) { faultsLoaded_ = false; faults_.clear(); diag_.readFaults(readEcu_, faults_); }
  dirty_ = true;
}

void App::onMenuSelect(MenuId id) {
  switch (id) {
    case MenuId::BtSwitchDevice: openScreen(Screen::SwitchDevice);   break;
    case MenuId::BtPhonebook:    openScreen(Screen::Phonebook);      break;
    case MenuId::DbgMicTest:     openScreen(Screen::MicTest);        break;
    case MenuId::DbgButtonMonitor: openScreen(Screen::ButtonMonitor); break;
    case MenuId::DbgBc127:       openScreen(Screen::Bc127Debug);     break;
    case MenuId::DbgCalibrate:   inputs_.startCalibration();         break;
    case MenuId::SetWifi:        openScreen(Screen::WifiInfo);       break;
    case MenuId::SetUpdate:      openScreen(Screen::UpdateInfo);     break;
    case MenuId::BtSingleDevice: openScreen(Screen::OneDevice);      break;
    case MenuId::DiagFavourites: openScreen(Screen::DiagFavourites); break;
    case MenuId::DiagReadGroup:  readGroup_ = 2; openScreen(Screen::DiagReadGroup); break;
    case MenuId::DiagGraph:      readGroup_ = 2; openScreen(Screen::DiagGraph);     break;
    case MenuId::DiagReadFaults: openScreen(Screen::DiagFaults);     break;
    case MenuId::Exit:           menuOpen_ = false;                  break;
    default: break;
  }
  dirty_ = true;
}

// ---- diagnostics sampling + rendering ----

void App::sampleDiag() {
  uint8_t e = readEcu_, g = readGroup_;
  int vi = 0;
  if (screen_ == Screen::DiagFavourites && presets_.size() > 0) {
    const Preset& p = presets_.at(diagPresetIdx_);
    e = p.ecu; g = p.group; vi = p.valueIndex;
  }
  if (!diag_.readGroup(e, g, group_)) return;
  if (vi < group_.count) {
    graph_.push_back(group_.values[vi].value);
    if (static_cast<int>(graph_.size()) > kGraphW) graph_.erase(graph_.begin());
  }
}

void App::renderDiag() {
  char l[24];

  if (screen_ == Screen::DiagFaults) {
    display_.beginFullScreen(true);
    display_.drawText(0, 0, kFontCentered, "FAULTS");
    if (!faultsLoaded_) { display_.drawText(0, 20, kFontCompressedLeft, "READING..."); return; }
    if (faults_.empty()) { display_.drawText(0, 20, kFontCompressedLeft, "NO FAULTS"); return; }

    int n = static_cast<int>(faults_.size());
    const int visible = 5;                       // fault rows + CLEAR ALL, windowed
    int total = n + 1;
    int start = listIndex_ - visible / 2;
    if (start < 0) start = 0;
    if (start > total - visible) start = total - visible < 0 ? 0 : total - visible;
    for (int r = 0; r < visible && start + r < total; ++r) {
      int i = start + r;
      if (i < n) std::snprintf(l, sizeof(l), "%c%05u%s", i == listIndex_ ? '>' : ' ', faults_[i].code, faults_[i].sporadic ? " *" : "");
      else       std::snprintf(l, sizeof(l), "%cCLEAR ALL", i == listIndex_ ? '>' : ' ');
      display_.drawText(0, static_cast<uint8_t>(14 + r * 9),
                        kFontCompressedLeft, l);
    }
    // Description of the highlighted fault scrolls along the bottom line.
    if (listIndex_ < n) {
      const char* desc = dtcDescription(faults_[listIndex_].code);
      display_.drawText(0, 68, kFontCompressedLeft, marquee(desc ? desc : "NO DESCRIPTION", 12).c_str());
    }
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

  // MultiValue: up to 4 label:value rows.
  display_.beginFullScreen(true);
  std::snprintf(l, sizeof(l), "%s %u", header, static_cast<unsigned>(readGroup_));
  display_.drawText(0, 0, kFontCentered, l);
  for (int i = 0; i < group_.count && i < 4; ++i) {
    std::snprintf(l, sizeof(l), "%s %s", group_.values[i].label.c_str(), fmt(group_.values[i]).c_str());
    display_.drawText(0, static_cast<uint8_t>(20 + i * 10), kFontCompressedLeft, l);
  }
}

void App::renderScreen() {
  if (isDiagScreen()) { renderDiag(); return; }

  display_.beginFullScreen(true);
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
  const char* title = screen_ == Screen::SwitchDevice ? "SWITCH DEV" : "PHONEBOOK";
  display_.drawText(0, 0, kFontCentered, title);
  int n = screenItemCount();
  const int visible = 8;
  int start = listIndex_ - visible / 2;
  if (start < 0) start = 0;
  if (start > n - visible) start = n - visible < 0 ? 0 : n - visible;
  for (int row = 0; row < visible && start + row < n; ++row) {
    int i = start + row;
    char line[24];
    if (screen_ == Screen::SwitchDevice) {
      const auto& d = btMgr_.paired()[i];
      bool active = d.mac == bt_.status().activeDeviceMac;
      std::snprintf(line, sizeof(line), "%c%s", active ? '*' : (i == listIndex_ ? '>' : ' '), d.name.c_str());
    } else {
      std::snprintf(line, sizeof(line), "%c%s", i == listIndex_ ? '>' : ' ', phonebook_.entries()[i].name.c_str());
    }
    display_.drawText(0, static_cast<uint8_t>(16 + row * 8),
                      kFontCompressedLeft, line);
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
