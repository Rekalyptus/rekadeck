#!/usr/bin/env bash
# Rekadeck firmware installer for macOS and Linux. On Windows, use flash.bat.
#
#   ./flash.sh               install or update the firmware on a Rekadeck plugged in over USB-C. An update keeps
#                            the board's Wi-Fi, its link to your account and any reviews it hasn't synced yet.
#   ./flash.sh --erase       wipe the board completely first, for a fresh start
#   ./flash.sh --port PORT   use this serial port instead of looking for the board, like /dev/ttyACM0 on Linux
#                            or /dev/cu.usbmodem101 on macOS
#   ./flash.sh --check       get everything ready (check the firmware files, download esptool) and stop there
#
# The first run downloads Espressif's esptool, the program that writes the firmware, into .tools/ in this folder
# (about 50 MB, only once). Nothing is installed anywhere else. Set ESPTOOL=/path/to/esptool to use your own.

set -euo pipefail

# esptool's standalone builds from https://github.com/espressif/esptool/releases, checked against these hashes
ESPTOOL_VERSION="v4.13.0"
esptool_sha256() {
  case "$1" in
    macos-arm64)   echo d3f7d76a1dddcbbdaafc3df2d5e859aed998945ee2b91b6dbfb2a190359b5c5c ;;
    macos-amd64)   echo 25301d9e9e0a249bae9be952bf0f129190d100601b972417193822996e53e704 ;;
    linux-amd64)   echo 2b17f7de233ca885ddf7d1c0f5c71b3d46821a07ecc682337de8a01d42fa15b3 ;;
    linux-aarch64) echo 076036e63d107c57d7e432c70af9b0375b45570f89a6cd1a24cd24600018b1e0 ;;
    linux-armv7)   echo b54d1bb0cd2d265fd5775ebc24b225e907321c43c7522b23627e671a0786bdaf ;;
  esac
}

ESP_USB_VENDOR="303a"   # Espressif: the ESP32-S3's own USB port, which is how the Rekadeck's board connects
WAIT_S=300              # how long to wait for the board to show up

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$HERE/firmware/prebuilt"
TOOLS="$HERE/.tools"

say()  { printf '%s\n' "$*"; }
fail() { printf '\nError: %s\n' "$*" >&2; exit 1; }
usage() { awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; }

ERASE=0
CHECK=0
PORT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --erase) ERASE=1 ;;
    --check) CHECK=1 ;;
    --port)
      [ $# -ge 2 ] || fail "--port needs a port name, like /dev/ttyACM0"
      PORT="$2"
      shift ;;
    --port=*) PORT="${1#--port=}" ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option $1 (./flash.sh --help lists them)" ;;
  esac
  shift
done
PORT_GIVEN="$PORT"
case "$PORT_GIVEN" in ''|/*) ;; *) PORT_GIVEN="/dev/$PORT_GIVEN" ;; esac   # --port ttyACM0 means /dev/ttyACM0

case "$(uname -s)" in
  Darwin) OS=macos ;;
  Linux) OS=linux ;;
  *) fail "this installer is for macOS and Linux. On Windows, run flash.bat." ;;
esac

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

# ------------------------------------------------------------------ the firmware in this folder
check_firmware() {
  local f sum name
  for f in bootloader.bin partitions.bin boot_app0.bin rekadeck.bin SHA256SUMS VERSION; do
    [ -f "$FW/$f" ] || fail "firmware/prebuilt/$f is missing. Download this folder again."
  done
  while read -r sum name; do
    name="${name%$'\r'}"
    [ -n "$sum" ] || continue
    [ -f "$FW/$name" ] || fail "firmware/prebuilt/$name is missing. Download this folder again."
    [ "$(sha256 "$FW/$name")" = "$sum" ] ||
      fail "firmware/prebuilt/$name is damaged (its checksum doesn't match). Download this folder again."
  done < "$FW/SHA256SUMS"
  VERSION="$(tr -d '[:space:]' < "$FW/VERSION")"
}

# ------------------------------------------------------------------ esptool
download() {   # url file
  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --progress-bar -o "$2" "$1"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "$2" "$1"
  else
    fail "this installer needs curl or wget to download esptool"
  fi
}

get_esptool() {
  if [ -n "${ESPTOOL:-}" ]; then
    command -v "$ESPTOOL" >/dev/null 2>&1 || fail "ESPTOOL is set to $ESPTOOL, which isn't a program that runs"
    ESPTOOL_CMD=("$ESPTOOL")
    return 0
  fi

  local arch asset sum dir exe tmp
  arch="$(uname -m)"
  case "$OS-$arch" in
    macos-arm64)                   asset=macos-arm64 ;;
    macos-x86_64)                  asset=macos-amd64 ;;
    linux-x86_64)                  asset=linux-amd64 ;;
    linux-aarch64|linux-arm64)
      # a 64-bit kernel can run a 32-bit system (32-bit Raspberry Pi OS on a Pi 4 or 5), which needs the 32-bit build
      if [ "$(getconf LONG_BIT 2>/dev/null || echo 64)" = 32 ]; then asset=linux-armv7; else asset=linux-aarch64; fi ;;
    linux-armv7l|linux-armv8l)     asset=linux-armv7 ;;
    *)                             asset="" ;;
  esac
  if [ -z "$asset" ]; then
    own_esptool || fail "Espressif has no ready-made esptool for $OS on $arch. Install it with
    python3 -m pip install esptool
and run this again."
    return 0
  fi

  dir="$TOOLS/esptool-$ESPTOOL_VERSION-$asset"
  exe="$dir/esptool-$asset/esptool"
  if [ ! -x "$exe" ]; then
    sum="$(esptool_sha256 "$asset")"
    tmp="$TOOLS/esptool-download.tar.gz"
    command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 ||
      fail "this installer needs curl or wget to download esptool. Install curl with your package manager
(for example: sudo apt install curl) and run this again."
    mkdir -p "$TOOLS"
    say "Downloading esptool $ESPTOOL_VERSION, the program that writes the firmware (about 50 MB, only this once)..."
    download "https://github.com/espressif/esptool/releases/download/$ESPTOOL_VERSION/esptool-$ESPTOOL_VERSION-$asset.tar.gz" "$tmp" ||
      fail "couldn't download esptool. Check your internet connection and try again."
    if [ "$(sha256 "$tmp")" != "$sum" ]; then
      rm -f "$tmp"
      fail "the esptool download didn't match its checksum, so it wasn't used. Try again."
    fi
    rm -rf "$dir.part" "$dir"
    mkdir -p "$dir.part"
    tar -xzf "$tmp" -C "$dir.part"
    rm -f "$tmp"
    mv "$dir.part" "$dir"
    [ -x "$exe" ] || fail "the esptool download didn't contain esptool-$asset/esptool"
  fi
  if ! "$exe" version >/dev/null 2>&1; then
    own_esptool || fail "the esptool downloaded for $OS on $arch doesn't run on this computer. Install it with
    python3 -m pip install esptool
and run this again."
    return 0
  fi
  ESPTOOL_CMD=("$exe")
}

own_esptool() {   # an esptool installed some other way (pip, a package manager)
  if command -v esptool >/dev/null 2>&1; then ESPTOOL_CMD=(esptool); return 0; fi
  if command -v esptool.py >/dev/null 2>&1; then ESPTOOL_CMD=(esptool.py); return 0; fi
  return 1
}

# ------------------------------------------------------------------ finding the board
board_ports() {   # every serial port that belongs to an Espressif USB device, one per line
  if [ "$OS" = linux ]; then
    local t
    for t in /sys/class/tty/ttyACM*; do
      if [ -r "$t/device/../idVendor" ] && [ "$(cat "$t/device/../idVendor")" = "$ESP_USB_VENDOR" ]; then
        echo "/dev/${t##*/}"
      fi
    done
  else
    { ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null || true; } |
      awk -v vid="$((16#$ESP_USB_VENDOR))" '
        /"idVendor" = / { v = $NF }
        /"IOCalloutDevice" = / { p = $NF; gsub(/"/, "", p); if (v == vid) print p }'
  fi
  return 0
}

other_ports() {   # serial ports that might be a board on a USB-to-serial chip, for the "not found" message
  local p
  for p in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
    if [ -e "$p" ]; then echo "    $p"; fi
  done
  return 0
}

no_board() {
  local others
  others="$(other_ports)"
  if [ -n "$PORT_GIVEN" ]; then
    [ -z "$others" ] || fail "$PORT_GIVEN isn't there. These serial ports are:
$others"
    fail "$PORT_GIVEN isn't there, and no serial ports that could be a board are plugged in."
  fi
  if [ -n "$others" ]; then
    fail "no Rekadeck showed up. These serial ports are here:
$others
If one of them is the Rekadeck, run ./flash.sh --port <that port>"
  fi
  fail "no Rekadeck showed up. Try another USB-C cable (some only charge) or another USB port, then run this again."
}

find_board() {
  local start=$SECONDS told=0 ports count reply wait_s=$WAIT_S
  if [ -n "$PORT_GIVEN" ]; then wait_s=15; fi
  while :; do
    if [ -n "$PORT_GIVEN" ]; then
      ports=""
      if [ -e "$PORT_GIVEN" ]; then ports="$PORT_GIVEN"; fi
    else
      ports="$(board_ports | sort -u)"
    fi
    count="$(printf '%s\n' "$ports" | grep -c '^/' || true)"
    if [ "$count" -eq 1 ]; then
      PORT="$ports"
      break
    fi
    if [ "$count" -gt 1 ]; then
      say ""
      say "More than one board is plugged in:"
      printf '%s\n' "$ports" | awk '{ printf "  %d  %s\n", NR, $0 }'
      [ -r /dev/tty ] || fail "pick one with --port"
      printf 'Which one is the Rekadeck? Type its number: '
      read -r reply < /dev/tty
      case "$reply" in ''|*[!0-9]*) fail "that isn't one of the numbers" ;; esac
      PORT="$(printf '%s\n' "$ports" | sed -n "${reply}p")"
      [ -n "$PORT" ] || fail "that isn't one of the numbers"
      break
    fi
    if [ "$told" -eq 0 ] && [ -n "$PORT_GIVEN" ]; then
      say ""
      say "Waiting for $PORT_GIVEN to show up..."
      told=2
    elif [ "$told" -eq 0 ]; then
      say ""
      say "Looking for the Rekadeck. Plug it in with a USB-C cable that carries data (some cables only charge)."
      say "If it's plugged in already, press one of its keys to wake it."
      told=1
    elif [ "$told" -eq 1 ] && [ $((SECONDS - start)) -ge 20 ]; then
      say "Still looking. Hold the board's BOOT button, press and let go of RESET, then let go of BOOT."
      told=2
    fi
    [ $((SECONDS - start)) -lt "$wait_s" ] || no_board
    sleep 0.2
  done

  if [ "$OS" = linux ]; then
    # A board that was just plugged in shows up a moment before the system has finished setting up its port
    local n=0 group
    while { [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; } && [ "$n" -lt 25 ]; do
      sleep 0.2
      n=$((n + 1))
    done
    if [ ! -r "$PORT" ] || [ ! -w "$PORT" ]; then
      group="$(stat -L -c %G "$PORT" 2>/dev/null || true)"
      case "$group" in
        ''|root|UNKNOWN)
          if getent group uucp >/dev/null 2>&1 && ! getent group dialout >/dev/null 2>&1; then group="uucp"
          else group="dialout"; fi ;;
      esac
      fail "found the board on $PORT, but your user isn't allowed to use it yet. Run this once:
    sudo usermod -aG $group $(id -un)
then log out and back in (or restart) and run ./flash.sh again. Running it with sudo works too."
    fi
  fi
  say "Found it on $PORT."
}

# ------------------------------------------------------------------ writing it
flash() {
  local args try
  for try in 1 2 3; do
    args=(--chip esp32s3 --port "$PORT" --baud 460800 --before default_reset --after hard_reset
          write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB)
    if [ "$ERASE" -eq 1 ]; then args+=(--erase-all); fi
    args+=(0x0 "$FW/bootloader.bin" 0x8000 "$FW/partitions.bin" 0xe000 "$FW/boot_app0.bin"
           0x10000 "$FW/rekadeck.bin")
    say ""
    if "${ESPTOOL_CMD[@]}" "${args[@]}"; then return 0; fi
    [ "$try" -lt 3 ] || break
    say ""
    say "That didn't work, trying again. If the board went to sleep, press one of its keys."
    sleep 2
    find_board   # it may have restarted and come back under another name
  done
  fail "couldn't write the firmware. Close anything else using the board (a serial monitor, the Arduino IDE),
then hold BOOT, press and let go of RESET, let go of BOOT, and run this again."
}

# ------------------------------------------------------------------ main
say "Rekadeck firmware installer"
check_firmware
say "Firmware $VERSION"
get_esptool

if [ "$CHECK" -eq 1 ]; then
  "${ESPTOOL_CMD[@]}" version || fail "esptool didn't run (${ESPTOOL_CMD[*]})"
  say "Ready: the firmware files check out and esptool runs."
  exit 0
fi

find_board
if [ "$ERASE" -eq 1 ]; then
  say "Wiping the board and installing firmware $VERSION. Don't unplug it until this says it's done."
else
  say "Installing firmware $VERSION. Don't unplug the board until this says it's done."
fi
flash

say ""
say "Done: Rekadeck firmware $VERSION is installed."
say ""
say "The board restarts by itself. If its screen hasn't changed after 15 seconds, press its RESET button."
say "A new board shows \"Set up Wi-Fi\": join the network it names from your phone, pick your home Wi-Fi,"
say "then link it to your account at the address it shows. The README has the details."
