// Unit tests for MenuSystem navigation over the real menu tree.
#include <unity.h>
#include "../../src/ui/MenuSystem.h"
#include "../../src/ui/MenuTree.h"

using namespace mmi;

void test_starts_at_root() {
  MenuSystem m(menuRoot());
  TEST_ASSERT_TRUE(m.atRoot());
  TEST_ASSERT_EQUAL_STRING("PHONE", m.current().label);
}

void test_scroll_wraps() {
  MenuSystem m(menuRoot());
  m.scrollUp(); // wrap to last top-level item
  TEST_ASSERT_EQUAL_STRING("EXIT", m.current().label);
  m.scrollDown(); // back to first
  TEST_ASSERT_EQUAL_STRING("PHONE", m.current().label);
}

void test_descend_and_back() {
  MenuSystem m(menuRoot());
  m.select();                 // into PHONE
  TEST_ASSERT_FALSE(m.atRoot());
  TEST_ASSERT_EQUAL_STRING("PHONEBOOK", m.current().label);
  m.back();                   // back to root
  TEST_ASSERT_TRUE(m.atRoot());
  TEST_ASSERT_EQUAL_STRING("PHONE", m.current().label);
}

void test_leaf_fires_callback() {
  MenuSystem m(menuRoot());
  MenuId got = MenuId::None;
  m.onSelect([&](MenuId id){ got = id; });
  // Navigate to EXIT (last top-level item) and select it.
  m.scrollUp();
  TEST_ASSERT_EQUAL_STRING("EXIT", m.current().label);
  m.select();
  TEST_ASSERT_EQUAL(MenuId::Exit, got);
}

void test_traffic_shortcut_opens_diagnostics() {
  MenuSystem m(menuRoot());
  m.openTopLevel(diagnosticsTopIndex()); // robust to menu reordering
  TEST_ASSERT_FALSE(m.atRoot());
  TEST_ASSERT_EQUAL_STRING("SELECT ECU", m.current().label);  // first diag item
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_at_root);
  RUN_TEST(test_scroll_wraps);
  RUN_TEST(test_descend_and_back);
  RUN_TEST(test_leaf_fires_callback);
  RUN_TEST(test_traffic_shortcut_opens_diagnostics);
  return UNITY_END();
}
