// Esp32Bluetooth.h — IBluetooth over the BC127 (Melody) on UART1.
// Parses the module's async event lines into BtStatus and issues its AT-style
// commands. Command spellings marked TODO need a final check against the Melody
// manual on-car; the event parsing follows the v1 sketch which worked.
#pragma once
#include "../IBluetooth.h"
#include "../../Config.h"
#include <Arduino.h>
#include <deque>
#include <map>
#include <vector>

namespace mmi {

class Esp32Bluetooth : public IBluetooth {
public:
  void begin() override {
    Serial1.begin(cfg::BC127_BAUD, SERIAL_8N1, cfg::PIN_BC127_RX, cfg::PIN_BC127_TX);
    delay(800);                 // let the BC127 finish its own boot
    sendCommand("STATUS");      // probe: a live module answers with STATE.../OK
  }

  void poll() override {
    while (Serial1.available()) {
      char c = Serial1.read();
      ++rxBytes_;
      if (c == '\r' || c == '\n') { if (!line_.empty()) { logLine("< " + line_); parse(line_); line_.clear(); } }
      else if (line_.size() < 256) line_.push_back(c);
    }
  }
  uint32_t rxBytes() const { return rxBytes_; }   // total raw bytes seen from the module

  const BtStatus& status() const override { return st_; }
  void sendCommand(const std::string& line) override {
    logLine("> " + line);
    Serial1.print(line.c_str()); Serial1.print('\r');
  }

  // Recent BC127 traffic (newest last), for the debug console + cluster view.
  std::string recentLog() const {
    std::string o;
    for (const auto& l : log_) { o += l; o += '\n'; }
    return o;
  }
  std::string debugLog() const override { return recentLog(); }

  void playPause() override { sendCommand(st_.playing ? "MUSIC 11 PAUSE" : "MUSIC 11 PLAY"); }
  void trackNext() override { sendCommand("MUSIC 11 FORWARD"); }
  void trackPrev() override { sendCommand("MUSIC 11 BACKWARD"); }
  void volumeUp()   override { sendCommand("VOLUME 11 UP"); }
  void volumeDown() override { sendCommand("VOLUME 11 DOWN"); }

  void callAnswer() override { sendCommand("CALL 11 ANSWER"); }
  void callReject() override { sendCommand("CALL 11 REJECT"); }
  void callEnd()    override { sendCommand("CALL 11 END"); }
  void dial(const std::string& number) override { sendCommand("CALL 11 " + number); }

  void connectDevice(const std::string& mac) override {
    disconnectActive();                       // enforce a single active link
    sendCommand("OPEN " + mac + " A2DP");
    sendCommand("OPEN " + mac + " HFP");
    st_.activeDeviceMac = mac;
  }
  void disconnectActive() override { sendCommand("CLOSE 11"); }

  void setSingleDevice(bool enabled) override { singleDevice_ = enabled; if (enabled) enforceSingle(); }

  void micLoopback(bool on) override {
    // TODO: confirm BC127 loopback/sidetone command in Melody manual.
    sendCommand(on ? "LOOPBACK 11 ON" : "LOOPBACK 11 OFF");
    st_.scoOpen = on; changed();
  }
  void setMicGain(uint8_t gain) override {
    sendCommand("SET HFP_GAIN=" + std::to_string(gain)); // TODO: verify key
  }

  void onStatusChanged(std::function<void()> cb) override { cb_ = std::move(cb); }

private:
  void logLine(const std::string& l) {
    log_.push_back(l); if (log_.size() > 30) log_.pop_front();
    if (cfg::DEBUG_BC127_SERIAL) { Serial.print("[BC127] "); Serial.println(l.c_str()); }
  }
  void changed() { if (cb_) cb_(); }
  static bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
  static std::string after(const std::string& s, const char* key) {
    size_t p = s.find(key); if (p == std::string::npos) return "";
    p += std::string(key).size();
    size_t e = s.find_first_of("\r\n", p);
    std::string v = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    return v;
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

  // Track which BC127 link ids belong to which phone (MAC), and enforce a single
  // active device by CLOSE-ing the links of all but the most-recently-linked one.
  void trackLink(const std::string& l) {
    auto t = tokens(l);
    // LINK <id> CONNECTED <profile> <mac> <state...>
    if (t.size() >= 5 && t[0] == "LINK" && t[2] == "CONNECTED") {
      int id = atoi(t[1].c_str());
      linkMac_[id] = t[4];
      lastMac_ = t[4];
      if (singleDevice_) enforceSingle();
    }
  }
  void enforceSingle() {
    if (lastMac_.empty()) return;
    std::vector<int> toClose;
    for (auto& kv : linkMac_) if (kv.second != lastMac_) toClose.push_back(kv.first);
    for (int id : toClose) { sendCommand("CLOSE " + std::to_string(id)); linkMac_.erase(id); }
  }

  void parse(const std::string& l) {
    bool ch = false;
    if (has(l, "LINK") && has(l, "CONNECTED")) trackLink(l);
    if (has(l, "OPEN_OK") || has(l, "LINK")) { st_.linked = true; ch = true; }
    if (has(l, "CLOSE_OK")) {
      st_.linked = false; st_.playing = false; ch = true;
      auto t = tokens(l); if (t.size() >= 2) linkMac_.erase(atoi(t[1].c_str()));
    }
    if (has(l, "TITLE:"))  { st_.title  = after(l, "TITLE:");  ch = true; }
    if (has(l, "ARTIST:")) { st_.artist = after(l, "ARTIST:"); ch = true; }
    if (has(l, "AVRCP_PLAY") || has(l, "A2DP_STREAM_START")) { st_.playing = true; ch = true; }
    if (has(l, "AVRCP_PAUSE") || has(l, "AVRCP_STOP") || has(l, "A2DP_STREAM_SUSPEND")) { st_.playing = false; ch = true; }
    if (has(l, "CALL_INCOMING")) { st_.call = CallState::Incoming; st_.callerNumber = after(l, "CALL_INCOMING"); ch = true; }
    if (has(l, "CALL_OUTGOING") || has(l, "CALL_DIAL")) { st_.call = CallState::Outgoing; ch = true; }
    if (has(l, "SCO_OPEN")) { st_.scoOpen = true; st_.call = CallState::Active; ch = true; }
    if (has(l, "SCO_CLOSE")) { st_.scoOpen = false; ch = true; }
    if (has(l, "CALL_END") || has(l, "CALL_IDLE")) { st_.call = CallState::Idle; st_.scoOpen = false; ch = true; }
    if (has(l, "CALLER_NUMBER")) { st_.callerNumber = after(l, "CALLER_NUMBER"); ch = true; }
    if (ch) changed();
  }

  BtStatus st_;
  std::string line_;
  std::deque<std::string> log_;
  std::function<void()> cb_;
  volatile uint32_t rxBytes_ = 0;
  std::map<int, std::string> linkMac_;   // connected link id -> device MAC
  std::string lastMac_;                  // most-recently-linked device (kept)
  bool singleDevice_ = false;            // enforcement off by default
};

} // namespace mmi
