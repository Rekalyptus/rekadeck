"""End-to-end scenarios: the real device core in the simulator, syncing with the mock server.

Each test starts from a device that has just linked itself and is showing its first card (see conftest.py).
Menu positions, counted upward from "Back to cards" (the menu wraps): 1 Sleep now, 2 Erase, 4 Wi-Fi setup,
5 Hardware check.
"""

import hashlib
import json
import os
import time

import pytest

UP_SLEEP, UP_ERASE, UP_WIFI, UP_CHECK = 1, 2, 4, 5


def grade(sim, n: int) -> None:
    for _ in range(n):
        sim.key(3)   # reveal
        sim.key(3)   # Good


def test_first_sync_shows_a_card_and_turns_the_radio_off(sim):
    whys = [r["why"] for r in sim.refreshes()]
    assert "first card" in whys
    assert not sim.state()["wifi"]   # off between check-ins


def test_a_second_key_3_during_a_refresh_still_counts(sim):
    """Reveal, then grade before the reveal's refresh has finished: the old key logic lost the second tap."""
    mark = sim.mark()
    sim.key(3, then=0.02)                # reveal: the refresh starts while the key is still down
    sim.key(3, then=1.0)                 # Good, pressed during that refresh
    sim.wait_log("key 3 in review")
    sim.wait_log("key 3 in review")
    assert sum("key 3 in review" in line for line in sim.history[mark:]) == 2


@pytest.mark.server_env(REKASIM_APP_ONLY="3")
def test_picture_cards_are_counted_apart(sim):
    assert sim.logged("on this device")
    line = next(l for l in sim.history if l.startswith("checked in"))
    due, here = int(line.split(", ")[1].split()[0]), int(line.split(", ")[2].split()[0])
    assert here == due - 3


def test_offline_reviews_wait_on_the_device_and_upload_later(sim):
    sim.control(router=False)
    grade(sim, 2)
    sim.menu()
    sim.menu_pick(UP_SLEEP)
    sim.wait_log("sync failed: wi-fi didn't connect (1 in a row)")
    sim.wait_log("deep sleep, timer set for")
    assert sim.state()["asleep"]
    assert not sim.logged("sync failed: wi-fi didn't connect (2 in a row)")   # tried once, not again on the way down

    sim.control(router=True)
    sim.key(2)   # wake: the first card shows straight away, and Wi-Fi joins behind it
    sim.wait_log("wifi connected")
    assert not sim.logged("uploaded 2 reviews")   # not over the card: the check-in waits for the next one
    grade(sim, 1)
    sim.wait_log("uploaded 3 reviews")           # the two from offline and this one, on the Syncing screen
    sim.wait_log("checked in")
    assert "syncing" in [r["why"] for r in sim.refreshes()]


def start_check(sim) -> None:
    """Open the hardware check, start it, and answer the screen question once the pattern is up."""
    sim.menu()
    sim.menu_pick(UP_CHECK)
    sim.key(3, then=0)                   # Start: the test pattern
    sim.settle()
    sim.key(3, then=0)                   # Yes, the picture is right
    sim.wait_log("check: Screen ok")
    sim.settle()                         # the keys page is up


def test_hardware_check_walks_every_part(sim):
    start_check(sim)
    for k in (1, 2, 4, 3):
        sim.key(k, then=0.5)
    sim.wait_log("check: Keys ok, All four respond")
    sim.wait_log("check: Battery ok")
    sim.key(3, then=0.5)                 # Continue, then plug USB in
    sim.control(usb=True)
    sim.wait_log("check: Charging ok")
    sim.wait_log("check: Storage ok")
    sim.wait_log("check: Wi-Fi ok")
    sim.wait_log("check: Server ok")
    sim.control(usb=False)


def test_hardware_check_reports_a_dead_key_and_no_charging(sim):
    start_check(sim)
    for k in (1, 2, 3):                  # key 4 "broken"
        sim.key(k, then=0.5)
    sim.wait_log("check: Keys problem, No press from key 4", timeout=45)
    sim.key(3, then=0.5)                 # Continue, and never plug in
    sim.wait_log("check: Charging problem, No rise in 40 s", timeout=60)


def test_erase_forgets_everything_and_starts_setup_again(sim, tmp_path):
    sim.control(skip_setup=False)        # so it waits in setup afterwards instead of rejoining by itself
    grade(sim, 1)
    sim.menu()
    sim.menu_pick(UP_ERASE)
    sim.wait_log("key 3 in menu")
    sim.key(3)                           # Erase
    sim.wait_log("erasing everything")
    sim.wait_log("restarting")
    sim.wait_state("portal", True)       # a fresh device: Wi-Fi setup
    settings = json.loads((tmp_path / "sim" / "settings.json").read_text())
    assert "token" not in settings and "ssid" not in settings and "pass" not in settings
    assert not any((tmp_path / "sim" / "fs").iterdir())


def test_a_brownout_goes_to_charge_me_without_wifi(sim):
    mark = sim.mark()
    sim.control(brownout=True)           # the battery sagged under a Wi-Fi burst and reset the board
    sim.wait_log("charge me: brownout")
    sim.wait_state("asleep", True)
    assert not sim.logged("wifi connected", since=mark)


def test_a_weak_battery_keeps_reviewing_but_doesnt_sync(sim):
    mark = sim.mark()
    sim.control(battery=25)              # about 3.5 V in the simulator: under the Wi-Fi limit, over the cutoff
    grade(sim, 1)
    sim.menu()
    sim.key(2, then=0.4)                 # Sync now
    sim.key(3)
    sim.wait_log("no wi-fi until it's charged")
    assert not sim.logged("sync failed", since=mark)   # a weak battery isn't a server failure


def _firmware_dir(version: str, good_hash: bool = True):
    def make(tmp_path):
        d = tmp_path / "firmware"
        d.mkdir()
        image = os.urandom(150_000)
        (d / f"rekadeck-{version}.bin").write_bytes(image)
        sha = hashlib.sha256(image).hexdigest()
        (d / "manifest.json").write_text(json.dumps({"releases": [{
            "version": version, "file": f"rekadeck-{version}.bin", "size": len(image),
            "sha256": sha if good_hash else "0" * 64, "channel": "stable"}]}))
        return str(d)
    return make


@pytest.mark.server_env(REKASIM_FIRMWARE_DIR=_firmware_dir("9.9.9"))
def test_an_offered_firmware_installs_at_sleep_and_restarts(sim):
    if not sim.logged("firmware 9.9.9 is available"):
        sim.wait_log("firmware 9.9.9 is available", timeout=10)
    sim.menu()
    sim.menu_pick(UP_SLEEP)
    sim.wait_log("installing firmware 9.9.9")
    sim.wait_log("sim: firmware 9.9.9 written")
    sim.wait_log("restarting")
    assert sim.state()["installed_fw"] == "9.9.9"


@pytest.mark.server_env(REKASIM_FIRMWARE_DIR=_firmware_dir("9.9.9", good_hash=False))
def test_a_release_with_a_wrong_hash_is_never_offered(sim):
    time.sleep(1)
    assert not sim.logged("is available")
