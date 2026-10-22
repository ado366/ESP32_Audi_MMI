// FisFont.h — pixel widths of the cluster's COMPACT (proportional) FIS font,
// so text can be positioned exactly (e.g. flush to the right edge). Each width
// includes the 1px gap after the glyph. Values for printable ASCII 0x20-0x7E
// from TLBFISLib's TLBFIS_COMPACT_FONT_WIDTHS (domnulvlad/TLBFISLib, GPL-3.0).
#pragma once
#include <cstdint>

namespace mmi {

inline uint8_t fisCompactCharWidth(char ch) {
  static const uint8_t k[95] = {
    // ' ' !  "  #  $  %  &  '  (  )  *  +  ,  -  .  /
       2, 2, 4, 6, 6, 6, 6, 2, 3, 3, 4, 6, 3, 4, 3, 4,
    // 0  1  2  3  4  5  6  7  8  9  :  ;  <  =  >  ?
       5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 3, 3, 4, 4, 4, 5,
    // @  A  B  C  D  E  F  G  H  I  J  K  L  M  N  O
       6, 5, 5, 5, 5, 4, 4, 5, 5, 4, 4, 5, 4, 6, 5, 5,
    // P  Q  R  S  T  U  V  W  X  Y  Z  [  \  ]  ^  _
       5, 5, 5, 5, 4, 5, 6, 6, 4, 4, 4, 3, 4, 3, 6, 5,
    // `  a  b  c  d  e  f  g  h  i  j  k  l  m  n  o
       3, 5, 5, 5, 5, 5, 4, 5, 5, 2, 4, 5, 4, 6, 5, 5,
    // p  q  r  s  t  u  v  w  x  y  z  {  |  }  ~
       5, 5, 4, 4, 4, 5, 4, 6, 4, 4, 4, 4, 2, 4, 6,
  };
  uint8_t c = static_cast<uint8_t>(ch);
  return (c >= 0x20 && c <= 0x7E) ? k[c - 0x20] : 5;
}

constexpr int kFisDegreeW = 4;   // width of the ° glyph (cluster code 0xBB) in the compact font

// Width in pixels of a string in the compact font (sum of char widths, which
// include each glyph's trailing 1px gap — so the last lit column is x+w-2).
inline int fisCompactWidth(const char* s) {
  int w = 0;
  for (; s && *s; ++s) w += fisCompactCharWidth(*s);
  return w;
}

} // namespace mmi
