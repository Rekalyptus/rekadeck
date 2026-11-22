#include "panel.h"

#include <SPI.h>

// UC8179 commands used here
static const uint8_t PSR = 0x00, PWR = 0x01, POF = 0x02, PON = 0x04, BTST = 0x06, DSLP = 0x07, DTM1 = 0x10,
                     DRF = 0x12, DTM2 = 0x13, DUSPI = 0x15, CDI = 0x50, TCON = 0x60, TRES = 0x61, PTL = 0x90,
                     PTIN = 0x91, CCSET = 0xE0, TSSET = 0xE5;

static const SPISettings SPI_CFG(10000000, MSBFIRST, SPI_MODE0);   // the controller takes up to 20 MHz
static const int ROW = reka::ROW_BYTES, ROWS = reka::SCREEN_H;

Panel::Panel(int sck, int mosi, int cs, int dc, int rst, int busy)
    : sck_(sck), mosi_(mosi), cs_(cs), dc_(dc), rst_(rst), busy_(busy) {}

void Panel::begin() {
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);
  pinMode(dc_, OUTPUT);
  pinMode(rst_, OUTPUT);
  digitalWrite(rst_, HIGH);
  pinMode(busy_, INPUT);
  SPI.begin(sck_, -1, mosi_, cs_);
}

void Panel::reset() {
  digitalWrite(rst_, HIGH);
  delay(20);
  digitalWrite(rst_, LOW);
  delay(2);
  digitalWrite(rst_, HIGH);
  delay(20);
}

void Panel::command(uint8_t c) {
  SPI.beginTransaction(SPI_CFG);
  digitalWrite(dc_, LOW);
  digitalWrite(cs_, LOW);
  SPI.transfer(c);
  digitalWrite(cs_, HIGH);
  SPI.endTransaction();
}

void Panel::data(uint8_t d) { data(&d, 1); }

void Panel::data(const uint8_t* d, size_t n) {
  SPI.beginTransaction(SPI_CFG);
  digitalWrite(dc_, HIGH);
  digitalWrite(cs_, LOW);
  SPI.writeBytes(d, n);
  digitalWrite(cs_, HIGH);
  SPI.endTransaction();
}

void Panel::image(uint8_t cmd, const uint8_t* fb, bool invert) {
  command(cmd);
  uint8_t row[ROW];
  for (int y = 0; y < ROWS; y++) {
    const uint8_t* src = fb + y * ROW;
    if (invert) {
      for (int i = 0; i < ROW; i++) row[i] = ~src[i];
      data(row, ROW);
    } else {
      data(src, ROW);
    }
  }
}

bool Panel::waitIdle(uint32_t timeout_ms) {
  // BUSY reads low while the controller works
  uint32_t t0 = millis();
  delay(1);
  while (digitalRead(busy_) == LOW) {
    if (millis() - t0 > timeout_ms) return false;
    delay(5);
  }
  return true;
}

bool Panel::powerOn() {
  command(PON);
  delay(100);
  return waitIdle(1000);
}

bool Panel::refresh(const uint8_t* next, const uint8_t* previous, reka::Refresh mode) {
  if (mode == reka::Refresh::Partial && !previous) mode = reka::Refresh::Fast;   // no reference picture to diff against
  reset();   // also wakes the controller from deep sleep
  bool ok;
  if (mode == reka::Refresh::Partial) {
    // Waveshare EPD_7IN5_V2_Init_Part + Display_Part
    command(PSR); data(0x1F);                     // black/white, waveforms from OTP
    ok = powerOn();
    command(CCSET); data(0x02);                   // fixed temperature...
    command(TSSET); data(0x6E);                   // ...of 110, which picks the panel's fast partial waveform
    command(CDI); data(0xA9); data(0x07);         // copy new to old after the refresh, 1 = white
    command(PTIN);                                // partial mode, window = the whole panel
    command(PTL);
    const uint8_t win[] = {0x00, 0x00, 0x03, 0x1F, 0x00, 0x00, 0x01, 0xDF, 0x01};   // x 0..799, y 0..479
    data(win, sizeof win);
    image(DTM1, previous, true);                  // what's on the panel now (old picture)...
    image(DTM2, next, true);                      // ...and what it should become. Only differences move.
  } else {
    if (mode == reka::Refresh::Full) {
      // Waveshare EPD_7IN5_V2_Init
      command(PWR); { const uint8_t d[] = {0x07, 0x07, 0x3F, 0x3F}; data(d, sizeof d); }
      command(BTST); { const uint8_t d[] = {0x17, 0x17, 0x28, 0x17}; data(d, sizeof d); }
      ok = powerOn();
      command(PSR); data(0x1F);
      command(TRES); { const uint8_t d[] = {0x03, 0x20, 0x01, 0xE0}; data(d, sizeof d); }
      command(DUSPI); data(0x00);
      command(CDI); data(0x10); data(0x07);
      command(TCON); data(0x22);
    } else {
      // Waveshare EPD_7IN5_V2_Init_Fast
      command(PSR); data(0x1F);
      command(CDI); data(0x10); data(0x07);
      ok = powerOn();
      command(BTST); { const uint8_t d[] = {0x27, 0x27, 0x18, 0x17}; data(d, sizeof d); }
      command(CCSET); data(0x02);                 // fixed temperature of 90: the fast full waveform
      command(TSSET); data(0x5A);
    }
    // Waveshare EPD_7IN5_V2_Display: old picture 1 = white, new picture 1 = black
    image(DTM1, next, true);
    image(DTM2, next, false);
  }
  command(DRF);
  delay(100);
  ok = waitIdle(mode == reka::Refresh::Full ? 8000 : 4000) && ok;
  command(POF);                                   // stop the drive voltages, the picture stays
  waitIdle(500);
  return ok;
}

void Panel::sleep() {
  // Waveshare EPD_7IN5_V2_Sleep
  command(CDI); data(0xF7);
  command(POF);
  waitIdle(500);
  command(DSLP); data(0xA5);
}
