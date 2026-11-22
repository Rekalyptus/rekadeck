// Bitmap fonts baked from Inter by tools/make_fonts.py, for the device's own screens.
#pragma once
#include <cstdint>

namespace reka {

struct Glyph {
  uint32_t offset;   // into the font's bitmap, rows packed MSB first, ceil(w / 8) bytes per row
  uint8_t w, h;
  int8_t x_off;      // from the pen position
  int8_t y_off;      // from the top of the line
  uint8_t advance;
};

struct Font {
  const uint8_t* bits;
  const Glyph* glyphs;  // ASCII 32..126
  uint8_t line_h;
  uint8_t ascent;
};

extern const Font FONT_SMALL, FONT_SMALL_B, FONT_BODY, FONT_BODY_B, FONT_TITLE, FONT_CODE, FONT_BIG;

}  // namespace reka
