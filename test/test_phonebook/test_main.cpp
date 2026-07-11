// Unit tests for the PBAP Phonebook (plan §3c / §8).
#include <unity.h>
#include "../../src/bt/Phonebook.h"
#include "../../src/ui/FisCharset.h"

using namespace mmi;

void test_parse_pbap_stream() {
  // Real Melody PB_PULL output: vCards, one per notification, terminated by OK.
  Phonebook pb;
  const std::string s =
    "PENDING\r"
    "PB_PULL 16 90 BEGIN:VCARD\r"
    "VERSION:2.1\r"
    "FN;CHARSET=UTF-8:Alice\r"
    "N;CHARSET=UTF-8:Smith;Alice;;;\r"
    "TEL;TYPE=CELL:+41 79 123 45 67\r"
    "END:VCARD\r"
    "PB_PULL 16 70 BEGIN:VCARD\r"
    "VERSION:2.1\r"
    "N;CHARSET=UTF-8:Jones;Bob;;;\r"     // no FN -> falls back to N ("Bob Jones")
    "TEL;TYPE=HOME:0797654321\r"
    "END:VCARD\r"
    "OK\r";
  size_t n = pb.loadFromPbap(s, 100);
  TEST_ASSERT_EQUAL_UINT(2, n);
  TEST_ASSERT_EQUAL_STRING("Alice", pb.entries()[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("+41 79 123 45 67", pb.entries()[0].number.c_str());
  TEST_ASSERT_EQUAL_STRING("Bob Jones", pb.entries()[1].name.c_str());
}

void test_incremental_feed() {
  // Same data fed one line at a time (as it arrives over serial).
  Phonebook pb; pb.beginPull();
  const char* lines[] = {
    "PB_PULL 16 90 BEGIN:VCARD", "FN:Carol", "TEL:0781112223", "END:VCARD" };
  for (auto l : lines) pb.feedLine(l, 100);
  TEST_ASSERT_EQUAL_UINT(1, pb.size());
  TEST_ASSERT_EQUAL_STRING("Carol", pb.entries()[0].name.c_str());
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
    s += "PB_PULL 16 50 BEGIN:VCARD\rFN:C" + std::to_string(i) +
         "\rTEL:0790000" + std::to_string(100 + i) + "\rEND:VCARD\r";
  s += "OK\r";
  size_t n = pb.loadFromPbap(s, 3); // cap at 3 ("memory permitting")
  TEST_ASSERT_EQUAL_UINT(3, n);
}

void test_fis_charset_mapping() {
  // Names are stored as UTF-8; the display maps them to THIS cluster's ROM codes
  // (read off the cluster): á=0xC0, š=0xCC, č=0xCB, ž=0xCD. Upper-cased.
  TEST_ASSERT_EQUAL_STRING("DOLE\xCD""AL", toFisText("Doležal").c_str());       // ž -> 0xCD
  TEST_ASSERT_EQUAL_STRING("TOMKOV\xC0", toFisText("Tomková").c_str());         // á -> 0xC0
  TEST_ASSERT_EQUAL_STRING("P\xCC\xCB""OLKA V\xC0""CLAV", toFisText("Pščolka Václav").c_str());  // š=0xCC, č=0xCB, á=0xC0
  TEST_ASSERT_EQUAL_STRING("HELLO", toFisText("hello").c_str());               // ASCII upper-cased
}

void test_normalize() {
  TEST_ASSERT_EQUAL_STRING("91234567", Phonebook::normalize("+41 79 123 45 67").c_str());
  TEST_ASSERT_EQUAL_STRING("", Phonebook::normalize("no-digits").c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_pbap_stream);
  RUN_TEST(test_incremental_feed);
  RUN_TEST(test_lookup_tolerant_matching);
  RUN_TEST(test_max_entries_cap);
  RUN_TEST(test_fis_charset_mapping);
  RUN_TEST(test_normalize);
  return UNITY_END();
}
