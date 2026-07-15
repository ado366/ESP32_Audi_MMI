// Esp32Inputs.cpp — the encoder ISR lives out-of-line: an IRAM_ATTR function
// defined inline in the header triggers the Xtensa "dangerous relocation:
// literal placed after use" linker error.
#ifdef ARDUINO_ARCH_ESP32
#include "Esp32Inputs.h"

namespace mmi {

void IRAM_ATTR Esp32Inputs::encIsrThunk() {
  static DRAM_ATTR const int8_t tbl[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
  Esp32Inputs* self = s_self;
  if (!self) return;
  uint8_t s = (uint8_t)((digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B));
  self->isrAccum_ += tbl[(self->isrLast_ << 2) | s];
  self->isrLast_ = s;
}

} // namespace mmi
#endif
