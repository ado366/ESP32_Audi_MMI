// Unit tests for the InputRouter binding table (plan §2c).
// Runs on the native env via `pio test -e native`.
#include <unity.h>
#include "../../src/ui/InputRouter.h"

using namespace mmi;

void test_left_steering_is_volume_everywhere() {
  TEST_ASSERT_EQUAL(Action::VolumeUp,   InputRouter::resolve(Control::SteerLeftPlus,  Context::NowPlaying));
  TEST_ASSERT_EQUAL(Action::VolumeUp,   InputRouter::resolve(Control::SteerLeftPlus,  Context::Menu));
  TEST_ASSERT_EQUAL(Action::VolumeUp,   InputRouter::resolve(Control::SteerLeftPlus,  Context::ActiveCall));
  TEST_ASSERT_EQUAL(Action::VolumeDown, InputRouter::resolve(Control::SteerLeftMinus, Context::IncomingCall));
}

void test_right_steering_changes_tracks_when_idle() {
  TEST_ASSERT_EQUAL(Action::TrackNext, InputRouter::resolve(Control::SteerRightPlus,  Context::NowPlaying));
  TEST_ASSERT_EQUAL(Action::TrackPrev, InputRouter::resolve(Control::SteerRightMinus, Context::NowPlaying));
}

void test_right_steering_controls_calls() {
  TEST_ASSERT_EQUAL(Action::CallAnswer, InputRouter::resolve(Control::SteerRightPlus,  Context::IncomingCall));
  TEST_ASSERT_EQUAL(Action::CallReject, InputRouter::resolve(Control::SteerRightMinus, Context::IncomingCall));
  TEST_ASSERT_EQUAL(Action::CallEnd,    InputRouter::resolve(Control::SteerRightMinus, Context::ActiveCall));
}

void test_encoder_answers_and_ends_calls() {
  TEST_ASSERT_EQUAL(Action::CallAnswer, InputRouter::resolve(Control::EncoderClick, Context::IncomingCall));
  TEST_ASSERT_EQUAL(Action::CallEnd,    InputRouter::resolve(Control::EncoderHold,  Context::ActiveCall));
}

void test_menu_navigation() {
  TEST_ASSERT_EQUAL(Action::ScrollDown, InputRouter::resolve(Control::EncoderCW,    Context::Menu));
  TEST_ASSERT_EQUAL(Action::ScrollUp,   InputRouter::resolve(Control::EncoderCCW,   Context::Menu));
  TEST_ASSERT_EQUAL(Action::Select,     InputRouter::resolve(Control::EncoderClick, Context::Menu));
  TEST_ASSERT_EQUAL(Action::Back,       InputRouter::resolve(Control::Return,       Context::Menu));
}

void test_menu_toggle_and_diagnostics_shortcut() {
  TEST_ASSERT_EQUAL(Action::MenuOpenClose,   InputRouter::resolve(Control::Menu,    Context::NowPlaying));
  TEST_ASSERT_EQUAL(Action::MenuOpenClose,   InputRouter::resolve(Control::Menu,    Context::Menu));
  // Traffic button (car icon) jumps to diagnostics from idle and from a menu.
  TEST_ASSERT_EQUAL(Action::JumpDiagnostics, InputRouter::resolve(Control::Traffic, Context::NowPlaying));
  TEST_ASSERT_EQUAL(Action::JumpDiagnostics, InputRouter::resolve(Control::Traffic, Context::Menu));
}

void test_encoder_play_pause_when_idle() {
  TEST_ASSERT_EQUAL(Action::PlayPause, InputRouter::resolve(Control::EncoderClick, Context::NowPlaying));
}

void test_encoder_longpress_opens_menu_from_idle() {
  // Encoder-only menu entry so calibration is reachable without analog buttons.
  TEST_ASSERT_EQUAL(Action::MenuOpenClose, InputRouter::resolve(Control::EncoderHold, Context::NowPlaying));
}

void test_diagnostics_uses_menu_style_nav() {
  // On a diagnostics screen the encoder scrolls/selects (not media control).
  TEST_ASSERT_EQUAL(Action::Select,     InputRouter::resolve(Control::EncoderClick, Context::Diagnostics));
  TEST_ASSERT_EQUAL(Action::ScrollDown, InputRouter::resolve(Control::EncoderCW,    Context::Diagnostics));
}

void test_chords_are_assignable() {
  TEST_ASSERT_EQUAL(Action::Assignable, InputRouter::resolve(Control::SteerLPlusRPlus, Context::NowPlaying));
  TEST_ASSERT_EQUAL(Action::Assignable, InputRouter::resolve(Control::MenuNav,         Context::NowPlaying));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_left_steering_is_volume_everywhere);
  RUN_TEST(test_right_steering_changes_tracks_when_idle);
  RUN_TEST(test_right_steering_controls_calls);
  RUN_TEST(test_encoder_answers_and_ends_calls);
  RUN_TEST(test_menu_navigation);
  RUN_TEST(test_menu_toggle_and_diagnostics_shortcut);
  RUN_TEST(test_encoder_play_pause_when_idle);
  RUN_TEST(test_encoder_longpress_opens_menu_from_idle);
  RUN_TEST(test_diagnostics_uses_menu_style_nav);
  RUN_TEST(test_chords_are_assignable);
  return UNITY_END();
}
