// Unit tests for the rolling "oscilloscope" graph buffer (GraphScope).
#include <unity.h>
#include "../../src/ui/GraphScope.h"

using namespace mmi;

static constexpr int STEP = 2, GAP = 4;

void test_empty_after_reset() {
  GraphScope g; g.reset();
  for (int x = 0; x < GraphScope::kW; ++x)
    for (int y = 0; y < GraphScope::kH; ++y)
      TEST_ASSERT_FALSE(g.pixel(x, y));
  TEST_ASSERT_EQUAL_INT(0, g.cursor());
}

void test_first_sample_is_a_point_and_cursor_advances() {
  GraphScope g; g.reset();
  g.push(0, STEP, GAP);                       // top row
  TEST_ASSERT_TRUE(g.pixel(0, 0));            // both step columns drawn at y=0
  TEST_ASSERT_TRUE(g.pixel(1, 0));
  TEST_ASSERT_FALSE(g.pixel(0, 5));           // a point, not a fill
  TEST_ASSERT_EQUAL_INT(STEP, g.cursor());
}

void test_gap_is_cleared_ahead_of_cursor() {
  GraphScope g; g.reset();
  g.push(GraphScope::kH - 1, STEP, GAP);      // draw at bottom, columns 0..1
  // The GAP columns just ahead (2..2+GAP-1) must be blank.
  for (int x = STEP; x < STEP + GAP; ++x)
    for (int y = 0; y < GraphScope::kH; ++y)
      TEST_ASSERT_FALSE(g.pixel(x, y));
}

void test_connector_fills_between_samples() {
  GraphScope g; g.reset();
  g.push(0, STEP, GAP);                        // top
  g.push(GraphScope::kH - 1, STEP, GAP);       // bottom -> connector spans full height
  // The second sample's columns (2..3) should be filled from top to bottom.
  TEST_ASSERT_TRUE(g.pixel(STEP, 0));
  TEST_ASSERT_TRUE(g.pixel(STEP, GraphScope::kH / 2));
  TEST_ASSERT_TRUE(g.pixel(STEP, GraphScope::kH - 1));
}

void test_cursor_wraps_at_right_edge() {
  GraphScope g; g.reset();
  int pushes = GraphScope::kW / STEP;          // one full sweep
  for (int i = 0; i < pushes; ++i) g.push(0, STEP, GAP);
  TEST_ASSERT_EQUAL_INT(0, g.cursor());        // wrapped back to the left
  TEST_ASSERT_TRUE(g.atSweepStart());
}

void test_strip_byte_layout() {
  GraphScope g; g.reset();
  g.push(0, STEP, GAP);                         // cols 0,1 lit at row 0
  uint8_t s[GraphScope::kH];
  g.strip(0, s);
  TEST_ASSERT_EQUAL_HEX8(0xC0, s[0]);           // MSB=col0, next=col1
  TEST_ASSERT_EQUAL_HEX8(0x00, s[1]);           // row 1 blank
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_after_reset);
  RUN_TEST(test_first_sample_is_a_point_and_cursor_advances);
  RUN_TEST(test_gap_is_cleared_ahead_of_cursor);
  RUN_TEST(test_connector_fills_between_samples);
  RUN_TEST(test_cursor_wraps_at_right_edge);
  RUN_TEST(test_strip_byte_layout);
  return UNITY_END();
}
