# Rekadeck

Rekadeck is a small e-paper flashcard device for [Rekalyptus](https://rekalyptus.com). It shows the cards you have
due, you grade each one with four keys, and it syncs your reviews over Wi-Fi. It works offline too: reviews wait on
the device until it can reach the server again.

This repository has everything you need to make one: a parts list, a 3D-printable case with its CAD files, an
assembly guide, the firmware, an installer for Windows, macOS and Linux, a desktop simulator, a mock server to develop
against, and the protocol the device speaks.

## What you need

| Part | Notes |
|---|---|
| SparkFun Thing Plus ESP32-S3 | The board the firmware is written for (ESP32-S3, 4 MB flash, 2 MB PSRAM) |
| Waveshare 7.5 inch e-Paper display, V2 | 800 x 480, black and white |
| Waveshare e-Paper Driver HAT | Connects the display to the board |
| 4 push buttons | Kailh Choc switches, or any momentary button wired to ground |
| LiPo battery with a 2-pin JST-PH plug | Optional. The board charges it over USB-C |
| USB-C cable that carries data | Some cables only charge |

To build the whole device, see [hardware/](hardware/): the full parts list with links, the case to 3D print, and the
assembly guide. Pin-by-pin wiring: [docs/HARDWARE.md](docs/HARDWARE.md).

## Install the firmware

1. Download this repository. Click **Code > Download ZIP** above and unzip it, or run:
   ```
   git clone https://github.com/Rekalyptus/rekadeck.git
   ```
2. Plug the board into your computer with the USB-C cable.
3. Run the installer from the folder you downloaded:
   - **Windows:** if you downloaded the ZIP, right-click it and choose **Extract All** first. Then double-click
     `flash.bat` in the extracted folder. If Windows says it protected your PC, click **More info**, then
     **Run anyway**.
   - **macOS or Linux:** open a terminal in the folder and run `./flash.sh`

The installer finds the board and writes the firmware. The first time, it also downloads esptool, Espressif's
program for writing ESP32 firmware (about 50 MB). esptool is kept in a `.tools` folder inside this one, and nothing
is installed anywhere else on your computer.

If the installer can't find the board:

- Press one of the Rekadeck's keys to wake it.
- Try another cable or USB port.
- Hold the board's **BOOT** button, press and let go of **RESET**, then let go of **BOOT**. Run the installer again.
- On Linux, if it says your user isn't allowed to use the port, run the command it prints, then log out and back in.

The installer has a few options. Run them from a terminal opened in this folder, like `./flash.sh --erase`, or
`.\flash.bat --erase` on Windows.

| Option | What it does |
|---|---|
| `--erase` | Wipes the board first: Wi-Fi, account link, cards, and any reviews that haven't synced |
| `--port <port>` | Uses this serial port instead of looking for the board, like `COM5`, `/dev/ttyACM0` or `/dev/cu.usbmodem101` |
| `--check` | Checks the firmware files and downloads esptool, without touching a board |

## Set it up

1. The Rekadeck starts on **Set up Wi-Fi** and shows a network name and password. Join that network from your
   phone, or scan the QR code on the screen.
2. A setup page opens. If it doesn't, go to `192.168.4.1` in your browser. Pick your home Wi-Fi and type its
   password.
3. The Rekadeck shows a link code. Go to the address on its screen, or scan the QR code, then sign in to Rekalyptus
   and enter the code.
4. It downloads your due cards and shows the first one.

Just built it? Before you set up Wi-Fi, press key 1 for **Hardware check**. It tests the screen, each key, the
battery, charging, storage, Wi-Fi and the server, and says how to fix the first problem it finds.

## Using it

| When | Keys |
|---|---|
| On a question | Any key shows the answer. On a multiple choice card, press the key under your answer |
| On an answer | 1 Again, 2 Hard, 3 Good, 4 Easy |
| Hold 1 | Undo the last grade |
| Hold 4 | Skip this card for now |
| Hold 1 and 4 for 2 seconds | Menu: sync now, info and updates, hardware check, Wi-Fi setup, unlink, erase, sleep |

The Rekadeck sleeps after a couple of minutes without a key press, and any key wakes it. While it sleeps, it checks
in every few hours to keep the count of due cards on its sleep screen current. It shows text cards. Cards that need
a picture, such as image occlusion, stay in the Rekalyptus app, and the sleep screen tells you how many there are.

## Updates

A Rekadeck that's set up updates itself over Wi-Fi. It installs a new version the next time it goes to sleep, as
long as it's on USB power or has at least 40% battery. To check now, open the menu and pick **Info and updates**.
Turn **Auto-update** off there if you'd rather install updates yourself.

You can also run the installer again at any time. It keeps the board's Wi-Fi, account link and unsynced reviews
unless you use `--erase`.

## For developers

You can run the device code on your computer without any hardware: [docs/DEVELOPING.md](docs/DEVELOPING.md) covers
the simulator, the mock server, building the firmware with PlatformIO, the tests, and making a release.
[PROTOCOL.md](PROTOCOL.md) documents the API between the device and its server.

```
flash.sh, flash.bat     the installers (flash.bat runs tools/flash.ps1)
hardware/               parts list, assembly guide, the case to 3D print (STL) and its CAD files (STEP)
firmware/               the ESP32-S3 firmware (PlatformIO). firmware/prebuilt/ is what the installers write
firmware/lib/rekacore/  the device itself: review loop, sync, sleep, setup, screens. Plain C++17, no Arduino
firmware/src/           the board layer: panel, Wi-Fi, flash, deep sleep, battery gauge
sim/                    a desktop simulator that runs the same device code in your browser
server/                 a mock server: device linking, card rendering, scheduling, a dashboard
tests/                  unit tests for the device code, and end-to-end tests of the simulator against the server
tools/                  release, font and certificate tools
flasher/                a web page that installs the firmware from Chrome or Edge
docs/                   hardware and developer notes
```

## License

MIT, see [LICENSE](LICENSE). That includes the case, its CAD files and the guides in `hardware/`. These third-party
parts keep their own licenses:

| Part | Where | License |
|---|---|---|
| ArduinoJson 7.2.1 | firmware/lib/rekacore/src/vendor | MIT |
| miniz 3.0.2 (inflate only) | firmware/lib/rekacore/src/vendor | MIT |
| QR Code generator (Nayuki) 1.8.0 | firmware/lib/rekacore/src/vendor | MIT |
| cpp-httplib 0.18.3 | sim/vendor (simulator only) | MIT |
| Inter 4.0 | server/rekaserver/fonts, and the glyphs built into fonts_data.cpp | SIL OFL 1.1 |
| Waveshare e-Paper reference driver | firmware/src/panel.cpp follows its register sequences | MIT |

The firmware images in `firmware/prebuilt/` also contain Espressif's
[ESP-IDF](https://github.com/espressif/esp-idf) (Apache 2.0) and the
[Arduino core for the ESP32](https://github.com/espressif/arduino-esp32) (LGPL 2.1), as built by PlatformIO.
You can rebuild them from this source, see [docs/DEVELOPING.md](docs/DEVELOPING.md).
