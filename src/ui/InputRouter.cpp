// InputRouter.cpp — the binding table from plan §2c.
// Context-sensitive: the same physical control does different things depending
// on whether we're idle, in a menu, or handling a call.
#include "InputRouter.h"

namespace mmi {

Action InputRouter::resolve(Control c, Context ctx) {
  // --- Bindings that are the same in every context ---
  switch (c) {
    case Control::SteerLeftPlus:  return Action::VolumeUp;   // Left = volume, always
    case Control::SteerLeftMinus: return Action::VolumeDown;
    case Control::Menu:           return Action::MenuOpenClose;
    case Control::Info:           return Action::JumpSpeedo;  // quick-recall the speedometer
    default: break;
  }

  // --- Call contexts override the Right steering buttons + encoder ---
  if (ctx == Context::IncomingCall) {
    switch (c) {
      case Control::SteerRightPlus:  return Action::CallAnswer;
      case Control::SteerRightMinus: return Action::CallReject;
      case Control::EncoderClick:    return Action::CallAnswer;
      case Control::EncoderHold:     return Action::CallReject;
      case Control::Return:          return Action::CallReject;
      default: break;
    }
    return Action::None;
  }
  if (ctx == Context::ActiveCall) {
    switch (c) {
      case Control::SteerRightMinus: return Action::CallEnd;
      case Control::EncoderHold:     return Action::CallEnd;
      case Control::Return:          return Action::CallEnd;
      default: break;
    }
    return Action::None;
  }

  // --- Menu + Diagnostics screens: encoder navigates, console buttons select/back ---
  if (ctx == Context::Menu || ctx == Context::Diagnostics) {
    switch (c) {
      case Control::EncoderCW:    return Action::ScrollDown;
      case Control::EncoderCCW:   return Action::ScrollUp;
      case Control::EncoderClick: return Action::Select;
      case Control::EncoderHold:  return Action::RootBack;
      case Control::Return:       return Action::Back;
      case Control::Traffic:      return Action::JumpDiagnostics;
      // Right steering can page the list too (alt. scroll)
      case Control::SteerRightPlus:  return Action::ScrollDown;
      case Control::SteerRightMinus: return Action::ScrollUp;
      default: break;
    }
    return Action::None;
  }

  // --- Idle / NowPlaying: media + radio + shortcuts ---
  switch (c) {
    // Right steering + encoder rotate change the song (BT track) / tuner station
    // (radio) — App routes to the right target based on the head-unit source.
    case Control::SteerRightPlus:  return Action::TrackNext;
    case Control::SteerRightMinus: return Action::TrackPrev;
    case Control::EncoderCW:       return Action::TrackNext;  // rotate = next song on Now-Playing
    case Control::EncoderCCW:      return Action::TrackPrev;  // rotate = previous song
    case Control::EncoderClick:    return Action::PlayPause;
    // Long-press opens the menu. This is the encoder-ONLY entry path, so the
    // menu (and Debug -> Calibrate) is reachable even when the analog buttons
    // are miscalibrated / not yet working.
    case Control::EncoderHold:     return Action::MenuOpenClose;
    case Control::Traffic:         return Action::JumpDiagnostics; // car icon shortcut
    case Control::Nav:             return Action::JumpNowPlaying;
    case Control::Info:            return Action::CycleInfo;
    // L-R+ chord cycles the OEM radio's band/source (v1 behaviour).
    case Control::SteerLMinusRPlus: return Action::RadioSource;
    // Other chords are resolved against Settings at a higher layer.
    case Control::SteerLPlusRPlus:
    case Control::SteerLPlusRMinus:
    case Control::SteerLMinusRMinus:
    case Control::MenuReturn:
    case Control::MenuNav:
    case Control::TrafficReverse:
    case Control::InfoReverse:
      return Action::Assignable;
    default: break;
  }
  return Action::None;
}

} // namespace mmi
