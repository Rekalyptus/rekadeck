// Rekadeck firmware entry point. Every wake from deep sleep starts here again, like a fresh boot.
#include <Arduino.h>

#include "device.h"
#include "hal_esp32.h"

// Arduino's default 8 KB loop stack is tight for HTTP + JSON + rendering in one call chain
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static Esp32Hal hal;
static reka::Device* device = nullptr;

void setup() {
  hal.begin();
  device = new reka::Device(hal);
  device->setup();   // may go straight back to sleep (a timer check-in, a flat battery)
}

void loop() {
  device->loop();    // returns once it is time to sleep, but deep sleep never comes back here
  hal.idle();
}
