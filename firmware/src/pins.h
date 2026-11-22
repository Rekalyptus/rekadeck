// Rekadeck wiring on the SparkFun Thing Plus ESP32-S3. Change these if you wire it differently.
#pragma once

// The four Choc keys, left to right. Each one shorts its pin to GND. They have to be RTC-capable pins
// (GPIO 0 to 21 on the S3) so they can wake the board from deep sleep.
static const int PIN_KEYS[4] = {1, 2, 4, 5};

// Waveshare e-Paper Driver HAT
static const int PIN_EPD_SCK = 12;
static const int PIN_EPD_MOSI = 11;
static const int PIN_EPD_CS = 10;
static const int PIN_EPD_DC = 14;
static const int PIN_EPD_RST = 15;
static const int PIN_EPD_BUSY = 16;
static const int PIN_EPD_PWR = 17;   // HAT rev 2.3 power switch, high = on

// MAX17048 fuel gauge on the board's own I2C bus (the Qwiic one)
static const int PIN_SDA = 8;
static const int PIN_SCL = 9;
static const uint8_t FUEL_GAUGE_ADDR = 0x36;

// Optional: a voltage divider from VBUS to a pin, so the firmware knows for sure when USB is plugged in.
// Leave at -1 to guess instead: a computer on the USB port counts while the battery isn't draining, a wall charger
// only while the battery is visibly charging.
static const int PIN_VBUS_SENSE = -1;

// The Thing Plus's own WS2812 RGB LED. Nothing uses it, but it keeps whatever colour it last latched (a glitch on
// its data line at power-up can light it), and it stays powered in deep sleep. The firmware writes "off" on every
// boot. The red PWR LED can't be switched from firmware: open the board's PWR solder jumper to stop its drain.
static const int PIN_RGB_LED = 46;
