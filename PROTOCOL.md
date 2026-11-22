# Rekadeck device protocol

How a Rekadeck talks to its server. The hosted service at https://rekalyptus.com and the mock server in `server/`
both speak it, and the device side is `firmware/lib/rekacore/src/api.cpp`. Any server with these endpoints can run
a Rekadeck.

All bodies are JSON unless noted. Every call except linking carries `Authorization: Bearer <token>`. Any 401 makes
the device forget its token and go back to the link screen.

HTTPS from firmware 0.3.0 (`https://` server addresses; `http://` still works for a server on your own network).
The device trusts the root certificates bundled in `firmware/src/ca_roots.h`. It does not check certificate dates:
it has no clock until its first check-in, and that check-in is itself over HTTPS. Requests to the same server reuse
one connection (HTTP/1.1 keep-alive), so a batch download pays for one TLS handshake, not one per file.

## Linking (OAuth device-code style, like a smart TV)

**`POST /api/device/code`**

```json
{"hw": "rekadeck-1", "fw": "0.1.0", "screen": {"w": 800, "h": 480, "bpp": 1}}
```

```json
{"device_code": "long secret", "user_code": "M8D-87B", "verify_url": "http://host:8000/link",
 "interval_s": 3, "expires_s": 600}
```

The device shows `user_code`, the URL and a QR code for `verify_url?code=<user_code>`. `screen` tells the backend
what to render for this device, which is how other panels (OLED, LCD, a bigger e-paper) can work later.

**`POST /api/device/token`** every `interval_s`:

```json
{"device_code": "long secret"}
```

Returns `{"status": "pending"}`, `{"status": "expired"}`, or `{"status": "ok", "token": "...", "device_id": "dev_..."}`.

**`POST /api/device/unlink`** (empty body): the backend deletes the device and its token stops working. The
device calls this from Menu, then Unlink, after uploading whatever reviews it still holds.

## Check-in

**`POST /api/device/hello`** on every wake with Wi-Fi, after every `checkin_every` reviews, on the wake timer, when
the queue is down to its last two cards (from firmware 0.3.1, unless a check-in got through in the last 30 s), and
when the queue runs out. From 0.3.4 every check-in the keys have to wait for runs on a full "Syncing" screen, and the
ones made while reviewing run between cards (after a grade, before the next card) rather than behind the card on
screen; Wi-Fi joins in the background beforehand. Key presses made during the sync are dropped.

```json
{"fw": "0.2.0", "battery": 82.0, "charging": false, "rtc_ms": 3781, "boot": 1678772037,
 "batch_id": "43e25531", "batch_version": 1, "cards_left": 17, "unsynced": 0,
 "mv": 3912, "usb": false, "awake_s": 2046, "wakes": 41, "timer_wakes": 7, "fail_streak": 0}
```

From firmware 0.2.0 the device also reports its power budget since power-on, so a battery that drains too fast
can be traced from the server side: `mv` is the battery voltage, `awake_s` the total time spent awake, `wakes` and
`timer_wakes` how often it woke, and `fail_streak` how many check-ins before this one couldn't reach the server.
Comparing check-ins gives drain per hour asleep and per hour awake.

```json
{"server_ms": 1794035038314, "version": 2, "stale": true, "due_now": 13, "due_today": 20, "due_device": 11,
 "settings": {"idle_s": 120, "again_gap": 5, "clean_after_pct": 15, "checkin_every": 10,
              "wake_every_min": 240, "batch_limit": 60, "utc_offset_min": -360},
 "firmware": {"version": "0.3.1", "url": "/api/device/firmware/0.3.1", "size": 1180416,
              "sha256": "9f2c...64 hex characters"}}
```

`due_now` is the number the apps show. `due_device` is how many of those this device can show: the same scope and
daily limits, text cards only (picture cards such as image occlusion need the app). It is never more than
`due_now`. The sleep screen shows `due_device` as the count and the difference as "Plus N picture cards in the app".
A server without `due_device` is treated as if it sent `due_device = due_now`.

`firmware` is present only when the server has a newer release for this device (see Firmware updates); otherwise it
is absent or null.

`version` is the account's content version. It goes up whenever anything that changes what's due happens
elsewhere: a review on the phone, a new card, an edit. `stale` is true when the device's batch is from an older
version, or the device has run out of cards while some are due, or the last batch was cut short (the server
draws frames for about 10 seconds per batch and leaves the rest for the next one). A stale device downloads a new
batch in the background and swaps it in between cards.

Settings, all owned by the server so they can change without a firmware update:

| Key | Meaning |
|---|---|
| idle_s | seconds without a key press before it sleeps |
| again_gap | cards shown before an Again card comes back |
| clean_after_pct | once partial refreshes have erased this much ink (percent of the panel's area), the next page turn uses a fast refresh to clear the ghosts |
| checkin_every | check in after this many reviews while awake |
| wake_every_min | the deep-sleep timer for quiet check-ins. While check-ins keep failing the device stretches it to two, then three times this |
| batch_limit | most cards per batch |
| utc_offset_min | the account's offset from UTC right now, in minutes, for the date on the screen. Sent on every check-in, so daylight saving changes reach the device |

The device clamps each one to a safe range before using it, so a bad value can't drain the battery or fill the flash:
idle_s 20 to 900, again_gap 1 to 50, clean_after_pct 5 to 60, checkin_every 3 to 100, wake_every_min 30 to 1440,
batch_limit 10 to 100, utc_offset_min -840 to 840. It also adds up to 10% of random delay to the wake timer and up to
30 minutes to the just-after-midnight wake, so devices in one time zone don't all check in at 12:01.

## Firmware updates

When `hello` carries `firmware`, the device installs it the next time it goes to sleep, if it is on USB or has at
least 40% battery (and 3.7 V). From 0.3.2, a board with no battery reading at all (the gauge runs off the cell, so a
missing or dead cell silences it) counts as on USB while a computer is on its port, and check-ins leave out
`battery`, `charging` and `mv` when there's no reading rather than send placeholders. Also from 0.3.2, Menu > Info
and updates has Check (a check-in), Install (now, without the 24-hour wait after a failure, stopped only by a battery
known to be low) and Auto-update (off: offers wait for Install; kept in the device's `autoupd` setting). The install
goes like this:

1. `GET /api/device/firmware/<version>` (with the device's bearer token) returns the application image
   (`application/octet-stream`, with `Content-Length`). The device streams it into its spare app slot, checks
   `size` and `sha256` against what `hello` promised, and only then marks the slot to boot.
2. It restarts into the new firmware, which keeps the review log and settings and checks in as a freshly powered-on
   device (new `boot` id).
3. The new firmware confirms itself the first time it reaches sleep or completes a check-in. One that crashes before
   that is rolled back by the bootloader on the next reset, to the version that installed it.

A failed install (download cut short, wrong hash) is not retried for the same version within 24 hours. The server
decides who is offered what: per-device `settings.auto_update = 0` turns offers off, and releases come from a
`manifest.json` in the server's firmware folder (for the mock server, the folder in `REKASIM_FIRMWARE_DIR`):

```json
{"releases": [{"version": "0.3.1", "file": "rekadeck-0.3.1.bin", "size": 1180416, "sha256": "9f2c...",
               "channel": "stable"}]}
```

The device is offered the highest `stable` release whose version is newer than its own `fw` (dotted numbers compared
numerically). `tools/release.py` builds the image and writes this entry.

## Clocks

The device has no real-time clock, only a counter since power-on (`rtc_ms`) that keeps running through deep sleep
and drifts a few percent. `boot` is a random number picked at each power-on, so times from different power-ons
are never mixed up.

Each `hello` and `reviews` call gives the server a pair: the device's `rtc_ms` and the server's own time when the
request arrived. The server turns device times into real times by interpolating between the two nearest pairs from
the same boot, which also cancels steady drift. With a single pair it applies the offset. A review from a boot the
server never heard from gets the time it arrived.

The device uses the same idea in reverse to know what's due: server time now is roughly
`anchor_server + (rtc_ms - anchor_rtc)` from its last check-in.

## Cards

**`GET /api/device/batch?limit=60`**: cards due within the next 24 hours, earliest first.

```json
{"batch_id": "987a94fa", "version": 2, "issued_ms": 1794035038314,
 "assets": [{"name": "question", "crc": 123, "size": 412}, {"name": "choice", "crc": 456, "size": 380}],
 "cards": [
   {"id": "c01", "rev": 3, "kind": "basic", "crc": 2271245102, "size": 3310, "due_ms": 1794035000000,
    "hide": [[214, 432]], "qstrip": "question", "choices": null},
   {"id": "m01", "rev": 7, "kind": "choice", "crc": 99812345, "size": 2967, "due_ms": 1794035100000,
    "hide": [[150, 292]], "qstrip": "choice",
    "choices": {"boxes": [[8, 296, 184, 120], [208, 296, 184, 120], [408, 296, 184, 120], [608, 296, 184, 120]],
                "correct": 0}}
 ]}
```

**`GET /api/device/frame/<id>?rev=<rev>`** and **`GET /api/device/asset/<name>`** return zlib-compressed bytes
(`application/octet-stream`).

A frame is the card's answer screen at full size: 800 x 480, 1 bit per pixel, rows of 100 bytes, most significant
bit first, 1 = black. `crc` is the CRC-32 of the uncompressed frame, and the device checks it after inflating.

The device builds the question screen itself from the same frame: it whites out the row bands in `hide` and pastes
the `qstrip` asset (an 800 x 48 key-label strip) over rows 432 to 480. So one download covers both sides of a card.
For a choice card, `boxes` (x, y, w, h) are the four answer boxes, one above each key. After a pick the device
inverts the correct box and draws a thick frame around the one picked.

Downloads go into a second slot on flash, one file per loop pass while no key is pressed. Frames whose CRC matches
one already on board are copied instead of downloaded. When everything checks out, the new manifest is written and
the active slot flips, so a half-finished download never touches the cards in use.

## Reviews

**`POST /api/device/reviews`**, in chunks of 50:

```json
{"boot": 1678772037, "rtc_ms": 412004,
 "reviews": [{"rid": 41, "card": "c01", "g": 3, "shown": 398100, "revealed": 401230, "graded": 403002,
              "choice": -1, "boot": 1678772037}]}
```

```json
{"accepted": [41], "server_ms": 1794035038314, "version": 3}
```

- `g`: 1 Again, 2 Hard, 3 Good, 4 Easy, 0 skipped (logged but not scheduled)
- `choice`: the answer box picked on a choice card (0 to 3), or -1
- `rid`: the device's own review counter. The device keeps every review in an append-only log on flash and only
  deletes the ones listed in `accepted`, so the backend has to treat a repeated `(device, rid)` as already stored
  and still list it in `accepted`.
