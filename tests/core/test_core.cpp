// Unit tests for the device core (firmware/lib/rekacore), run against FakeHal. No framework: each TEST registers
// itself, CHECK records a failure and carries on. Build and run: cmake -S tests/core -B build/core && ctest.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <algorithm>

#include "api.h"
#include "device.h"
#include "fake_hal.h"
#include "keys.h"
#include "store.h"
#include "vendor/ArduinoJson.h"

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    g_checks++;                                                                     \
    if (!(cond)) {                                                                  \
      g_failures++;                                                                 \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
    }                                                                               \
  } while (0)

struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
struct Reg { Reg(const char* n, void (*f)()) { registry().push_back({n, f}); } };
#define TEST(name) static void name(); static Reg reg_##name(#name, name); static void name()

using reka::HttpResult;

static const uint64_t MIN = 60000, HOUR = 60 * MIN, DAY = 24 * HOUR;
static const std::string SHA(64, 'a');

// ------------------------------------------------------------------ helpers
static void linked(FakeHal& h) {
  h.settings["ssid"] = "Home";
  h.settings["pass"] = "secret";
  h.settings["server"] = "https://deck.example";
  h.settings["token"] = "tok";
  h.pwr.gauge = true;
  h.pwr.pct = 80;
  h.pwr.mv = 3950;
  h.pwr.usb = false;
}

struct HelloOpts {
  uint64_t server_ms = 400 * DAY + 30 * MIN;   // 00:30 UTC: the next midnight is far off, so it doesn't cap timers
  int due_now = 13;
  int due_device = -1;                          // -1: leave it out, like an older server
  std::string firmware;                         // version on offer, "" for none
  std::string settings = "{\"wake_every_min\":240,\"utc_offset_min\":0}";
  bool stale = false;
};

static std::string helloJson(const HelloOpts& o) {
  std::string s = "{\"server_ms\":" + std::to_string(o.server_ms) + ",\"version\":1,\"stale\":" +
                  (o.stale ? "true" : "false") + ",\"due_now\":" + std::to_string(o.due_now) + ",\"due_today\":20";
  if (o.due_device >= 0) s += ",\"due_device\":" + std::to_string(o.due_device);
  if (!o.firmware.empty())
    s += ",\"firmware\":{\"version\":\"" + o.firmware + "\",\"url\":\"/api/device/firmware/" + o.firmware +
         "\",\"size\":1200000,\"sha256\":\"" + SHA + "\"}";
  return s + ",\"settings\":" + o.settings + "}";
}

static std::function<HttpResult(const std::string&, const std::string&, const std::string&)> serverWith(HelloOpts o) {
  return [o](const std::string&, const std::string& url, const std::string&) {
    HttpResult r;
    if (url.find("/hello") != std::string::npos) { r.status = 200; r.body = helloJson(o); }
    else if (url.find("/unlink") != std::string::npos) { r.status = 200; r.body = "{}"; }
    else if (url.find("/reviews") != std::string::npos) { r.status = 200; r.body = "{\"accepted\":[]}"; }
    else { r.status = 404; }
    return r;
  };
}

// A timer wake: setup() checks in and goes back to sleep (or restarts into new firmware) without any loop() call
static void timerWake(FakeHal& h) {
  h.timer_wake = true;
  h.slept = h.restarted = false;
  reka::Device d(h);
  d.setup();
}

static bool between(int64_t v, uint64_t lo, uint64_t hi) { return v >= (int64_t)lo && v <= (int64_t)hi; }

// ------------------------------------------------------------------ keys
static reka::Ev feed(reka::Keys& k, uint32_t& t, uint8_t raw, uint32_t ms) {
  reka::Ev last = reka::Ev::None;
  for (uint32_t i = 0; i < ms; i += 10) {
    k.poll(t += 10, raw);
    for (reka::Ev e = k.next(); e != reka::Ev::None; e = k.next()) last = e;
  }
  return last;
}

TEST(keys_tap_hold_and_chord) {
  reka::Keys k;
  uint32_t t = 1000;
  CHECK(feed(k, t, 0x04, 60) == reka::Ev::K3);            // keys 2 and 3 act as soon as they go down
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
  CHECK(feed(k, t, 0x01, 60) == reka::Ev::None);          // key 1 waits for the release...
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::K1);            // ...so a hold can still become undo
  CHECK(feed(k, t, 0x01, 1100) == reka::Ev::Hold1);
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);          // the release after a hold is not also a tap
  CHECK(feed(k, t, 0x09, 2200) == reka::Ev::Menu);        // 1 and 4 together
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
  CHECK(feed(k, t, 0x02, 10) == reka::Ev::None);          // a 10 ms blip is bounce, not a press
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
}

TEST(keys_a_second_tap_during_a_refresh_still_counts) {
  // Key 3 reveals and is still down when the refresh starts; it's let go and tapped again before the device looks.
  // Polls only see "held" throughout, but the board counted the second press.
  reka::Keys k;
  uint32_t t = 0;
  CHECK(feed(k, t, 0x04, 60) == reka::Ev::K3);
  k.poll(t += 750, 0x04, 0x04);   // after the refresh: still reads held, one new press
  CHECK(k.next() == reka::Ev::K3);
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);

  // key 1 fires on release: the unseen release ends the first tap, the second ends with the real release
  CHECK(feed(k, t, 0x01, 60) == reka::Ev::None);
  k.poll(t += 750, 0x01, 0x01);
  CHECK(k.next() == reka::Ev::K1);
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::K1);

  // a first press noticed late (the key was up before) is just a press, not two
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
  k.poll(t += 10, 0x04, 0x04);
  CHECK(feed(k, t, 0x04, 40) == reka::Ev::K3);
  CHECK(k.next() == reka::Ev::None);
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
}

TEST(keys_wake_key_is_ignored_until_released) {
  reka::Keys k;
  k.ignoreUntilReleased();
  uint32_t t = 0;
  CHECK(feed(k, t, 0x04, 200) == reka::Ev::None);
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);
  CHECK(feed(k, t, 0x04, 60) == reka::Ev::K3);
}

TEST(keys_menu_chord_held_through_the_wake_opens_the_menu) {
  reka::Keys k;
  k.ignoreUntilReleased();
  uint32_t t = 5000;
  CHECK(feed(k, t, 0x09, 100) == reka::Ev::Menu);   // held since before the boot: no second 2 s wait
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::None);    // and letting go isn't also K1 or K4
  CHECK(feed(k, t, 0x01, 60) == reka::Ev::None);    // the keys work normally after it
  CHECK(feed(k, t, 0x00, 60) == reka::Ev::K1);

  reka::Keys j;                                      // key 1 woke it, key 4 joined later: an ordinary 2 s hold
  j.ignoreUntilReleased();
  t = 5000;
  CHECK(feed(j, t, 0x01, 300) == reka::Ev::None);
  CHECK(feed(j, t, 0x09, 1500) == reka::Ev::None);
  CHECK(feed(j, t, 0x09, 600) == reka::Ev::Menu);
  CHECK(feed(j, t, 0x00, 60) == reka::Ev::None);

  reka::Keys i;                                      // the chord let go early fires nothing
  i.ignoreUntilReleased();
  t = 5000;
  CHECK(feed(i, t, 0x01, 300) == reka::Ev::None);
  CHECK(feed(i, t, 0x09, 500) == reka::Ev::None);
  CHECK(feed(i, t, 0x00, 60) == reka::Ev::None);
}

// ------------------------------------------------------------------ settings
TEST(settings_are_clamped_to_safe_ranges) {
  reka::Settings s;
  s.parse("{\"wake_every_min\":1,\"batch_limit\":300,\"idle_s\":5,\"utc_offset_min\":-2000,\"again_gap\":0}");
  CHECK(s.wake_every_min == 30);
  CHECK(s.batch_limit == 100);
  CHECK(s.idle_s == 20);
  CHECK(s.utc_offset_min == -840);
  CHECK(s.again_gap == 1);
  s.parse("{\"wake_every_min\":\"often\",\"checkin_every\":7}");   // not a number: the old value stays
  CHECK(s.wake_every_min == 30);
  CHECK(s.checkin_every == 7);
  s.parse("not json");
  CHECK(s.checkin_every == 7);
}

// ------------------------------------------------------------------ power
TEST(empty_battery_at_boot_sleeps_on_keys_without_touching_the_radio) {
  FakeHal h;
  linked(h);
  h.pwr.pct = 2;
  h.server = serverWith({});
  timerWake(h);
  CHECK(h.slept && h.sleep_timer == 0);
  CHECK(h.calls.empty() && h.wifi_joins == 0);
  CHECK(h.logged("charge me"));
}

TEST(a_brownout_boot_does_not_try_wifi_again) {
  FakeHal h;
  linked(h);
  h.browned = true;
  h.server = serverWith({});
  timerWake(h);
  CHECK(h.slept && h.sleep_timer == 0);
  CHECK(h.wifi_joins == 0);
}

TEST(on_usb_a_brownout_or_low_gauge_does_not_stop_it) {
  FakeHal h;
  linked(h);
  h.pwr.pct = 1;
  h.pwr.usb = true;
  h.server = serverWith({});
  timerWake(h);
  CHECK(h.count("/hello") == 1);
}

TEST(a_weak_battery_skips_wifi_but_is_not_a_failed_sync) {
  FakeHal h;
  linked(h);
  h.pwr.mv = 3500;   // above the cutoff, below what Wi-Fi bursts need
  h.server = serverWith({});
  timerWake(h);
  CHECK(h.wifi_joins == 0 && h.calls.empty());
  CHECK(h.state.fail_streak == 0);
  CHECK(h.slept && h.sleep_timer > 0);
  CHECK(h.logged("no wi-fi until it's charged"));
}

// ------------------------------------------------------------------ check-ins
TEST(an_unreachable_server_is_tried_once_per_wake_and_backs_off) {
  FakeHal h;
  linked(h);
  h.server = [](const std::string&, const std::string&, const std::string&) { return HttpResult{}; };   // no answer
  timerWake(h);
  CHECK(h.count("/hello") == 1);   // not a second try on the way to sleep
  CHECK(h.state.fail_streak == 1);
  CHECK(between(h.sleep_timer, 8 * HOUR, 8 * HOUR + 48 * MIN));   // 4 h doubled, plus up to 10% jitter
  timerWake(h);
  CHECK(h.state.fail_streak == 2);
  CHECK(between(h.sleep_timer, 12 * HOUR, 12 * HOUR + 72 * MIN));
  h.server = serverWith({});
  timerWake(h);
  CHECK(h.state.fail_streak == 0);
  CHECK(between(h.sleep_timer, 4 * HOUR, 4 * HOUR + 24 * MIN));
}

TEST(the_midnight_wake_is_spread_over_half_an_hour) {
  HelloOpts o;
  o.server_ms = 400 * DAY + 23 * HOUR;   // 11 PM: midnight is an hour away
  std::vector<int64_t> timers;
  for (uint32_t seed = 1; seed < 40; seed++) {
    FakeHal h;
    linked(h);
    h.rng = seed * 7919;
    h.server = serverWith(o);
    timerWake(h);
    timers.push_back(h.sleep_timer);
  }
  int64_t lo = timers[0], hi = timers[0];
  for (auto t : timers) { lo = std::min(lo, t); hi = std::max(hi, t); }
  CHECK(lo >= (int64_t)(HOUR + MIN));
  CHECK(hi <= (int64_t)(HOUR + 31 * MIN));
  CHECK(hi - lo > 10 * MIN);   // actually spread, not all at 12:01
}

TEST(due_device_splits_the_count_and_old_servers_still_work) {
  FakeHal h;
  linked(h);
  HelloOpts o;
  o.due_now = 13;
  o.due_device = 11;
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.state.due_now == 11);
  CHECK(h.state.due_app == 2);

  FakeHal old;
  linked(old);
  HelloOpts p;
  p.due_now = 13;   // no due_device at all
  old.server = serverWith(p);
  timerWake(old);
  CHECK(old.state.due_now == 13);
  CHECK(old.state.due_app == 0);

  FakeHal bad;
  linked(bad);
  HelloOpts q;
  q.due_now = 5;
  q.due_device = 9;   // can't be more than due_now
  bad.server = serverWith(q);
  timerWake(bad);
  CHECK(bad.state.due_now == 5 && bad.state.due_app == 0);
}

TEST(server_settings_are_clamped_before_they_set_the_timer) {
  FakeHal h;
  linked(h);
  HelloOpts o;
  o.settings = "{\"wake_every_min\":1,\"utc_offset_min\":0}";
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.sleep_timer >= (int64_t)(30 * MIN));
}

// ------------------------------------------------------------------ firmware updates
TEST(an_offered_update_installs_at_sleep_with_enough_charge) {
  FakeHal h;
  linked(h);
  HelloOpts o;
  o.firmware = "9.9.9";
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.ota_calls.size() == 1);
  if (!h.ota_calls.empty()) {
    CHECK(h.ota_calls[0].url == "https://deck.example/api/device/firmware/9.9.9");
    CHECK(h.ota_calls[0].bearer == "tok");
    CHECK(h.ota_calls[0].size == 1200000);
    CHECK(h.ota_calls[0].sha == SHA);
  }
  CHECK(h.restarted && !h.slept);
}

TEST(an_update_waits_for_charge_or_usb) {
  FakeHal h;
  linked(h);
  h.pwr.pct = 30;
  HelloOpts o;
  o.firmware = "9.9.9";
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.ota_calls.empty());
  CHECK(h.slept && !h.restarted);
  CHECK(h.logged("waits for more charge"));

  h.pwr.usb = true;   // on USB the same battery is fine
  h.calls.clear();
  timerWake(h);
  CHECK(h.ota_calls.size() == 1);
}

TEST(the_running_version_or_a_malformed_offer_is_not_installed) {
  FakeHal h;
  linked(h);
  HelloOpts o;
  o.firmware = reka::FW_VERSION;
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.ota_calls.empty());

  FakeHal m;
  linked(m);
  m.server = [](const std::string&, const std::string& url, const std::string&) {
    HttpResult r;
    r.status = 200;
    if (url.find("/hello") != std::string::npos)   // a short hash and an absolute URL: both refused
      r.body = "{\"server_ms\":34560000000,\"due_now\":1,\"firmware\":{\"version\":\"9.9.9\",\"url\":\"http://evil/x\","
               "\"size\":10,\"sha256\":\"abc\"},\"settings\":{}}";
    return r;
  };
  timerWake(m);
  CHECK(m.ota_calls.empty());
}

TEST(a_failed_update_is_not_retried_for_a_day) {
  FakeHal h;
  linked(h);
  h.ota_ok = false;
  HelloOpts o;
  o.firmware = "9.9.9";
  h.server = serverWith(o);
  timerWake(h);
  CHECK(h.ota_calls.size() == 1);
  CHECK(h.slept && !h.restarted);   // the old firmware carries on
  CHECK(h.logged("firmware update failed"));
  h.rtc_ms += 4 * HOUR;
  timerWake(h);
  CHECK(h.ota_calls.size() == 1);   // not again yet
  h.rtc_ms += 21 * HOUR;
  h.ota_ok = true;
  timerWake(h);
  CHECK(h.ota_calls.size() == 2);
  CHECK(h.restarted);
}

// ------------------------------------------------------------------ downloads
TEST(a_batch_bigger_than_the_flash_is_trimmed_to_fit) {
  FakeHal h;
  linked(h);
  h.fs_bytes = 300 * 1024;
  HelloOpts o;
  o.stale = true;
  std::string manifest = "{\"batch_id\":\"b1\",\"version\":1,\"assets\":[],\"cards\":[";
  for (int i = 0; i < 100; i++)
    manifest += std::string(i ? "," : "") + "{\"id\":\"c" + std::to_string(i) +
                "\",\"rev\":1,\"kind\":\"basic\",\"crc\":1,\"size\":8000,\"due_ms\":0,\"hide\":[],\"qstrip\":\"\"}";
  manifest += "]}";
  auto hello = serverWith(o);
  h.server = [&](const std::string& m, const std::string& url, const std::string& b) {
    if (url.find("/batch") != std::string::npos) return HttpResult{200, manifest};
    return hello(m, url, b);
  };
  timerWake(h);
  // 300 KB, less the 160 KB reserve and 8 KB for the manifest and done list, at one 8 KB block pair per card
  CHECK(h.logged("flash holds 16 of 100 cards"));
  CHECK(h.count("/frame/") <= 16);
}

// ------------------------------------------------------------------ running out of cards
// A server for whole review sessions: batches of basic cards with real frames, every review accepted, and the
// hello's answers set by the test. A batch hands the device the latest cards, so the hello after it isn't stale.
struct DeckServer {
  HelloOpts hello;
  bool stale = true;                               // the next hello's answer: there are new cards to fetch
  std::vector<std::vector<std::string>> batches;   // card ids, one batch per /batch call, the last one repeating
  size_t issued = 0;
  bool frames_fail = false;
  uint32_t frame_ms = 0;                           // how long each frame takes to come down
  int hellos = 0;
  std::string frame_z;
  uint32_t frame_crc = 0;

  DeckServer() {   // one frame for every card: a zlib stream of a single stored block (the core only inflates)
    std::vector<uint8_t> fb(reka::FB_BYTES, 0);
    fb[1000] = 0xFF;
    frame_crc = 0xFFFFFFFFu;   // CRC-32, bit by bit (gfx.cpp's crc32 is renamed by a miniz macro, can't link to it)
    for (uint8_t c : fb) {
      frame_crc ^= c;
      for (int k = 0; k < 8; k++) frame_crc = (frame_crc & 1) ? 0xEDB88320u ^ (frame_crc >> 1) : frame_crc >> 1;
    }
    frame_crc ^= 0xFFFFFFFFu;
    uint32_t a = 1, b = 0;
    for (uint8_t c : fb) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint16_t len = (uint16_t)fb.size(), nlen = (uint16_t)~len;
    frame_z = {'\x78', '\x01', '\x01', (char)(len & 0xFF), (char)(len >> 8), (char)(nlen & 0xFF), (char)(nlen >> 8)};
    frame_z.append((const char*)fb.data(), fb.size());
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) frame_z += (char)((adler >> s) & 0xFF);
  }

  HttpResult answer(const std::string& url, const std::string& body) {
    if (url.find("/hello") != std::string::npos) {
      hellos++;
      HelloOpts o = hello;
      o.stale = stale;
      return {200, helloJson(o)};
    }
    if (url.find("/reviews") != std::string::npos) {
      JsonDocument req, resp;
      deserializeJson(req, body);
      JsonArray acc = resp["accepted"].to<JsonArray>();
      for (JsonObject r : req["reviews"].as<JsonArray>()) acc.add(r["rid"].as<uint32_t>());
      std::string out;
      serializeJson(resp, out);
      return {200, out};
    }
    if (url.find("/batch") != std::string::npos) {
      auto& ids = batches[std::min(issued, batches.size() - 1)];
      std::string m = "{\"batch_id\":\"b" + std::to_string(++issued) + "\",\"version\":1,\"assets\":[],\"cards\":[";
      for (size_t i = 0; i < ids.size(); i++)
        m += std::string(i ? "," : "") + "{\"id\":\"" + ids[i] + "\",\"rev\":1,\"kind\":\"basic\",\"crc\":" +
             std::to_string(frame_crc) + ",\"size\":" + std::to_string(frame_z.size()) +
             ",\"due_ms\":0,\"hide\":[],\"qstrip\":\"\"}";
      stale = false;
      return {200, m + "]}"};
    }
    if (url.find("/frame/") != std::string::npos) return frames_fail ? HttpResult{500, ""} : HttpResult{200, frame_z};
    return {404, ""};
  }
};

struct Session {
  FakeHal h;
  DeckServer srv;
  reka::Device d{h};
  explicit Session(std::vector<std::vector<std::string>> batches) {
    linked(h);
    srv.batches = std::move(batches);
    h.server = [this](const std::string&, const std::string& url, const std::string& body) {
      if (url.find("/frame/") != std::string::npos) {
        h.now_ms += srv.frame_ms;
        h.rtc_ms += srv.frame_ms;
      }
      return srv.answer(url, body);
    };
  }
  // A key wake with nothing on flash: the first batch comes down, then its first card. Then a moment to look at it:
  // a key already down when a sync ends is ignored until it's let go, like the key that woke it.
  void start() {
    d.setup();
    run(100);
  }
  void run(uint32_t ms) {       // loop() passes 10 ms apart
    for (uint32_t t = 0; t < ms; t += 10) {
      h.now_ms += 10;
      h.rtc_ms += 10;
      d.loop();
    }
  }
  void tap(uint8_t key) {
    h.held = key;
    run(60);
    h.held = 0;
    run(60);
  }
  void good() { tap(0x04); tap(0x04); }   // key 3 shows the answer, key 3 again grades it Good
  size_t mark() const { return h.screens.size(); }
  std::vector<std::string> since(size_t m) const { return {h.screens.begin() + m, h.screens.end()}; }
};

static bool has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }
static size_t at(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) - v.begin(); }

TEST(running_out_while_more_cards_download_never_says_caught_up) {
  Session s({{"a1", "a2", "a3"}, {"b1", "b2", "b3"}});
  s.start();
  CHECK(s.h.screens.back() == "first card");
  s.good();
  s.good();                    // straight after the wake's check-in: too soon to ask again
  CHECK(s.srv.hellos == 1);
  s.srv.stale = true;          // new cards came up meanwhile
  s.srv.frame_ms = 6000;       // and take longer to download than a sync used to wait: 18 s for the three
  size_t m = s.mark();
  s.good();                    // the last card
  auto seen = s.since(m);
  if (std::getenv("SHOW_SCREENS"))
    for (auto& w : seen) std::printf("    screen: %s\n", w.c_str());
  CHECK(has(seen, "looking for more cards"));                  // off the answered card at once
  CHECK(has(seen, "getting more cards"));
  CHECK(at(seen, "looking for more cards") < at(seen, "getting more cards"));
  s.run(1000);
  seen = s.since(m);
  if (std::getenv("SHOW_SCREENS"))
    for (auto& w : seen) std::printf("    screen after: %s\n", w.c_str());
  CHECK(!has(seen, "all done") && !has(seen, "none ready"));
  CHECK(seen.back() == "first card");                          // b1, as soon as the batch landed
  CHECK(s.srv.issued == 2);
}

TEST(running_out_with_nothing_new_says_caught_up) {
  Session s({{"a1", "a2"}});
  s.srv.hello.due_now = 0;
  s.srv.hello.due_device = 0;
  s.start();
  s.good();
  size_t m = s.mark();
  s.good();
  auto seen = s.since(m);
  CHECK(has(seen, "looking for more cards"));
  CHECK(!seen.empty() && seen.back() == "all done");
  CHECK(!has(seen, "getting more cards"));
  CHECK(s.srv.issued == 1);
}

TEST(cards_due_without_questions_are_not_called_caught_up) {
  Session s({{"a1"}});
  s.srv.hello.due_now = 5;
  s.srv.hello.due_device = 5;   // due here, none of them on the device: no question to show yet
  s.start();
  size_t m = s.mark();
  s.good();
  auto seen = s.since(m);
  CHECK(!seen.empty() && seen.back() == "none ready");
  CHECK(!has(seen, "all done"));
}

TEST(a_short_queue_fetches_more_before_it_runs_out) {
  Session s({{"a1", "a2", "a3"}, {"b1", "b2", "b3"}});
  s.start();
  s.run(31000);                 // reading the first card for a while
  s.srv.stale = true;
  s.good();                     // two left: check in behind the next card
  s.run(2000);
  CHECK(s.srv.hellos == 2);
  CHECK(s.srv.issued == 2);     // and the new batch came down in the background
  size_t m = s.mark();
  s.good();
  auto seen = s.since(m);
  CHECK(!has(seen, "looking for more cards") && !has(seen, "all done"));
  CHECK(!seen.empty() && seen.back() == "next card");
}

TEST(a_download_that_breaks_off_says_so) {
  Session s({{"a1"}, {"b1", "b2"}});
  s.start();
  s.srv.stale = true;
  s.srv.frames_fail = true;
  size_t m = s.mark();
  s.good();
  s.run(500);
  auto seen = s.since(m);
  CHECK(has(seen, "getting more cards"));
  CHECK(!seen.empty() && seen.back() == "download failed");
  CHECK(!has(seen, "all done"));
  s.srv.frames_fail = false;
  s.srv.stale = true;
  m = s.mark();
  s.tap(0x01);                  // Sync on the done screen tries again
  s.run(500);
  seen = s.since(m);
  CHECK(!seen.empty() && seen.back() == "first card");
}

TEST(unlinked_while_looking_for_more_cards_shows_the_link_screen) {
  Session s({{"a1"}});
  s.start();
  auto ok = s.h.server;
  s.h.server = [&](const std::string& m, const std::string& url, const std::string& b) {
    if (url.find("/reviews") != std::string::npos) return HttpResult{401, ""};
    return ok(m, url, b);
  };
  size_t n = s.mark();
  s.good();
  auto seen = s.since(n);
  CHECK(!has(seen, "all done") && !has(seen, "none ready"));
  CHECK(s.h.settings["token"].empty());
}

// ------------------------------------------------------------------ the card count
// With DUMP_SCREENS=<dir>, the last picture goes to <dir>/<name>.pbm (1 bit, MSB first, 1 = black: the framebuffer's
// own layout), to look at a layout change without a board.
static void dumpScreen(const FakeHal& h, const char* name) {
  const char* dir = std::getenv("DUMP_SCREENS");
  if (!dir || h.last_fb.empty()) return;
  if (FILE* f = std::fopen((std::string(dir) + "/" + name + ".pbm").c_str(), "wb")) {
    std::fprintf(f, "P4\n%d %d\n", reka::SCREEN_W, reka::SCREEN_H);
    std::fwrite(h.last_fb.data(), 1, h.last_fb.size(), f);
    std::fclose(f);
  }
}

// Ink in a box of the last picture, to tell a drawn count from an empty spot
static int inkIn(const FakeHal& h, int x0, int y0, int x1, int y1) {
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) n += (h.last_fb[y * reka::ROW_BYTES + (x >> 3)] >> (7 - (x & 7))) & 1;
  return n;
}

TEST(the_top_row_shows_the_cards_ready_on_the_device) {
  // "N ready" after the date, on every screen with the status row (the date here ends before x 195)
  Session s({{"a1", "a2", "a3"}});
  s.srv.hello.due_now = 67;
  s.srv.hello.due_device = 67;    // the account's count; only three of them are on the device
  s.start();
  dumpScreen(s.h, "review_3_ready");
  int three = inkIn(s.h, 195, 0, 540, 40);
  CHECK(three > 0);
  s.good();
  dumpScreen(s.h, "review_2_ready");
  int two = inkIn(s.h, 195, 0, 540, 40);
  CHECK(two > 0 && two != three);
  s.good();
  s.good();                        // the last one: the done screen
  dumpScreen(s.h, "done_0_ready");
  CHECK(inkIn(s.h, 195, 0, 540, 40) > 0);
  s.run(125000);                   // left alone: the sleep screen, under the account's big count
  CHECK(s.h.screens.back().rfind("sleep screen", 0) == 0);
  dumpScreen(s.h, "sleep_0_ready");
  CHECK(inkIn(s.h, 195, 0, 540, 40) > 0);
}

TEST(a_check_in_while_reviewing_gets_its_own_screen_between_cards) {
  std::vector<std::string> ids;
  for (int i = 0; i < 14; i++) ids.push_back("c" + std::to_string(i));
  Session s({ids});
  s.start();
  int hellos = s.srv.hellos;
  for (int i = 0; i < 9; i++) s.good();
  CHECK(s.srv.hellos == hellos);
  // Someone presses key 3 at the Syncing screen to see whether it's alive: that mustn't reveal the next card
  auto inner = s.h.server;
  std::vector<uint8_t> sync_screen;
  s.h.server = [&](const std::string& m, const std::string& url, const std::string& b) {
    if (url.find("/hello") != std::string::npos) {
      s.h.pressed |= 0x04;
      sync_screen = s.h.last_fb;
    }
    return inner(m, url, b);
  };
  size_t m = s.mark();
  s.good();                                   // the 10th review: the check-in, between cards
  s.run(300);
  auto seen = s.since(m);
  CHECK(s.srv.hellos == hellos + 1);
  CHECK(has(seen, "syncing") && at(seen, "syncing") < at(seen, "next card"));
  CHECK(!seen.empty() && seen.back() == "next card");   // the press at the Syncing screen was dropped
  s.h.last_fb = sync_screen;
  dumpScreen(s.h, "syncing");
}

// ------------------------------------------------------------------ updates from the info page
static void openInfo(Session& s) {
  s.h.held = 0x09;   // the menu chord
  s.run(2100);
  s.h.held = 0;
  s.run(100);
  s.tap(0x02);       // down to "Sync now"
  s.tap(0x02);       // down to "Info and updates"
  s.tap(0x04);       // select
}

TEST(with_auto_update_off_an_offer_waits_for_the_info_page) {
  Session s({{"a1", "a2"}});
  s.h.settings["autoupd"] = "0";
  s.srv.hello.firmware = "9.9.9";
  s.start();
  CHECK(s.h.logged("9.9.9 is available, auto-update is off"));
  s.run(125000);                          // left alone: goes to sleep
  CHECK(s.h.slept && s.h.ota_calls.empty());
}

TEST(the_info_page_checks_for_an_update_and_installs_it) {
  Session s({{"a1", "a2"}});
  s.start();
  openInfo(s);
  CHECK(s.h.screens.back() == "device info");
  int hellos = s.srv.hellos;
  s.srv.hello.firmware = "9.9.9";         // released since the wake's check-in
  s.tap(0x01);                            // Check
  CHECK(s.srv.hellos == hellos + 1);
  CHECK(s.h.ota_calls.empty());           // a check only looks
  s.tap(0x02);                            // auto-update off, remembered
  CHECK(s.h.settings["autoupd"] == "0");
  s.tap(0x01);                            // Install
  CHECK(s.h.ota_calls.size() == 1 && s.h.restarted);
}

TEST(a_failed_install_from_the_info_page_can_be_retried_at_once) {
  Session s({{"a1"}});
  s.h.ota_ok = false;
  s.srv.hello.firmware = "9.9.9";
  s.start();
  openInfo(s);
  s.tap(0x01);                            // Install: fails
  CHECK(s.h.ota_calls.size() == 1 && !s.h.restarted);
  CHECK(s.h.screens.back() == "device info");
  s.h.ota_ok = true;
  s.tap(0x01);                            // Check finds it again...
  s.tap(0x01);                            // ...and Install doesn't wait a day
  CHECK(s.h.ota_calls.size() == 2 && s.h.restarted);
}

TEST(no_battery_reading_on_a_computer_still_updates_at_sleep) {
  // The gauge runs off the cell: a dead or missing one silences it, and the board runs off USB
  Session s({{"a1"}});
  s.h.pwr.gauge = false;
  s.h.pwr.usb_host = true;
  s.srv.hello.firmware = "9.9.9";
  s.start();
  s.run(125000);
  CHECK(s.h.ota_calls.size() == 1 && s.h.restarted);
  CHECK(s.h.calls.size() && s.h.calls[0].body.find("\"battery\"") == std::string::npos);   // no made-up 100%

  Session w({{"a1"}});                    // on a wall charger: nothing says it's powered, so it waits
  w.h.pwr.gauge = false;
  w.srv.hello.firmware = "9.9.9";
  w.start();
  w.run(125000);
  CHECK(w.h.ota_calls.empty() && w.h.slept);
  CHECK(w.h.logged("waits for a battery reading or USB"));
}

// ------------------------------------------------------------------ review log
TEST(review_log_counts_and_removes) {
  FakeHal h;
  reka::ReviewLog log(h);
  for (int g = 1; g <= 3; g++) {
    reka::Review r;
    r.card = "c" + std::to_string(g);
    r.grade = g;
    log.append(r);
  }
  CHECK(log.count() == 3);
  CHECK(log.all().size() == 3);
  log.remove({2});
  CHECK(log.count() == 2);
  auto all = log.all();
  CHECK(all.size() == 2 && all[0].rid == 1 && all[1].rid == 3);
  log.clear();
  CHECK(log.count() == 0);
}

int main() {
  for (auto& t : registry()) {
    int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed, %zu tests\n", g_checks, g_failures, registry().size());
  return g_failures ? 1 : 0;
}
