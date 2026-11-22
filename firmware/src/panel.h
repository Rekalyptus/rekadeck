// Driver for the Waveshare 7.5" e-Paper V2 (800 x 480, black and white, UC8179 controller), with all three
// refresh modes the panel has:
//
//   Full     ~4 s, several flashes, temperature compensated. Clears every trace of ghosting.
//   Fast     ~1.2 s, one flash. Clears nearly all ghosting.
//   Partial  ~0.4 s, no flash, only pixels that change move. Leaves faint ghosts that add up.
//
// The register sequences follow Waveshare's own reference driver (EPD_7in5_V2.c from waveshareteam/e-Paper,
// MIT license, Copyright (c) Waveshare team). Timings are the ones measured on this panel by the GxEPD2 author.
#pragma once
#include <Arduino.h>

#include "hal.h"

class Panel {
 public:
  Panel(int sck, int mosi, int cs, int dc, int rst, int busy);
  void begin();   // pins and SPI. The HAT must already have power.

  // Shows `next` (our format: rows of 100 bytes, 1 = black). `previous` is what the panel shows now and is
  // required for a partial refresh, since the controller only moves pixels that differ from it.
  // Returns false if the panel didn't answer in time.
  bool refresh(const uint8_t* next, const uint8_t* previous, reka::Refresh mode);

  void sleep();   // deep sleep before the board sleeps and the HAT loses power. refresh() wakes it again.

 private:
  void reset();
  void command(uint8_t c);
  void data(uint8_t d);
  void data(const uint8_t* d, size_t n);
  void image(uint8_t command, const uint8_t* fb, bool invert);
  bool waitIdle(uint32_t timeout_ms);
  bool powerOn();

  int sck_, mosi_, cs_, dc_, rst_, busy_;
};
