// A 1-bit framebuffer with just enough drawing for the device's own screens, plus frame inflation for cards.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "fonts.h"
#include "hal.h"

namespace reka {

constexpr int STRIP_Y = 432;                      // key label strip, rows 432..479
constexpr int KEY_X[4] = {100, 300, 500, 700};    // screen x right above each key

uint32_t crc32(const uint8_t* data, size_t n);
// zlib stream -> exactly FB_BYTES of frame, checked against crc. False if anything is off.
bool inflateFrame(const std::string& z, uint32_t crc, uint8_t* out);

class Gfx {
 public:
  uint8_t fb[FB_BYTES];

  void clear() { std::fill(fb, fb + FB_BYTES, 0); }
  void load(const uint8_t* frame) { std::copy(frame, frame + FB_BYTES, fb); }
  void pixel(int x, int y, bool black);
  void fill(int x, int y, int w, int h, bool black = true);
  void invert(int x, int y, int w, int h);
  void frame(int x, int y, int w, int h, int t);              // outline t pixels thick
  void pill(int x, int y, int w, int h, bool black = true);   // a filled capsule, its ends rounded by h / 2
  void clearRows(int y0, int y1) { fill(0, y0, SCREEN_W, y1 - y0, false); }
  void copyRows(const uint8_t* src, int y0, int y1);          // rows from another full frame

  // y is the top of the line, returns end x. black = false draws white ink, for text on a filled shape.
  int text(const Font& f, int x, int y, const std::string& s, bool black = true);
  int width(const Font& f, const std::string& s) const;
  void centered(const Font& f, int cx, int y, const std::string& s, bool black = true) {
    text(f, cx - width(f, s) / 2, y, s, black);
  }
  int paragraph(const Font& f, int x, int y, int w, const std::string& s, int line_h = 0);  // wraps, returns bottom
  void strip(const char* labels[4]);                          // a label above each key
  void stripText(const std::string& s);                       // one line centred in the strip
  int qr(const std::string& text, int x, int y, int module);  // returns the side length drawn, 0 on failure
};

}  // namespace reka
