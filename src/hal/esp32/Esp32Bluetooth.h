// Esp32Bluetooth.h — IBluetooth over the BC127 (Melody 7.x) on UART1.
//
// Melody Link IDs are <device><profile> hex (Link ID Management, manual p.13):
//   device 1..5, profile 0=A2DP 1=AVRCP 2=AGHFP 3=HFP 4=BLE 5=SPP 6=PBAP.
// So media/call commands must target the ACTIVE device's link, not a hardcoded
// "11"/"13". We learn the active device and the paired list from the module:
//   STATUS -> STATE + one "LINK <id> CONNECTED <profile> <mac> ..." per link
//   LIST   -> one "LIST <mac> <profiles...>" per paired device
// STATUS is re-polled periodically so a phone that connects/disconnects (e.g.
// swapping the OnePlus 9 Pro for the 13) is reflected as the active device.
#pragma once
#include "../IBluetooth.h"
#include "../../Config.h"
#include "../../bt/Phonebook.h"
#include <Arduino.h>
#include <deque>
#include <map>
#include <vector>

namespace mmi {

class Esp32Bluetooth : public IBluetooth {
public:
  // Melody profile ids (second Link-ID digit).
  enum Prof { A2DP = 0, AVRCP = 1, HFP = 3, PBAP = 6 };

  void begin() override {
    Serial1.begin(cfg::BC127_BAUD, SERIAL_8N1, cfg::PIN_BC127_RX, cfg::PIN_BC127_TX);
    delay(800);                 // let the BC127 finish its own boot
    refreshDevices();           // STATUS + LIST: learn active device + paired list
  }

  void poll() override {
    while (Serial1.available()) {
      char c = Serial1.read();
      ++rxBytes_;
      if (c == '\r' || c == '\n') { if (!line_.empty()) { logLine("< " + line_); parse(line_); line_.clear(); } }
      else if (line_.size() < 256) line_.push_back(c);
    }
    // Re-poll STATUS so the active device tracks connect/disconnect/switch.
    uint32_t now = millis();
    if (now - lastStatusPoll_ > kStatusPollMs) { lastStatusPoll_ = now; sendCommand("STATUS"); }
    // The queued PB_PULL fires on OPEN_OK / LINK-up for the PBAP profile (see
    // parse()); firing it early hits "wrong parameter" because the link isn't up
    // yet. Just give up quietly if the link never comes / the download stalls.
    if (pbapPullQueued_ && now - pbapStart_ > 8000)  { pbapPullQueued_ = false; pbapPulling_ = false; }
    if (pbapPulling_    && now - pbapStart_ > 15000) pbapPulling_ = false;

    // Debounced phonebook auto-pull: only download once the active phone has been
    // stably active for a few seconds, so the book doesn't re-sync every time the
    // active device flip-flops between two connected phones.
    std::string am = activeMac();
    if (!am.empty() && !macEq(am, pbapSourceMac_)) {
      if (!macEq(am, pendingPullMac_)) { pendingPullMac_ = am; pendingPullSince_ = now; }
      else if (now - pendingPullSince_ > 3000 && !pbapPulling_ && !pbapPullQueued_) {
        startPull(activeDev_, am, 0); pendingPullMac_.clear();
      }
    } else {
      pendingPullMac_.clear();
    }
  }
  uint32_t rxBytes() const { return rxBytes_; }

  const BtStatus& status() const override { return st_; }
  void sendCommand(const std::string& line) override {
    logLine("> " + line);
    Serial1.print(line.c_str()); Serial1.print('\r');
  }

  std::string recentLog() const {
    std::string o;
    for (const auto& l : log_) { o += l; o += '\n'; }
    return o;
  }
  std::string debugLog() const override { return recentLog(); }

  // ---- media / calls: target the ACTIVE device's link ----
  void playPause() override { sendCommand("MUSIC " + link(AVRCP) + (st_.playing ? " PAUSE" : " PLAY")); }
  void trackNext() override { sendCommand("MUSIC " + link(AVRCP) + " FORWARD"); }
  void trackPrev() override { sendCommand("MUSIC " + link(AVRCP) + " BACKWARD"); }
  void volumeUp()   override { sendCommand("VOLUME " + link(A2DP) + " UP"); }
  void volumeDown() override { sendCommand("VOLUME " + link(A2DP) + " DOWN"); }

  void callAnswer() override { sendCommand("CALL " + link(HFP) + " ANSWER"); }
  void callReject() override { sendCommand("CALL " + link(HFP) + " REJECT"); }
  void callEnd()    override { sendCommand("CALL " + link(HFP) + " END"); }
  void dial(const std::string& number) override { sendCommand("CALL " + link(HFP) + " " + number); }

  void connectDevice(const std::string& mac) override {
    if (singleDevice_) disconnectActive();     // enforce a single active link
    // If it's already connected (both phones can be), just make it the active
    // control target; otherwise open it. Either way STATUS reconciles link ids.
    int dev = 0;
    for (auto& kv : mediaDev_) if (macEq(kv.second, mac)) dev = kv.first;
    if (dev) { setActiveDev(dev); }
    else { sendCommand("OPEN " + mac + " A2DP"); sendCommand("OPEN " + mac + " HFP"); st_.activeDeviceMac = mac; st_.activeDeviceName.clear(); sendCommand("NAME " + mac); }
    sendCommand("STATUS");
  }
  void disconnectActive() override { if (activeDev_) sendCommand("CLOSE " + link(A2DP)); }

  // ---- PBAP phonebook (Melody PB_PULL) ----
  // The module allows a single PBAP link at a time, so the phonebook always
  // belongs to the ACTIVE device; switching phones re-pulls the new one.
  std::vector<Contact> contacts() const override { return book_.entries(); }
  size_t contactCount() const override { return book_.size(); }
  std::string contactsSource() const override {         // which phone the book is from
    for (const auto& d : paired_) if (macEq(d.mac, pbapSourceMac_) && !d.name.empty()) return d.name;
    return pbapSourceMac_.empty() ? "" : st_.activeDeviceName;
  }
  void pullPhonebook() override { startPull(activeDev_, activeMac(), 0); }
  // Recent calls = combined call history (PB_PULL phonebook 5).
  std::vector<Contact> callHistory() const override { return calls_.entries(); }
  size_t callHistoryCount() const override { return calls_.size(); }
  void pullCallHistory() override { startPull(activeDev_, activeMac(), 1); }

  std::vector<BtDevice> pairedDevices() const override { return paired_; }
  void refreshDevices() override { sendCommand("STATUS"); sendCommand("LIST"); }

  void setSingleDevice(bool enabled) override { singleDevice_ = enabled; if (enabled) enforceSingle(); }

  void micLoopback(bool on) override {
    sendCommand(on ? "LOOPBACK " + link(HFP) + " ON" : "LOOPBACK " + link(HFP) + " OFF");
    st_.scoOpen = on; changed();
  }
  void setMicGain(uint8_t gain) override { sendCommand("SET HFP_GAIN=" + std::to_string(gain)); }

  void onStatusChanged(std::function<void()> cb) override { cb_ = std::move(cb); }

private:
  void logLine(const std::string& l) {
    log_.push_back(l); if (log_.size() > 30) log_.pop_front();
    if (cfg::DEBUG_BC127_SERIAL) { Serial.print("[BC127] "); Serial.println(l.c_str()); }
  }
  void changed() { if (cb_) cb_(); }
  static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
  static char hexc(int v) { return v < 10 ? char('0' + v) : char('A' + v - 10); }
  static int hexv(char c) { c = toupper(c); return (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0; }

  // Build a link id for a profile on the active device (default device 1).
  std::string link(int profile) const {
    int dev = activeDev_ > 0 ? activeDev_ : 1;
    std::string s; s.push_back(hexc(dev)); s.push_back(hexc(profile)); return s;
  }
  // MAC of the current active device (for OPEN/PB_PULL).
  std::string activeMac() const {
    auto it = mediaDev_.find(activeDev_);
    if (it != mediaDev_.end() && !it->second.empty()) return it->second;
    return st_.activeDeviceMac;
  }
  std::string link6(int dev) const { std::string s; s.push_back(hexc(dev)); s.push_back(hexc(PBAP)); return s; }

  // repository 1=local; phonebook 1=contacts, 5=combined call history.
  void sendPbPull(int dev) { sendCommand("PB_PULL " + link6(dev) + (pbapTarget_ ? " 1 5" : " 1 1")); }

  // Start a PBAP download from device <dev> (mac) into contacts (target 0) or
  // call history (target 1). If that phone's PBAP link is already up, PB_PULL
  // immediately; otherwise free any other PBAP link (the module only allows one)
  // and OPEN PBAP — the pull fires when the link is up.
  void startPull(int dev, const std::string& mac, int target) {
    if (dev <= 0 || mac.empty()) return;
    pbapTarget_ = target;
    Phonebook& bk = target ? calls_ : book_;
    bk.clear(); bk.beginPull();
    pbapSourceMac_ = mac; pbapPullDev_ = dev;
    pbapPulling_ = true; pbapStart_ = millis();
    if (pbapDev_ == dev) { pbapPullQueued_ = false; sendPbPull(dev); }
    else {
      if (pbapDev_ && pbapDev_ != dev) sendCommand("CLOSE " + link6(pbapDev_)); // free the single PBAP slot
      pbapPullQueued_ = true;
      sendCommand("OPEN " + mac + " PBAP");
    }
    changed();
  }

  // Fire the queued PB_PULL for the RIGHT phone once its PBAP link is up.
  void firePbapPull(int dev) {
    if (!pbapPullQueued_) return;
    if (pbapPullDev_ && dev > 0 && dev != pbapPullDev_) return;  // not this phone's link yet
    pbapPullQueued_ = false;
    int d = dev > 0 ? dev : (pbapPullDev_ > 0 ? pbapPullDev_ : activeDev_);
    sendPbPull(d);
  }

  static std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> t; size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && s[i] == ' ') ++i;
      size_t j = i; while (j < s.size() && s[j] != ' ') ++j;
      if (j > i) t.push_back(s.substr(i, j - i));
      i = j;
    }
    return t;
  }
  static bool macEq(const std::string& a, const std::string& b) {
    // BC127 sometimes normalises leading zeros; match on the shared suffix.
    if (a.empty() || b.empty()) return false;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    return a.compare(a.size() - n, n, b, b.size() - n, n) == 0;
  }

  BtDevice* findDev(const std::string& mac) {
    for (auto& d : paired_) if (macEq(d.mac, mac)) return &d;
    return nullptr;
  }

  // Choose the active media device. With BOTH phones connected we want the one
  // that is actually the audio source: prefer the STREAMING/PLAYING device, then
  // the currently-active device if it is still connected, then the lowest number.
  void chooseActive() {
    int best = 0, keep = 0;
    for (auto& kv : mediaDev_) {                      // dev -> mac from this scan
      if (best == 0 || kv.first < best) best = kv.first;
      if (!st_.activeDeviceMac.empty() && macEq(kv.second, st_.activeDeviceMac)) keep = kv.first;
    }
    int dev = streamDev_ ? streamDev_ : (keep ? keep : best);
    if (dev == 0) { activeDev_ = 0; if (st_.linked || !st_.activeDeviceMac.empty()) { st_.linked = false; st_.playing = false; st_.activeDeviceMac.clear(); st_.activeDeviceName.clear(); changed(); } return; }
    setActiveDev(dev);
    bool play = avrcpPlaying_.count(dev) ? avrcpPlaying_[dev] : false;
    if (play != st_.playing) { st_.playing = play; changed(); }
  }

  // Make device <dev> active: update mac + resolve its name (via NAME if unknown).
  void setActiveDev(int dev) {
    if (dev <= 0) return;
    activeDev_ = dev; st_.linked = true;
    auto it = mediaDev_.find(dev);
    std::string mac = it != mediaDev_.end() ? it->second : "";
    if (mac.empty()) { sendCommand("STATUS"); return; }      // learn its mac, then reconcile
    if (!macEq(mac, st_.activeDeviceMac) || st_.activeDeviceName.empty()) {
      st_.activeDeviceMac = mac;
      st_.title.clear(); st_.artist.clear();                // metadata belongs to the new device
      BtDevice* d = findDev(mac);
      if (d && !d->name.empty()) st_.activeDeviceName = d->name;
      else { st_.activeDeviceName.clear(); sendCommand("NAME " + mac); }
      // Phonebook auto-pull is handled by a debounce in poll() (see below), so
      // the book doesn't thrash while the active device oscillates between phones.
      changed();
    }
  }

  void enforceSingle() {
    // Close every A2DP link that isn't the active device's.
    if (!activeDev_) return;
    for (auto& kv : mediaDev_)
      if (kv.first != activeDev_) sendCommand("CLOSE " + (std::string() + hexc(kv.first) + hexc(A2DP)));
  }

  static int parseId(const std::string& s) { return s.empty() ? 0 : ((hexv(s[0]) << 4) | (s.size() > 1 ? hexv(s[1]) : 0)); }

  // Register a "LINK <id> CONNECTED <profile> <mac> <state...>" line.
  void noteLink(const std::vector<std::string>& t) {
    if (t.size() < 5) return;
    int id = parseId(t[1]);
    int dev = id >> 4, prof = id & 0xF;
    const std::string& mac = t[4];
    const std::string st = t.size() > 5 ? t[5] : "";
    if (prof == A2DP || prof == AVRCP) mediaDev_[dev] = mac;
    if (prof == A2DP && st == "STREAMING") streamDev_ = dev;   // the audio source
    if (prof == AVRCP) { avrcpPlaying_[dev] = (st == "PLAYING"); if (st == "PLAYING") streamDev_ = dev; }
    if (prof == PBAP) { pbapDev_ = dev; firePbapPull(dev); }   // PBAP link up on this phone

    if (BtDevice* d = findDev(mac)) d->connected = true;
    else paired_.push_back({mac, "", true});
  }

  void parse(const std::string& l) {
    auto t = tokens(l);
    bool ch = false;

    // ---- PBAP phonebook download in progress: feed vCard lines to the parser ----
    // Melody streams "PB_PULL <link> <size> BEGIN:VCARD", the vCard body, then a
    // final "OK". vCard lines match no event keyword, so we still let them fall
    // through harmlessly; we only special-case the terminating OK.
    if (pbapPulling_) {
      (pbapTarget_ ? calls_ : book_).feedLine(l, kMaxContacts);
      if (l == "OK") { pbapPulling_ = false; changed(); return; }
      if ((!t.empty() && t[0] == "PB_PULL") || l.find("VCARD") != std::string::npos) { changed(); return; }
    }

    // ---- STATUS scan: STATE ... LINK ... LINK ... OK ----
    if (l.rfind("STATE ", 0) == 0) { scanning_ = true; mediaDev_.clear(); avrcpPlaying_.clear(); streamDev_ = 0; pbapDev_ = 0; for (auto& d : paired_) d.connected = false; return; }
    if (scanning_ && t.size() >= 3 && t[0] == "LINK" && t[2] == "CONNECTED") { noteLink(t); return; }
    if (scanning_ && (l == "OK" || l.rfind("OK", 0) == 0)) { scanning_ = false; chooseActive(); return; }

    // ---- async link events ----
    if (t.size() >= 5 && t[0] == "LINK" && t[2] == "CONNECTED") { noteLink(t); chooseActive(); }
    if (t.size() >= 4 && t[0] == "OPEN_OK") {                 // OPEN_OK <id> <profile> <mac>
      int id = (hexv(t[1][0]) << 4) | (t[1].size() > 1 ? hexv(t[1][1]) : 0);
      int prof = id & 0xF; if (prof == A2DP || prof == AVRCP) { mediaDev_[id >> 4] = t.size() >= 4 ? t[3] : ""; chooseActive(); }
      if (prof == PBAP) { pbapDev_ = id >> 4; firePbapPull(id >> 4); }  // PBAP link up -> download
      st_.linked = true; ch = true;
    }
    if (has(l, "CLOSE_OK")) {
      if (t.size() >= 2) { int id = parseId(t[1]); int dev = id >> 4; if ((id & 0xF) == PBAP) { if (pbapDev_ == dev) pbapDev_ = 0; } else { mediaDev_.erase(dev); avrcpPlaying_.erase(dev); if (streamDev_ == dev) streamDev_ = 0; } }
      chooseActive(); ch = true;
    }

    // ---- LIST <mac> <profiles...>  (paired device list) ----
    if (t.size() >= 2 && t[0] == "LIST") {
      const std::string& mac = t[1];
      BtDevice* d = findDev(mac);
      if (!d) { paired_.push_back({mac, "", false}); d = &paired_.back(); }
      if (d->name.empty()) sendCommand("NAME " + mac);   // resolve even if STATUS added it first
      ch = true;
    }

    // ---- NAME <mac> "<name>" ----
    if (t.size() >= 3 && t[0] == "NAME") {
      std::string name = l.substr(l.find(t[2]));
      if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
      if (BtDevice* d = findDev(t[1])) d->name = name;
      if (macEq(t[1], st_.activeDeviceMac)) { st_.activeDeviceName = name; }
      ch = true;
    }

    // ---- metadata (AVRCP_MEDIA <id> TITLE:/ARTIST: ...) — only for the active device ----
    if (t.size() >= 3 && t[0] == "AVRCP_MEDIA") {
      int dev = parseId(t[1]) >> 4;
      if (activeDev_ == 0 || dev == activeDev_) {
        if (t[2] == "TITLE:")  { st_.title  = afterKey(l, "TITLE:");  ch = true; }
        if (t[2] == "ARTIST:") { st_.artist = afterKey(l, "ARTIST:"); ch = true; }
      }
    }
    // ---- playback: the playing/streaming device becomes active ----
    if (t[0] == "AVRCP_PLAY" || t[0] == "A2DP_STREAM_START") {
      if (t.size() >= 2) setActiveDev(parseId(t[1]) >> 4);
      st_.playing = true; ch = true;
    }
    if (t[0] == "AVRCP_PAUSE" || t[0] == "AVRCP_STOP" || t[0] == "A2DP_STREAM_SUSPEND") {
      int dev = t.size() >= 2 ? (parseId(t[1]) >> 4) : 0;
      if (dev == 0 || dev == activeDev_) { st_.playing = false; ch = true; }
    }
    if (has(l, "CALL_INCOMING")) { st_.call = CallState::Incoming; st_.callerNumber = afterKey(l, "CALL_INCOMING"); ch = true; }
    if (has(l, "CALL_OUTGOING") || has(l, "CALL_DIAL")) { st_.call = CallState::Outgoing; ch = true; }
    if (has(l, "SCO_OPEN")) { st_.scoOpen = true; st_.call = CallState::Active; ch = true; }
    if (has(l, "SCO_CLOSE")) { st_.scoOpen = false; ch = true; }
    if (has(l, "CALL_END") || has(l, "CALL_IDLE")) { st_.call = CallState::Idle; st_.scoOpen = false; ch = true; }
    if (has(l, "CALLER_NUMBER")) { st_.callerNumber = afterKey(l, "CALLER_NUMBER"); ch = true; }
    if (ch) changed();
  }

  static std::string afterKey(const std::string& s, const char* key) {
    size_t p = s.find(key); if (p == std::string::npos) return "";
    p += std::string(key).size();
    size_t e = s.find_first_of("\r\n", p);
    std::string v = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
  }

  static constexpr uint32_t kStatusPollMs = 4000;

  BtStatus st_;
  std::string line_;
  std::deque<std::string> log_;
  std::function<void()> cb_;
  volatile uint32_t rxBytes_ = 0;
  std::vector<BtDevice> paired_;         // from LIST/STATUS
  std::map<int, std::string> mediaDev_;  // device number -> mac (A2DP/AVRCP), current scan
  std::map<int, bool> avrcpPlaying_;     // device number -> AVRCP playing, current scan
  int  streamDev_ = 0;                   // device currently streaming/playing, current scan
  int  activeDev_ = 0;                   // active media device number (1..5), 0=none
  bool scanning_ = false;                // inside a STATUS response
  bool singleDevice_ = false;
  uint32_t lastStatusPoll_ = 0;
  // PBAP phonebook download state
  Phonebook book_;                       // accumulated contacts (parsed vCards)
  Phonebook calls_;                      // accumulated call history (phonebook 5)
  int  pbapTarget_ = 0;                  // current pull target: 0=contacts, 1=calls
  bool pbapPulling_ = false;             // a PB_PULL is streaming
  bool pbapPullQueued_ = false;          // waiting for the PBAP link before PB_PULL
  uint32_t pbapStart_ = 0;
  int  pbapDev_ = 0;                     // device that currently holds the (single) PBAP link
  int  pbapPullDev_ = 0;                 // device we're pulling from
  std::string pbapSourceMac_;            // mac the current book_ belongs to
  std::string pendingPullMac_;           // candidate active phone awaiting the debounce
  uint32_t pendingPullSince_ = 0;
  static constexpr size_t kMaxContacts = 500;
};

} // namespace mmi
