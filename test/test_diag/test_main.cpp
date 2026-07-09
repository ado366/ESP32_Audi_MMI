// Unit tests for diagnostics presets persistence + graph rendering (plan §4/§8).
#include <unity.h>
#include "../../src/diag/Presets.h"
#include "../../src/ui/GraphRenderer.h"
#include "../../src/hal/native/MemoryStorage.h"

using namespace mmi;

void test_presets_persist() {
  MemoryStorage st;
  { PresetStore ps;
    Preset p; p.ecu = 0x01; p.group = 115; p.valueIndex = 2; p.view = View::Graph;
    p.min = 0; p.max = 2; std::snprintf(p.label, 9, "BOOST");
    ps.add(p);
    ps.save(st);
  }
  PresetStore ps2; ps2.load(st);
  TEST_ASSERT_EQUAL_INT(1, ps2.size());
  TEST_ASSERT_EQUAL_UINT8(115, ps2.at(0).group);
  TEST_ASSERT_EQUAL_INT((int)View::Graph, (int)ps2.at(0).view);
  TEST_ASSERT_EQUAL_STRING("BOOST", ps2.at(0).label);
}

void test_presets_cap() {
  PresetStore ps;
  for (int i = 0; i < 20; ++i) { Preset p; TEST_ASSERT_TRUE(ps.size() < 9 ? ps.add(p) : !ps.add(p)); }
  TEST_ASSERT_EQUAL_INT(9, ps.size()); // MAX_PRESETS
}

static bool pixel(const std::vector<uint8_t>& b, int w, int x, int y) {
  size_t bit = (size_t)y * w + x;
  return (b[bit >> 3] >> (7 - (bit & 7))) & 1;
}

void test_graph_min_is_bottom_max_is_top() {
  const int w = 8, h = 8;
  std::vector<float> lo(w, 0.f), hi(w, 10.f);
  auto blo = GraphRenderer::render(lo, 0, 10, w, h);
  auto bhi = GraphRenderer::render(hi, 0, 10, w, h);
  // Value at min -> bottom row lit; value at max -> top row lit.
  TEST_ASSERT_TRUE(pixel(blo, w, w - 1, h - 1));
  TEST_ASSERT_TRUE(pixel(bhi, w, w - 1, 0));
}

void test_graph_clamps_out_of_range() {
  const int w = 4, h = 8;
  std::vector<float> over(w, 999.f);
  auto b = GraphRenderer::render(over, 0, 10, w, h); // clamps to top row, no crash
  TEST_ASSERT_TRUE(pixel(b, w, w - 1, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_presets_persist);
  RUN_TEST(test_presets_cap);
  RUN_TEST(test_graph_min_is_bottom_max_is_top);
  RUN_TEST(test_graph_clamps_out_of_range);
  return UNITY_END();
}
