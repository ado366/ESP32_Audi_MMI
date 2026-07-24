// Unit tests for the coarse-to-fine KWP timing search (KwpTuneSearch).
// We can't exercise the real K-line off-car, so these drive the pure search with
// a synthetic scoring function whose optimum is known and assert it homes in on
// it, plus that save()/load() resumes an interrupted sweep to the same result.
#include <unity.h>
#include "../../src/diag/KwpTuneSearch.h"
#include <cstdlib>

using namespace mmi;

// Synthetic K-line: reads/window peak at (init=206, byte=6, frame=0) and fall off
// with distance. Scaled like a real read count (~0..90) with a gradient steeper
// than the ~10% hysteresis margin, so the optimum is genuinely resolvable.
static int synthScore(const KwpTiming& t) {
  int s = 90 - std::abs(t.init - 206) * 8 - std::abs(t.byte - 6) * 12 - std::abs(t.frame - 0) * 4;
  return s < 0 ? 0 : s;
}

// Drive a search to completion against a scorer, capping iterations for safety.
static void runToDone(KwpTuneSearch& s, int (*score)(const KwpTiming&)) {
  int guard = 0;
  while (s.active() && guard++ < 1000) s.report(score(s.current()));
}

void test_totals_and_progress() {
  TEST_ASSERT_EQUAL_INT(33, KwpTuneSearch::totalCands());   // 5+6+3 + 7+7+5
  KwpTuneSearch s; s.start();
  TEST_ASSERT_TRUE(s.active());
  TEST_ASSERT_EQUAL_INT(0, s.candIndex());
  s.report(100);
  TEST_ASSERT_EQUAL_INT(1, s.candIndex());
}

void test_homes_in_on_optimum() {
  KwpTuneSearch s; s.start();
  runToDone(s, synthScore);
  TEST_ASSERT_TRUE(s.done());
  TEST_ASSERT_FALSE(s.active());
  KwpTiming b = s.best();
  TEST_ASSERT_EQUAL_INT(206, b.init);    // coarse 205 -> fine +1
  TEST_ASSERT_EQUAL_INT(6, b.byte);      // coarse 7   -> fine -1
  TEST_ASSERT_EQUAL_INT(0, b.frame);     // coarse 0
}

// Interrupt the sweep partway, persist the POD, restore into a fresh instance and
// finish — must land on the same optimum (models a key-off mid-drive).
void test_resume_across_reboot() {
  KwpTuneSearch s; s.start();
  for (int i = 0; i < 9 && s.active(); ++i) s.report(synthScore(s.current()));  // stop mid-sweep
  KwpTuneSearch::State snap = s.save();
  TEST_ASSERT_TRUE(snap.active && !snap.done);

  KwpTuneSearch s2; s2.load(snap);       // "reboot": new object, restored state
  runToDone(s2, synthScore);
  TEST_ASSERT_TRUE(s2.done());
  KwpTiming b = s2.best();
  TEST_ASSERT_EQUAL_INT(206, b.init);
  TEST_ASSERT_EQUAL_INT(6, b.byte);
  TEST_ASSERT_EQUAL_INT(0, b.frame);
}

// A K-line that never reads (all scores 0) must leave the baseline untouched.
static int deadScore(const KwpTiming&) { return 0; }
void test_dead_kline_keeps_baseline() {
  KwpTuneSearch s; s.start(200, 5, 0);
  runToDone(s, deadScore);
  TEST_ASSERT_TRUE(s.done());
  KwpTiming b = s.best();
  TEST_ASSERT_EQUAL_INT(200, b.init);
  TEST_ASSERT_EQUAL_INT(5, b.byte);
  TEST_ASSERT_EQUAL_INT(0, b.frame);
}

// A flat "plateau" of equally-good timings (byte anywhere in [4,10] reads ~80).
// Within the plateau, differences are below the noise margin, so the fine stage
// must NOT drift to an extreme — it should settle inside the good region.
static int plateauByte(const KwpTiming& t) {
  int d = t.byte < 4 ? (4 - t.byte) : (t.byte > 10 ? t.byte - 10 : 0);
  int s = 80 - d * 20; return s < 0 ? 0 : s;
}
void test_fine_stage_holds_on_flat_top() {
  KwpTuneSearch s; s.start(200, 5, 0);
  runToDone(s, plateauByte);
  TEST_ASSERT_TRUE(s.done());
  int b = s.best().byte;
  TEST_ASSERT_TRUE(b >= 4 && b <= 10);   // stays in the flat good region, no drift to 0/50
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_totals_and_progress);
  RUN_TEST(test_homes_in_on_optimum);
  RUN_TEST(test_resume_across_reboot);
  RUN_TEST(test_dead_kline_keeps_baseline);
  RUN_TEST(test_fine_stage_holds_on_flat_top);
  return UNITY_END();
}
