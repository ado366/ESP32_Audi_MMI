// Unit tests for BluetoothManager single-active-device selection (plan §3 / §8).
#include <unity.h>
#include "../../src/bt/BluetoothManager.h"
#include "../../src/hal/native/FakeBluetooth.h"
#include "../../src/hal/native/MemoryStorage.h"

using namespace mmi;

static std::vector<PairedDevice> devices() {
  return {
    {"AA", "PIXEL",  0, true},
    {"BB", "IPHONE", 0, true},
    {"CC", "GALAXY", 0, true},
  };
}

void test_prefers_last_used_when_available() {
  FakeBluetooth bt; MemoryStorage st;
  BluetoothManager m(bt, st);
  m.setPaired(devices());
  m.onConnected("BB");                 // BB becomes last-used
  TEST_ASSERT_EQUAL_STRING("BB", m.preferred().c_str());
}

void test_falls_back_when_last_used_unavailable() {
  FakeBluetooth bt; MemoryStorage st;
  BluetoothManager m(bt, st);
  m.setPaired(devices());
  m.onConnected("BB");
  m.setAvailable("BB", false);         // last-used device out of range
  // Falls back to another available device (not empty, not BB).
  std::string p = m.preferred();
  TEST_ASSERT_FALSE(p.empty());
  TEST_ASSERT_TRUE(p != "BB");
}

void test_none_available_gives_empty() {
  FakeBluetooth bt; MemoryStorage st;
  BluetoothManager m(bt, st);
  m.setPaired(devices());
  m.setAvailable("AA", false);
  m.setAvailable("BB", false);
  m.setAvailable("CC", false);
  TEST_ASSERT_TRUE(m.preferred().empty());
}

void test_switch_updates_priority_and_single_link() {
  FakeBluetooth bt; MemoryStorage st;
  BluetoothManager m(bt, st);
  m.setPaired(devices());
  m.onConnected("AA");
  m.switchTo("CC");                    // manual switch
  TEST_ASSERT_EQUAL_STRING("CC", m.preferred().c_str());
  // FakeBluetooth enforces a single active link -> active device is CC only.
  TEST_ASSERT_EQUAL_STRING("CC", bt.status().activeDeviceMac.c_str());
}

void test_last_used_persists_across_restart() {
  MemoryStorage st;
  { FakeBluetooth bt; BluetoothManager m(bt, st); m.setPaired(devices()); m.onConnected("CC"); }
  // New manager instance, same storage -> remembers CC.
  FakeBluetooth bt2; BluetoothManager m2(bt2, st);
  m2.begin();
  m2.setPaired(devices());
  TEST_ASSERT_EQUAL_STRING("CC", m2.preferred().c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_prefers_last_used_when_available);
  RUN_TEST(test_falls_back_when_last_used_unavailable);
  RUN_TEST(test_none_available_gives_empty);
  RUN_TEST(test_switch_updates_priority_and_single_link);
  RUN_TEST(test_last_used_persists_across_restart);
  return UNITY_END();
}
