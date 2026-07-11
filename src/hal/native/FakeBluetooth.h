// FakeBluetooth.h — scriptable fake BC127 for the emulator (plan §7).
// Lets the single-active-device, call, phonebook and mic-test logic be
// developed entirely on the Mac. Scenario methods mimic BC127 async events.
#pragma once
#include "../IBluetooth.h"
#include "../../bt/Phonebook.h"
#include <vector>
#include <deque>

namespace mmi {

class FakeBluetooth : public IBluetooth {
public:
  void begin() override { status_.linked = false; }
  void poll() override {}
  const BtStatus& status() const override { return status_; }
  void sendCommand(const std::string& c) override { logLine("> " + c); }
  std::string debugLog() const override {
    std::string o; for (auto& l : log_) { o += l; o += '\n'; } return o;
  }

  void playPause() override { status_.playing = !status_.playing; changed(); }
  void trackNext() override { track_ = (track_ + 1) % 3; applyTrack(); }
  void trackPrev() override { track_ = (track_ + 2) % 3; applyTrack(); }
  void volumeUp() override {}
  void volumeDown() override {}

  void callAnswer() override { if (status_.call == CallState::Incoming) { status_.call = CallState::Active; status_.scoOpen = true; changed(); } }
  void callReject() override { status_.call = CallState::Idle; status_.scoOpen = false; changed(); }
  void callEnd() override    { status_.call = CallState::Idle; status_.scoOpen = false; changed(); }
  void dial(const std::string& number) override { status_.call = CallState::Outgoing; status_.callerNumber = number; changed(); }

  void connectDevice(const std::string& mac) override {
    // Enforce single active link: any prior link is dropped.
    status_.linked = true; status_.activeDeviceMac = mac;
    status_.activeDeviceName = nameFor(mac);
    logLine("< OPEN_OK " + mac);
    changed();
  }
  void disconnectActive() override { status_.linked = false; status_.activeDeviceName.clear(); status_.activeDeviceMac.clear(); changed(); }

  void micLoopback(bool on) override { status_.scoOpen = on; changed(); }
  void setMicGain(uint8_t g) override { micGain_ = g; }

  void onStatusChanged(std::function<void()> cb) override { cb_ = std::move(cb); }

  // ---- scenario scripting (called by the emulator UI / tests) ----
  void scriptConnect(const std::string& mac, const std::string& name) {
    pairbook_.push_back({name, mac});
    connectDevice(mac);
  }
  void scriptSecondConnects(const std::string& mac, const std::string& name) {
    // A second device tries to connect while one is active: the manager must
    // decide; the fake just reports the attempt by switching the link.
    pairbook_.push_back({name, mac});
    connectDevice(mac); // single-link => replaces
  }
  void scriptIncomingCall(const std::string& number) {
    status_.call = CallState::Incoming;
    status_.callerNumber = number;
    status_.callerName = book_.lookup(number); // resolve name via phonebook (§3c)
    changed();
  }
  void setPhonebook(std::vector<Contact> pb) {
    phonebook_ = std::move(pb);
    book_.clear();
    for (auto& c : phonebook_) book_.add(c.name, c.number, 1000);
  }
  // Simulate an AVRCP metadata update (title/artist) + start playback.
  void scriptMetadata(const std::string& title, const std::string& artist) {
    status_.title = title; status_.artist = artist; status_.playing = true; changed();
  }
  const std::vector<Contact>& phonebook() const { return phonebook_; }
  std::vector<Contact> contacts() const override { return phonebook_; }
  size_t contactCount() const override { return phonebook_.size(); }
  std::string contactsSource() const override { return status_.activeDeviceName; }
  void pullPhonebook() override { changed(); }   // already populated; just nudge a refresh
  uint8_t micGain() const { return micGain_; }

private:
  void changed() { if (cb_) cb_(); }
  void logLine(const std::string& l) { log_.push_back(l); if (log_.size() > 30) log_.pop_front(); }
  std::string nameFor(const std::string& mac) const {
    for (auto& c : pairbook_) if (c.number == mac) return c.name;
    return "DEVICE";
  }
  void applyTrack() {
    static const char* T[3][2] = {{"BOHEMIAN RHAPSODY","QUEEN"},{"STARMAN","BOWIE"},{"ONE","METALLICA"}};
    status_.title = T[track_][0]; status_.artist = T[track_][1]; status_.playing = true; changed();
  }
  int track_ = 0;

  BtStatus status_;
  std::function<void()> cb_;
  std::deque<std::string> log_;     // recent traffic for the BC127 debug view
  std::vector<Contact> pairbook_;   // paired devices (name/mac)
  std::vector<Contact> phonebook_;  // PBAP contacts (raw)
  Phonebook book_;                  // resolver over phonebook_
  uint8_t micGain_ = 8;
};

} // namespace mmi
