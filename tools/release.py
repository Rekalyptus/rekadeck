#!/usr/bin/env python3
"""Cuts a Rekadeck firmware release. Run from anywhere:

    python3 tools/release.py                      build the release firmware and package it
    python3 tools/release.py --no-build           package what the last build left in firmware/.pio
    python3 tools/release.py --flasher-dir site   also put together a folder to host the web installer from

Writes release/<version>/ (the version comes from FW_VERSION in firmware/lib/rekacore/src/api.h):

    rekadeck-<version>.bin        the app image devices download over Wi-Fi
    rekadeck-<version>-full.bin   bootloader, partition table and app in one image, for a blank board
    manifest.json                 the web installer's manifest (ESP Web Tools)

and adds the version to the server's update manifest (release/manifest.json, PROTOCOL.md "Firmware updates") with a
copy of the app image next to it, so that folder holds exactly what goes in the server's firmware folder. Nothing is
uploaded: the summary prints the commands for that.

A stable build of the release env also replaces firmware/prebuilt/, which is what flash.sh and flash.bat install.

Standard library only (Python 3.10+). Needs PlatformIO's `pio` on the PATH.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
API_H = FIRMWARE / "lib" / "rekacore" / "src" / "api.h"
FLASHER_PAGE = ROOT / "flasher" / "index.html"
PREBUILT = FIRMWARE / "prebuilt"

CHIP = "esp32s3"
BOOTLOADER_AT = 0x0           # where the ESP32-S3 ROM looks for the second-stage bootloader
PARTITION_TABLE_AT = 0x8000   # ESP-IDF's default, which the Arduino core uses

# What flash.sh and tools/flash.ps1 write, part by part: offsets and flash settings are spelled out in both
FLASH_SCRIPTS_LAYOUT = {"otadata": 0xE000, "app": 0x10000}
FLASH_SCRIPTS_SETTINGS = ("dio", "80m", "4MB")

# Image header encodings (byte 2 = flash mode, byte 3 = size << 4 | frequency), as esptool writes them for the S3
FLASH_MODES = {0: "qio", 1: "qout", 2: "dio", 3: "dout"}
FLASH_SIZES = {0: "1MB", 1: "2MB", 2: "4MB", 3: "8MB", 4: "16MB", 5: "32MB", 6: "64MB", 7: "128MB"}
FLASH_FREQS = {0xF: "80m", 0x0: "40m", 0x1: "26m", 0x2: "20m"}


def die(msg):
    sys.stdout.flush()
    print(f"release.py: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def version_key(v):
    """'0.3.0' -> (0, 3). Dotted numbers compared numerically, trailing zeros don't count."""
    parts = [int(x) for x in v.split(".")]
    while parts and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def rel(path):
    """Shortest readable form of a path for the summary."""
    path = Path(path).resolve()
    for base in (Path.cwd(), ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


def write_json(path, data):
    """Write through a temp file and rename, so a reader never sees half a manifest."""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".tmp-", suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o644)   # mkstemp makes it private; the server and web host need to read it
    os.replace(tmp, path)


# ------------------------------------------------------------------ what the build is
def read_version():
    m = re.search(r'FW_VERSION\s*=\s*"([^"]*)"', API_H.read_text())
    if not m:
        die(f"no FW_VERSION in {rel(API_H)}")
    v = m.group(1)
    if not re.fullmatch(r"\d+(\.\d+)*", v):
        die(f"FW_VERSION is {v!r}: it has to be dotted numbers like 0.3.0, devices and the server compare it numerically")
    return v


def parse_size(s):
    s = s.strip()
    if s[-1:].upper() in ("K", "M"):
        return int(s[:-1], 0) * (1024 if s[-1].upper() == "K" else 1024 * 1024)
    return int(s, 0)


def read_partitions():
    """subtype -> (offset, size) from partitions.csv"""
    parts = {}
    for line in (FIRMWARE / "partitions.csv").read_text().splitlines():
        fields = [f.strip() for f in line.split("#", 1)[0].split(",")]
        if len(fields) >= 5 and fields[3]:
            parts[fields[2]] = (parse_size(fields[3]), parse_size(fields[4]))
    for need in ("ota", "ota_0", "ota_1"):
        if need not in parts:
            die(f"partitions.csv has no {need} partition, so this isn't the over-the-air layout release.py expects")
    return parts


def default_server(env):
    """The REKA_DEFAULT_SERVER this env compiles in, from platformio.ini, or None."""
    text = (FIRMWARE / "platformio.ini").read_text()
    m = re.search(r"^\[env:" + re.escape(env) + r"\]\s*$(.*?)(?=^\[|\Z)", text, re.M | re.S)
    if not m:
        die(f"platformio.ini has no [env:{env}]")
    s = re.search(r"-DREKA_DEFAULT_SERVER='\"([^\"]+)\"'", m.group(1))
    return s.group(1) if s else None


def flash_params(image):
    head = image.read_bytes()[:4]
    if len(head) < 4 or head[0] != 0xE9:
        die(f"{rel(image)} isn't an ESP32 image")
    mode, size, freq = FLASH_MODES.get(head[2]), FLASH_SIZES.get(head[3] >> 4), FLASH_FREQS.get(head[3] & 0xF)
    if not (mode and size and freq):
        die(f"can't read the flash settings in {rel(image)}'s header ({head.hex()})")
    return mode, freq, size


def pio_paths():
    """PlatformIO's own Python (esptool runs under it) and its packages folder."""
    try:
        out = subprocess.run(["pio", "system", "info", "--json-output"], check=True, capture_output=True, text=True).stdout
    except FileNotFoundError:
        die("pio isn't on the PATH (brew install platformio, or the PlatformIO extension's shell)")
    except subprocess.CalledProcessError as e:
        die(f"pio system info failed:\n{e.stderr}")
    info = json.loads(out)
    return info["python_exe"]["value"], Path(info["core_dir"]["value"]) / "packages"


def newest_source_mtime():
    paths = [FIRMWARE / "platformio.ini", FIRMWARE / "partitions.csv"]
    for d in (FIRMWARE / "src", FIRMWARE / "lib"):
        paths += [p for p in d.rglob("*") if p.is_file()]
    return max(p.stat().st_mtime for p in paths)


# ------------------------------------------------------------------ the release
def main():
    if sys.version_info < (3, 10):
        die("needs Python 3.10 or newer")
    ap = argparse.ArgumentParser(description="Build and package a Rekadeck firmware release.")
    ap.add_argument("--env", default="release", help="PlatformIO environment to build (default: release)")
    ap.add_argument("--no-build", action="store_true", help="package the existing build output instead of building")
    ap.add_argument("--out", type=Path, default=ROOT / "release", help="output folder (default: release/)")
    ap.add_argument("--server-manifest", type=Path, default=None,
                    help="the server's update manifest to add this release to (default: <out>/manifest.json)")
    ap.add_argument("--channel", default="stable", help="release channel; devices are only offered stable (default)")
    ap.add_argument("--flasher-dir", type=Path, default=None,
                    help="also put the web installer (index.html, manifest.json, full image) in this folder")
    args = ap.parse_args()

    version = read_version()
    parts = read_partitions()
    server = default_server(args.env)
    build = FIRMWARE / ".pio" / "build" / args.env

    if not args.no_build:
        print(f"Building {args.env} (firmware {version})...", flush=True)
        r = subprocess.run(["pio", "run", "-d", str(FIRMWARE), "-e", args.env])
        if r.returncode != 0:
            die(f"pio run -e {args.env} failed")

    bootloader, table, app = build / "bootloader.bin", build / "partitions.bin", build / "firmware.bin"
    for f in (bootloader, table, app):
        if not f.is_file():
            die(f"{rel(f)} is missing; run without --no-build")
    if args.no_build and app.stat().st_mtime < newest_source_mtime():
        die(f"{rel(app)} is older than the firmware source; run without --no-build")

    image = app.read_bytes()
    if (version + "\0").encode() not in image:
        die(f"{rel(app)} doesn't contain FW_VERSION {version!r}: it was built from other source")
    if server and (server + "\0").encode() not in image:
        die(f"{rel(app)} doesn't contain {args.env}'s default server {server}: it was built for another env")

    # Both app slots have to hold it: an update always goes into whichever one isn't running
    slot = min(parts["ota_0"][1], parts["ota_1"][1])
    margin = slot - len(image)
    fit = f"app image {len(image):,} bytes, app slot {slot:,} bytes (0x{slot:X})"
    if margin < 0:
        die(f"{fit}: {-margin:,} bytes too big, not releasing it")
    print(f"\nSize: {fit}, {margin:,} bytes to spare ({100 * len(image) / slot:.1f}% used)")

    python, packages = pio_paths()
    esptool = packages / "tool-esptoolpy" / "esptool.py"
    boot_app0 = packages / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"
    for f in (esptool, boot_app0):
        if not f.is_file():
            die(f"{f} is missing (pio run installs it)")

    # Flash settings exactly as the build wrote them into the bootloader header (PlatformIO turns the board's qio
    # into dio there: the ROM loads the bootloader in dio and the qio bootloader switches to quad itself).
    mode, freq, size = flash_params(bootloader)
    if flash_params(app) != (mode, freq, size):
        die(f"bootloader.bin and firmware.bin disagree on flash settings: {(mode, freq, size)} vs {flash_params(app)}")

    pieces = [(BOOTLOADER_AT, bootloader), (PARTITION_TABLE_AT, table), (parts["ota"][0], boot_app0),
              (parts["ota_0"][0], app)]
    for (a, fa), (b, _) in zip(pieces, pieces[1:]):
        if a + fa.stat().st_size > b:
            die(f"{fa.name} at 0x{a:X} runs into the next part at 0x{b:X}")

    out = args.out.resolve() / version
    ota_bin = out / f"rekadeck-{version}.bin"
    full_bin = out / f"rekadeck-{version}-full.bin"
    web_manifest = out / "manifest.json"
    server_manifest = (args.server_manifest or args.out / "manifest.json").resolve()
    if server_manifest == web_manifest:
        die("--server-manifest can't be the web installer's manifest.json")
    if server_manifest.exists():
        try:
            server_data = json.loads(server_manifest.read_text())
        except ValueError as e:
            die(f"{rel(server_manifest)} isn't valid JSON: {e}")
        if not isinstance(server_data, dict) or not isinstance(server_data.get("releases"), list):
            die(f'{rel(server_manifest)} isn\'t a {{"releases": [...]}} manifest')
    else:
        server_data = {"releases": []}
    if out.exists():
        print(f"Replacing the files already in {rel(out)}")
    out.mkdir(parents=True, exist_ok=True)

    ota_bin.write_bytes(image)   # the exact bytes checked above

    cmd = [python, str(esptool), "--chip", CHIP, "merge_bin", "-o", str(full_bin),
           "--flash_mode", mode, "--flash_freq", freq, "--flash_size", size]
    for at, f in pieces:
        cmd += [f"0x{at:x}", str(f)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die(f"esptool merge_bin failed:\n{r.stdout}{r.stderr}")
    merged = full_bin.read_bytes()
    for at, f in pieces:   # every part lands where it belongs, unchanged
        part = image if f == app else f.read_bytes()
        if merged[at:at + len(part)] != part:
            die(f"{full_bin.name} doesn't hold {f.name} intact at 0x{at:X}")

    write_json(web_manifest, {
        "name": "Rekadeck",
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,   # the firmware doesn't speak Improv, so don't wait 10 s for it
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": full_bin.name, "offset": 0}]}],
    })

    # ---- the server's update manifest, PROTOCOL.md "Firmware updates"
    ota_sha = sha256(ota_bin)
    releases = [r for r in server_data["releases"] if not (isinstance(r, dict) and r.get("version") == version)]
    replaced = len(releases) != len(server_data["releases"])
    releases.append({"version": version, "file": ota_bin.name, "size": len(image), "sha256": ota_sha,
                     "channel": args.channel})

    def order(r):
        v = r.get("version") if isinstance(r, dict) else None
        return version_key(v) if isinstance(v, str) and re.fullmatch(r"\d+(\.\d+)*", v) else ()
    releases.sort(key=order, reverse=True)
    server_data["releases"] = releases
    server_dir = server_manifest.parent
    server_dir.mkdir(parents=True, exist_ok=True)
    if (server_dir / ota_bin.name).resolve() != ota_bin:
        shutil.copyfile(ota_bin, server_dir / ota_bin.name)   # the image goes next to the manifest that names it
    write_json(server_manifest, server_data)

    # ---- what flash.sh and flash.bat install: the parts at their own offsets rather than the full image, so a board
    # updated over USB keeps its settings (Wi-Fi, server, link in NVS). Only a stable release-env build goes there.
    prebuilt = args.env == "release" and args.channel == "stable"
    if prebuilt:
        layout = {"otadata": parts["ota"][0], "app": parts["ota_0"][0]}
        if layout != FLASH_SCRIPTS_LAYOUT or (mode, freq, size) != FLASH_SCRIPTS_SETTINGS:
            die(f"flash.sh and tools/flash.ps1 write {FLASH_SCRIPTS_LAYOUT} with {FLASH_SCRIPTS_SETTINGS}, but this "
                f"build is {layout} with {(mode, freq, size)}: update both scripts and FLASH_SCRIPTS_* here")
        PREBUILT.mkdir(parents=True, exist_ok=True)
        names = [(bootloader, "bootloader.bin"), (table, "partitions.bin"), (boot_app0, "boot_app0.bin"),
                 (ota_bin, "rekadeck.bin")]
        for src, name in names:
            shutil.copyfile(src, PREBUILT / name)
        (PREBUILT / "VERSION").write_text(version + "\n")
        (PREBUILT / "SHA256SUMS").write_text("".join(f"{sha256(PREBUILT / n)}  {n}\n" for _, n in names))

    # ---- optional: a folder to host the web installer from
    if args.flasher_dir:
        site = args.flasher_dir.resolve()
        site.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(FLASHER_PAGE, site / "index.html")
        shutil.copyfile(web_manifest, site / "manifest.json")
        shutil.copyfile(full_bin, site / full_bin.name)

    # ---- summary
    print(f"\nRekadeck {version}, built from env {args.env}, default server {server or '(firmware default)'}")
    if args.env != "release":
        print(f"  Not the release env: a blank board installed from this build looks for "
              f"{server or 'the default in device.h'}, not https://rekalyptus.com")
    print(f"Flash settings from the build: {mode}, {freq}, {size}\n")
    for f in (ota_bin, full_bin, web_manifest):
        print(f"  {rel(f)}")
        print(f"      {f.stat().st_size:>9,} bytes  sha256 {sha256(f)}")
    print(f"\nServer update manifest {rel(server_manifest)} ({'replaced' if replaced else 'added'} {version}, "
          f"channel {args.channel}):")
    for r in releases:
        if not isinstance(r, dict):
            print(f"  ? {r!r}")
            continue
        f = server_dir / str(r.get("file", ""))
        if not f.is_file():
            state = "file not in this folder"
        elif f.stat().st_size != r.get("size") or sha256(f) != r.get("sha256"):
            state = "FILE DOESN'T MATCH, the server will skip it"
        else:
            state = "ok"
        print(f"  {r.get('version')}  {r.get('channel')}  {r.get('file')}  {r.get('size')} bytes  {state}")
    if args.channel != "stable":
        print(f"  Devices are only offered stable releases, so nothing will install {version} yet.")
    if args.flasher_dir:
        print(f"\nWeb installer in {rel(site)}: index.html, manifest.json, {full_bin.name}")
        print("  Host it over HTTPS (or open it from localhost); Web Serial doesn't work on plain http.")
    if prebuilt:
        print(f"\nflash.sh and flash.bat now install {version} ({rel(PREBUILT)}). Commit that folder with the release.")

    print("\nTo offer it to devices, copy the image and then the manifest into the server's firmware folder (not run):")
    print(f"  scp {server_dir / ota_bin.name} {server_manifest} <user>@<server>:")
    print("then on the server, with FIRMWARE_DIR set to that folder:")
    print(f'  cp ~/{ota_bin.name} "$FIRMWARE_DIR/{ota_bin.name}"')
    print(f'  cp ~/{server_manifest.name} "$FIRMWARE_DIR/manifest.json.new"')
    print('  mv "$FIRMWARE_DIR/manifest.json.new" "$FIRMWARE_DIR/manifest.json"')
    print("Devices pick it up at their next check-in and install it the next time they go to sleep.")


if __name__ == "__main__":
    main()
