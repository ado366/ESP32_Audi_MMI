// Unit tests for the PBAP Phonebook (plan §3c / §8).
#include <unity.h>
#include "../../src/bt/Phonebook.h"

using namespace mmi;

void test_parse_pbap_stream() {
  Phonebook pb;
  const std::string s =
    "PBAP_PB NAME: Alice\r"
    "PBAP_PB TEL: +41 79 123 45 67\r"
    "PBAP_PB NAME: Bob\r"
    "PBAP_PB TEL: 0797654321\r"
    "PBAP_PB OK\r";
  size_t n = pb.loadFromPbap(s, 100);
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_STRING("Alice", pb.entries()[0].name.c_str());
}

void test_lookup_tolerant_matching() {
  Phonebook pb;
  pb.add("Alice", "+41791234567", 100);
  // Incoming CLI comes with different formatting but same last 8 digits.
  TEST_ASSERT_EQUAL_STRING("Alice", pb.lookup("079 123 45 67").c_str());
  TEST_ASSERT_EQUAL_STRING("Alice", pb.lookup("0041791234567").c_str());
  TEST_ASSERT_EQUAL_STRING("", pb.lookup("+41799999999").c_str());
}

void test_max_entries_cap() {
  Phonebook pb;
  std::string s;
  for (int i = 0; i < 10; ++i)
    s += "PBAP_PB NAME: C" + std::to_string(i) + "\rPBAP_PB TEL: 0790000" + std::to_string(100 + i) + "\r";
  s += "PBAP_PB OK\r";
  size_t n = pb.loadFromPbap(s, 3); // cap at 3 ("memory permitting")
  TEST_ASSERT_EQUAL_UINT(3, n);
}

void test_normalize() {
  TEST_ASSERT_EQUAL_STRING("91234567", Phonebook::normalize("+41 79 123 45 67").c_str());
  TEST_ASSERT_EQUAL_STRING("", Phonebook::normalize("no-digits").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_pbap_stream);
  RUN_TEST(test_lookup_tolerant_matching);
  RUN_TEST(test_max_entries_cap);
  RUN_TEST(test_normalize);
  return UNITY_END();
}
