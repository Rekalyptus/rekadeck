# Developing the Rekadeck

The device code lives in `firmware/lib/rekacore` and only talks to the hardware through one interface, `hal.h`. The
real board (`firmware/src`) and the desktop simulator (`sim/`) each implement it, so the simulator runs the exact
code the device runs, and anything you fix in the simulator is fixed on the device.

## Run it on your computer

You need Python 3.10 or newer, CMake and a C++17 compiler.

- macOS: `xcode-select --install`, then `brew install cmake`
- Debian or Ubuntu: `sudo apt install cmake g++ python3-venv`

**1. Start the mock server** (terminal 1)

```
cd server
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python -m rekaserver
```

It prints two addresses. The dashboard is http://127.0.0.1:8000. The other one is your computer's LAN address,
which a real board needs if you point it at this server.

**2. Build and start the simulator** (terminal 2)

```
cd sim
cmake -B build
cmake --build build -j
./build/rekadeck_sim
```

Then open http://127.0.0.1:8090: the screen, the four keys, and switches for the things that are hard to do to real
hardware.

**3. First-time setup**

The simulator does it for you by default: it joins **Home WiFi** and approves its own link code on the server, so
the first card shows up a few seconds after it starts. Switch off "Skip Wi-Fi setup and linking" in the right column
to go through it by hand:

1. The simulated Rekadeck starts on "Set up Wi-Fi". Click "Open the setup page" in the right column. That page is
   what your phone would see after joining the Rekadeck's own network.
2. Pick **Home WiFi** and type `flashcards` (try a wrong password first to see the error). The server address is
   filled in already.
3. The device shows a link code. Open the dashboard, click "Link a device" and type the code. A few seconds later the
   first card appears.

**4. Review**

Press keys 1 to 4 on your keyboard or click the keys under the screen. The key map is in the
[README](../README.md#using-it).

**5. Watch the panel**

The simulated screen behaves like the real panel, with the refresh times measured on a real board (see
[HARDWARE.md](HARDWARE.md#the-display)). The Panel section on the right lists every refresh with its time and the
reason the device did it. Switch on the tint to see what each refresh draws (green) and erases (orange): erased ink
is what turns into ghosts. The ghosting is a rough model.

A partial refresh takes the same time whether it changes one word or the whole card, so the number of refreshes is
what costs time, not their size. Once partial refreshes have erased 15% of the panel's area in ink
(`clean_after_pct` on the server), the next page turn uses a fast refresh to clean up. A text card erases 3 to 6%
when it's turned, so that's every third to fifth card.

**6. Things worth trying**

- Dashboard, "Review 3 cards on my phone": the device notices at its next check-in, downloads a new batch in the
  background and swaps it in between cards.
- Turn the Wi-Fi router off, review a few cards, let it sleep (Skip 2 minutes), turn the router back on and press a
  key. The offline reviews upload with their real review times.
- "Fire the wake timer" while it sleeps: the quiet check-in every few hours that keeps the due count current.
- Clock drift: the simulated clock runs 2% fast. The dashboard's drift column shows the server working that out and
  correcting review times.
- "Pull the battery" and "Factory reset" exercise the power-on and blank-device paths.

The simulator keeps its flash and settings in `sim_data/` next to where you start it. Delete that folder for a
brand-new device. The server's database is `server/rekasim.db`, and the dashboard's Reset button reseeds it.

## Tests

The same three jobs run in CI (`.github/workflows/ci.yml`).

```
# unit tests for the device code, against an in-memory board
cmake -S tests/core -B build/core
cmake --build build/core -j
ctest --test-dir build/core --output-on-failure

# end-to-end: the simulator running the device code, syncing with the mock server
cmake -S sim -B sim/build && cmake --build sim/build -j
pip install -r server/requirements.txt pytest
pytest tests/sim -q

# the firmware for the real board, both environments
pio run -d firmware -e rekadeck -e release
```

## Build for the real board

The firmware is a PlatformIO project. Install the PlatformIO extension for VS Code, or the command line tool
(`pip install platformio`, or `brew install platformio` on macOS), then:

```
cd firmware
pio run -e release -t upload    # build and flash over USB-C
pio device monitor              # serial log, 115200 baud
```

There are two environments:

- `release` connects to `https://rekalyptus.com` unless you type another server in the setup page. This is the one
  the installers and the over-the-air updates ship.
- `rekadeck` (the default for a bare `pio run`) is for development against the mock server. Set its
  `REKA_DEFAULT_SERVER` in `platformio.ini` to the LAN address the mock server prints, or type the address on the
  setup page under Advanced.

Plain `http://` works for a server on your own network. Anything else should be HTTPS: the device trusts the root
certificates in `firmware/src/ca_roots.h` (regenerate it with `tools/make_ca_roots.py`).

Pins are in `firmware/src/pins.h` and the flash layout in `firmware/partitions.csv`: two 1.44 MB app slots for
over-the-air updates, 1 MB of LittleFS for the cards and the review log, and a crash dump.

## Making a release

Set `FW_VERSION` in `firmware/lib/rekacore/src/api.h` (dotted numbers, compared numerically), then run:

```
python3 tools/release.py
```

It builds the release env, refuses an app image that doesn't fit an app slot, and writes:

```
firmware/prebuilt/                              what flash.sh and flash.bat install (commit it)
release/<version>/rekadeck-<version>.bin        the app image devices download over Wi-Fi
release/<version>/rekadeck-<version>-full.bin   bootloader, partition table and app in one image
release/<version>/manifest.json                 the web installer's manifest
release/manifest.json                           a server's update manifest, with the app image next to it
```

`--no-build` packages the last build instead (it refuses one older than the source), and `--channel beta` adds a
release no device is offered.

**Updates over Wi-Fi.** A server offers devices the newest stable release in its firmware folder, newer than what
they run (PROTOCOL.md, "Firmware updates"). Copy `release/rekadeck-<version>.bin` there first, then
`release/manifest.json`. The mock server does the same from `REKASIM_FIRMWARE_DIR`: start it with
`REKASIM_FIRMWARE_DIR=../release` and a simulated or real device on an older version is offered the newest release.

**The web installer.** `flasher/index.html` installs the full image from desktop Chrome or Edge using ESP Web Tools,
with every file it loads pinned by hash. `python3 tools/release.py --flasher-dir site` puts the page, its manifest
and the full image in `site/`. Host that folder over HTTPS (browsers only allow USB access from secure pages;
`python3 -m http.server -d site` serves it on localhost for a test). The full image starts at address 0, so it also
clears the board's Wi-Fi settings and link.

## Things that bit us on real hardware

- The ESP32-S3 ROM has its own, older miniz, and `esp32s3.rom.ld` assigns the `tinfl_*` and `mz_*` names to it, so
  the link silently used the ROM's inflater with its bigger struct. `vendor/miniz.h` renames ours (`reka_*`), and CI
  checks the image links ours.
- Arduino's loop task has an 8 KB stack, which the inflater alone overflowed. `main.cpp` raises it to 16 KB.
- The ESP-IDF system time doesn't always start at 0 after a reset, so `rtcMs()` measures from its own zero point.
- Phones joining the setup network kept retrying DHCP at full transmit power, so setup runs the radio at 8.5 dBm
  and goes back to full power for the home network.
- With `Serial.setTxTimeoutMs(0)`, the Arduino core's USB serial blocks forever once a host has opened the port and
  closed it again: the device froze until something read the port. The firmware uses a 5 ms timeout.
- The Arduino core's log lines carry their source file's full path, which would put the build machine's user name
  into the image. `firmware/scripts/build_paths.py` maps those paths to a neutral prefix.
