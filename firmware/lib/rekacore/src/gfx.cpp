#include "gfx.h"

#include <cstring>

extern "C" {
#include "vendor/miniz.h"
#include "vendor/qrcodegen.h"
}

namespace reka {

uint32_t crc32(const uint8_t* data, size_t n) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

bool inflateFrame(const std::string& z, uint32_t crc, uint8_t* out) {
  // tinfl_decompress_mem_to_mem() puts its ~11 KB decompressor on the stack, more than the ESP32's whole 8 KB
  // loop task: the first batch download overflowed it on every boot. Same call, decompressor kept off the stack
  // (only the main loop ever inflates).
  static tinfl_decompressor decomp;
  tinfl_init(&decomp);
  size_t in_len = z.size(), out_len = FB_BYTES;
  tinfl_status status = tinfl_decompress(&decomp, (const mz_uint8*)z.data(), &in_len, out, out, &out_len,
                                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (status != TINFL_STATUS_DONE || out_len != (size_t)FB_BYTES) return false;
  return crc32(out, FB_BYTES) == crc;
}

void Gfx::pixel(int x, int y, bool black) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
  uint8_t& b = fb[y * ROW_BYTES + (x >> 3)];
  uint8_t m = 0x80 >> (x & 7);
  if (black) b |= m; else b &= ~m;
}

void Gfx::fill(int x, int y, int w, int h, bool black) {
  for (int yy = std::max(0, y); yy < std::min(SCREEN_H, y + h); yy++)
    for (int xx = std::max(0, x); xx < std::min(SCREEN_W, x + w); xx++) pixel(xx, yy, black);
}

void Gfx::invert(int x, int y, int w, int h) {
  for (int yy = std::max(0, y); yy < std::min(SCREEN_H, y + h); yy++)
    for (int xx = std::max(0, x); xx < std::min(SCREEN_W, x + w); xx++) fb[yy * ROW_BYTES + (xx >> 3)] ^= 0x80 >> (xx & 7);
}

void Gfx::frame(int x, int y, int w, int h, int t) {
  fill(x, y, w, t);
  fill(x, y + h - t, w, t);
  fill(x, y, t, h);
  fill(x + w - t, y, t, h);
}

void Gfx::pill(int x, int y, int w, int h, bool black) {
  // each row runs the full width, less what the round ends cut off at that height (measured at the pixel centre)
  int r2 = h * h;   // (2r)^2, everything below in half-pixel units to stay in integers
  for (int row = 0; row < h; row++) {
    int dy = 2 * row + 1 - h;
    int cut = 0;
    while (cut < h / 2 && (h - 2 * cut - 1) * (h - 2 * cut - 1) + dy * dy > r2) cut++;
    fill(x + cut, y + row, w - 2 * cut, 1, black);
  }
}

void Gfx::copyRows(const uint8_t* src, int y0, int y1) {
  std::memcpy(fb + y0 * ROW_BYTES, src + y0 * ROW_BYTES, (size_t)(y1 - y0) * ROW_BYTES);
}

int Gfx::text(const Font& f, int x, int y, const std::string& s, bool black) {
  for (unsigned char c : s) {
    if (c < 32 || c > 126) c = '?';
    const Glyph& g = f.glyphs[c - 32];
    const uint8_t* bits = f.bits + g.offset;
    int stride = (g.w + 7) / 8;
    for (int gy = 0; gy < g.h; gy++)
      for (int gx = 0; gx < g.w; gx++)
        if (bits[gy * stride + (gx >> 3)] & (0x80 >> (gx & 7))) pixel(x + g.x_off + gx, y + g.y_off + gy, black);
    x += g.advance;
  }
  return x;
}

int Gfx::width(const Font& f, const std::string& s) const {
  int w = 0;
  for (unsigned char c : s) w += f.glyphs[(c < 32 || c > 126 ? '?' : c) - 32].advance;
  return w;
}

int Gfx::paragraph(const Font& f, int x, int y, int w, const std::string& s, int line_h) {
  if (!line_h) line_h = f.line_h + f.line_h / 5;
  std::string line, word;
  auto flush = [&]() {
    text(f, x, y, line);
    y += line_h;
    line.clear();
  };
  for (size_t i = 0; i <= s.size(); i++) {
    char c = i < s.size() ? s[i] : ' ';
    if (c == ' ' || c == '\n') {
      std::string cand = line.empty() ? word : line + " " + word;
      if (!line.empty() && width(f, cand) > w) {
        flush();
        line = word;
      } else {
        line = cand;
      }
      word.clear();
      if (c == '\n') flush();
    } else {
      word += c;
    }
  }
  if (!line.empty()) flush();
  return y;
}

void Gfx::strip(const char* labels[4]) {
  clearRows(STRIP_Y, SCREEN_H);
  for (int i = 0; i < 4; i++)
    if (labels[i] && labels[i][0]) centered(FONT_SMALL_B, KEY_X[i], STRIP_Y + 14, labels[i]);
}

void Gfx::stripText(const std::string& s) {
  clearRows(STRIP_Y, SCREEN_H);
  centered(FONT_SMALL, SCREEN_W / 2, STRIP_Y + 14, s);
}

int Gfx::qr(const std::string& text, int x, int y, int module) {
  static uint8_t code[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  static uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
  if (!qrcodegen_encodeText(text.c_str(), temp, code, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true)) return 0;
  int n = qrcodegen_getSize(code);
  for (int qy = 0; qy < n; qy++)
    for (int qx = 0; qx < n; qx++)
      if (qrcodegen_getModule(code, qx, qy)) fill(x + qx * module, y + qy * module, module, module);
  return n * module;
}

}  // namespace reka
