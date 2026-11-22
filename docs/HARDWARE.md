# Rekadeck hardware

The electronics in detail. To build the whole device, with the case, the full parts list and the step-by-step
assembly guide, start at [hardware/](../hardware/).

## Parts

| Part | Notes |
|---|---|
| SparkFun Thing Plus ESP32-S3 | ESP32-S3-MINI-1 module with 4 MB flash and 2 MB PSRAM, a LiPo charger, a MAX17048 battery gauge, and USB-C wired straight to the ESP32-S3 |
| Waveshare 7.5 inch e-Paper display, V2 | 800 x 480, black and white, UC8179 controller |
| Waveshare e-Paper Driver HAT | Drives the display from SPI. The firmware also switches the display's power through the HAT's PWR pin |
| 4 push buttons | Written for Kailh Choc switches. Any momentary button works |
| LiPo battery, 2-pin JST-PH plug | Plugs into the board's battery socket. Optional: the Rekadeck runs from USB-C alone |

## Wiring

The display's ribbon cable goes into the Driver HAT. Then wire the HAT's pin header to the board:

| HAT pin | Thing Plus pin |
|---|---|
| VCC | 3V3 |
| GND | GND |
| DIN | GPIO 11 |
| CLK | GPIO 12 |
| CS | GPIO 10 |
| DC | GPIO 14 |
| RST | GPIO 15 |
| BUSY | GPIO 16 |
| PWR | GPIO 17 |

The four keys, left to right, go to GPIO 1, 2, 4 and 5. Wire one side of each key to its pin and the other side to
GND. No resistors: the firmware turns on the pins' own pull-ups, including during deep sleep, when a key press wakes
the board. Keys have to stay on GPIO 0 to 21, the pins that can wake an ESP32-S3.

The battery gauge is on the board (I2C on GPIO 8 and 9), so there's nothing to wire for it.

Wired it differently? Change `firmware/src/pins.h` and build the firmware yourself, see
[DEVELOPING.md](DEVELOPING.md).

After installing the firmware, run **Hardware check** (key 1 on the Wi-Fi setup screen, or from the menu). It walks
through the screen, each key, the battery gauge, charging, a flash write and read-back, Wi-Fi and the server, then
lists what passed and how to fix the first problem. Every result also goes to the serial log at 115200 baud.

## The display

The driver in `firmware/src/panel.cpp` follows Waveshare's reference code for the 7.5 inch V2 and uses three kinds
of refresh. Measured on a real board, including powering the panel up and down:

| Refresh | Time | What you see |
|---|---|---|
| Partial | about 0.75 s | Only the pixels that change move. Each one that goes from black to white leaves a faint ghost |
| Fast | about 1.8 s | The whole panel flashes dark and light, then the new picture appears. Clears nearly all ghosting |
| Full | about 2.9 s | Several flashes. Clears everything, and it's the only one that adjusts for temperature |

Turning to the next card and revealing an answer are partial refreshes. Every few cards, once the ghosts add up, a
page turn uses a fast refresh instead. The full refresh happens at most once a day, while the device goes to sleep.
If partial refreshes flash on your panel, it's probably from an earlier V2 batch, which needs a different partial
waveform.

## Battery life

The ESP32-S3 itself uses microamps in deep sleep, so most of the drain while asleep comes from the board's
always-on parts:

- **The red PWR LED** is lit whenever the board has power, and firmware can't switch it off. Cut the trace between
  the two pads of the **PWR** solder jumper on the back of the board. (The yellow CHG LED only lights while charging.)
- **The RGB LED** keeps whatever colour it last latched. The firmware writes "off" to it on every boot.
- **The display HAT** is switched off through its PWR pin while the board sleeps.

While awake, the firmware runs the CPU at 80 MHz and turns Wi-Fi off as soon as a sync is done. Every check-in sends
the battery voltage, time spent awake and number of wakes to the server, so drain can be measured rather than
guessed.

The firmware can't see the USB power line directly, so it guesses whether it's on USB: a computer counts while the
battery isn't draining, and a wall charger only while the battery is charging. For an exact answer, add a voltage
divider from VBUS to a spare pin and set `PIN_VBUS_SENSE` in `pins.h`.

The board also runs without a battery, from USB-C alone. Without a cell the gauge has no power and gives no reading,
so the device treats a computer on its USB port as USB power.
