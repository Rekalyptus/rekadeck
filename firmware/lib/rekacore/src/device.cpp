#include "device.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "vendor/ArduinoJson.h"

namespace reka {

static const char* MENU_ITEMS[] = {"Back to cards", "Sync now",           "Info and updates",  "Hardware check",
                                   "Wi-Fi setup",   "Unlink this device", "Erase this device", "Sleep now"};
enum { MI_BACK, MI_SYNC, MI_INFO, MI_CHECK, MI_WIFI, MI_UNLINK, MI_ERASE, MI_SLEEP };
static constexpr int MENU_N = 8, MENU_Y = 92, MENU_ROW = 41;
static constexpr uint32_t SETUP_TIMEOUT_MS = 10 * 60 * 1000, USB_CHECKIN_MS = 15 * 60 * 1000, WIFI_RETRY_MS = 5 * 60 * 1000;
// A queue down to its last REFILL_AT cards checks in for more, unless a check-in got through in the last REFILL_GAP_MS
static constexpr size_t REFILL_AT = 2;
static constexpr uint32_t REFILL_GAP_MS = 30 * 1000;
static constexpr uint64_t DEEP_CLEAN_MS = 20ULL * 3600 * 1000;   // Waveshare: give the panel a full refresh at least daily
static constexpr int STATUS_Y = 14, WIFI_X = 693, BATT_X = 719;   // the status line, above row 40 where card text starts
static const char* PANEL_PATH = "/panel.fb";

// Battery limits, from the cell's resting voltage. Wi-Fi draws bursts of 300 mA or more, and a weak cell sags
// enough under them to brown the board out, which reboots it, which tries Wi-Fi again: a loop that ends with a dead
// cell. Below RADIO_MIN_MV reviewing goes on but syncing waits for a charge; below CUTOFF the device stops.
static constexpr int RADIO_MIN_MV = 3550, CUTOFF_MV = 3400;
static constexpr float CUTOFF_PCT = 3.0f;
// Firmware updates install at sleep, never on a battery that might not see them through
static constexpr float OTA_MIN_PCT = 40.0f;
static constexpr int OTA_MIN_MV = 3700;
static constexpr uint64_t OTA_RETRY_MS = 24ULL * 3600 * 1000;

Device::Device(Hal& hal) : hal_(hal), store_(hal), log_(hal), api_(hal) {}

// ------------------------------------------------------------------ boot
void Device::setup() {
  RtcState& rtc = hal_.rtc();
  if (rtc.magic != RTC_MAGIC) {            // power-on: nothing survived
    rtc = RtcState{};
    rtc.magic = RTC_MAGIC;
    rtc.boot_id = hal_.random32() | 1;
    force_full_ = true;                   // the panel's state is unknown, so the first picture gets the slow clean
  } else {
    rtc.wakes++;
    if (hal_.wokeByTimer()) rtc.timer_wakes++;
  }
  frame_.assign(FB_BYTES, 0);
  scratch_.assign(FB_BYTES, 0);
  g_.clear();
  {   // what the panel still shows from before the sleep: the base for the first partial refresh
    std::string old;
    if (!force_full_ && hal_.readFile(PANEL_PATH, old) && old.size() == (size_t)FB_BYTES) {
      shown_.assign(old.begin(), old.end());
      shown_valid_ = true;
    }
  }
  hal_.getSetting("ssid", ssid_);
  hal_.getSetting("pass", pass_);
  if (!hal_.getSetting("server", server_) || server_.empty()) server_ = REKA_DEFAULT_SERVER;
  hal_.getSetting("token", token_);
  std::string cfg;
  if (hal_.getSetting("cfg", cfg)) cfg_.parse(cfg);
  std::string autoupd;
  if (hal_.getSetting("autoupd", autoupd)) auto_update_ = autoupd != "0";
  if (!hal_.getSetting("apid", ap_name_)) {
    char id[8];
    snprintf(id, sizeof id, "%04X", (unsigned)(hal_.random32() & 0xFFFF));
    ap_name_ = id;
    hal_.setSetting("apid", ap_name_);
  }
  ap_name_ = "Rekadeck-" + ap_name_.substr(0, 4);
  if (!hal_.getSetting("appass", ap_pass_) || ap_pass_.size() < 8) {
    // the setup network's WPA2 password, shown on the panel; no lookalike characters, it's typed off a screen
    static const char ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    ap_pass_.clear();
    for (int i = 0; i < 8; i++) ap_pass_ += ALPHABET[hal_.random32() % (sizeof ALPHABET - 1)];
    hal_.setSetting("appass", ap_pass_);
  }
  api_.configure(server_, token_);
  store_.load();
  loadAssets();
  if (hal_.wakeKeys()) keys_.ignoreUntilReleased();   // the key that woke us shouldn't also reveal a card

  PowerInfo p = hal_.power();
  if (!p.usb && hal_.brownedOut()) return chargeMe("brownout");
  if (!p.usb && p.gauge && (p.pct < CUTOFF_PCT || p.mv < CUTOFF_MV)) return chargeMe("battery empty");
  if (ssid_.empty()) return enterWifiSetup();
  if (token_.empty()) return enterLink();
  if (hal_.wokeByTimer()) {               // a quiet check-in: send reviews, refresh the due count, sleep again
    hal_.log("timer wake: checking in");
    goToSleep(syncBlocking(true, 30000));
    return;
  }
  wakeAndReview(rtc.screen == 1);
}

void Device::chargeMe(const char* why) {
  hal_.log((std::string("charge me: ") + why).c_str());
  g_.clear();
  g_.text(FONT_TITLE, 48, 160, "Charge me");
  g_.paragraph(FONT_BODY, 48, 214, 704,
               "The battery is too low to keep going. Plug in USB-C, then press any key. Your reviews are saved.");
  show(Refresh::Fast, "charge me");
  hal_.rtc().screen = 3;
  sleepNow(0);   // keys only: no timer check-ins on a battery this low
}

void Device::wakeAndReview(bool from_sleep_screen) {
  // Cards on flash and a clock estimate from the last check-in: show the first card straight away and join Wi-Fi
  // while it draws. The check-in runs in the background, and a stale batch gets swapped in between cards.
  buildQueue();
  if (hal_.rtc().anchor_server && cardsLeft() > 0) {
    startWifi();
    checkin_due_ = true;
    enterReview(Refresh::Fast);
    return;
  }
  (void)from_sleep_screen;
  // Nothing on flash to show: the first batch is all there is, so give it the time it needs (60 cards over Wi-Fi
  // take about 16 s on the board). Whatever is left carries on in the background on the done screen.
  syncOnScreen("Syncing", "syncing", true, cardsLeft() > 0 ? 10000 : 60000);
  enterReview(Refresh::Fast);
}

// ------------------------------------------------------------------ main loop
bool Device::loop() {
  if (sleeping_) return false;
  uint32_t now = hal_.millis();
  uint8_t held = hal_.keys();
  presses_ = hal_.presses();   // kept for the hardware check's key test, which reads keys raw
  keys_.poll(now, held, presses_);

  if (mode_ == Mode::WifiSetup) {
    PortalSubmit sub;
    if (hal_.portalPoll(sub)) {
      g_.clearRows(360, 420);
      g_.text(FONT_BODY_B, 48, 368, "Joining " + sub.ssid + "...");
      show(Refresh::Partial, "joining wi-fi");
      hal_.log(("portal: joining " + sub.ssid).c_str());
      if (hal_.wifiConnect(sub.ssid, sub.pass, 15000)) {
        ssid_ = sub.ssid;
        pass_ = sub.pass;
        if (!sub.server.empty()) server_ = sub.server;
        while (!server_.empty() && server_.back() == '/') server_.pop_back();
        hal_.setSetting("ssid", ssid_);
        hal_.setSetting("pass", pass_);
        hal_.setSetting("server", server_);
        api_.configure(server_, token_);
        hal_.portalStop();
        if (token_.empty()) {
          enterLink();
        } else {
          syncOnScreen("Syncing", "syncing", true, 15000);
          enterReview(Refresh::Fast);
        }
      } else {
        drawSetup("Couldn't join " + sub.ssid + ". Check the password and try again.");
        show(Refresh::Partial, "wi-fi error");
      }
    }
  }

  if (mode_ == Mode::Link && now >= next_poll_) {
    if (code_.device_code.empty()) {
      enterLink();
    } else {
      std::string token;
      bool expired = false;
      Net n = api_.pollToken(code_.device_code, token, expired);
      next_poll_ = hal_.millis() + code_.interval_s * 1000;
      if (n == Net::Ok && !token.empty()) {
        token_ = token;
        hal_.setSetting("token", token_);
        api_.configure(server_, token_);
        hal_.log("linked");
        syncOnScreen("Linked", "linked, syncing", true, 30000);   // and the first sync follows
        enterReview(Refresh::Fast);
      } else if (n == Net::Ok && expired) {
        enterLink();
      }
    }
  }

  for (Ev e = keys_.next(); e != Ev::None; e = keys_.next()) {
    onKey(e);
    if (sleeping_) return false;
  }
  if (mode_ == Mode::Check) {             // the key test and the charging watch run between key events
    checkLoop(hal_.millis());             // not now: a key handled above may have started a step after it
    if (sleeping_) return false;
  }

  if (toast_until_ && now > toast_until_) {   // put back whatever the toast covered
    toast_until_ = 0;
    if (mode_ == Mode::Review) showCurrent(Refresh::Partial, "note cleared");
    else if (mode_ == Mode::Menu) { drawMenu(); show(Refresh::Partial, "note cleared"); }
    else if (mode_ == Mode::Done) { drawDone(); show(Refresh::Partial, "note cleared"); }
  }

  // background work while nobody is pressing anything
  if (wifi_pending_) wifiStep();
  if (!keys_.busy() && (mode_ == Mode::Review || mode_ == Mode::Done)) {
    if (dl_.active) {
      downloadStep();
      if (mode_ == Mode::Done && swapped_) enterReview(Refresh::Fast);   // the cards the done screen was waiting on
      else if (mode_ == Mode::Done && !dl_.active) enterDone();          // it broke off: stop saying they're coming
    } else if (mode_ == Mode::Review && checkin_due_) {
      // The check-in itself waits for the next card (advance()), where it gets a screen of its own instead of
      // freezing this one. Joining Wi-Fi doesn't block, so it starts now and the sync between cards is quicker.
      if (!hal_.wifiConnected() && !syncFailedLately()) startWifi();
    } else if (!checkin_due_ && !wifi_pending_ && hal_.wifiConnected()) {
      // Nothing left to send or fetch. Wi-Fi idling between check-ins was most of the radio's cost in a session;
      // the next check-in rejoins in about a second with the cached access point.
      hal_.wifiOff();
      hal_.log("wifi off until the next check-in");
    }
  }

  // idle: send reviews, draw the sleep screen, sleep
  uint32_t idle = now - keys_.lastActivity();
  if (mode_ == Mode::Idle) {
    if (now - usb_checked_ >= 2000) {    // not every loop: each look is a few I2C reads and a USB frame count
      usb_checked_ = now;
      if (!hal_.power().usb) {
        goToSleep();
      } else if (now - idle_checkin_ > USB_CHECKIN_MS) {
        idle_checkin_ = now;
        syncBlocking(true, 30000);
        drawSleep();                     // a new due count, date or charge level; unchanged, show() skips it
        show(Refresh::Partial, "sleep screen");
        hal_.wifiOff();
      }
    }
  } else if (mode_ == Mode::WifiSetup || mode_ == Mode::Link) {
    if (idle > SETUP_TIMEOUT_MS && !hal_.power().usb) {
      hal_.portalStop();
      if (mode_ == Mode::WifiSetup && !ssid_.empty() && !token_.empty()) {
        goToSleep();                     // came here from the menu: the old network still works
      } else {
        g_.clearRows(STRIP_Y - 60, SCREEN_H);
        g_.centered(FONT_BODY_B, SCREEN_W / 2, STRIP_Y - 10, "Paused to save battery. Press any key to carry on.");
        show(Refresh::Partial, "setup paused");
        hal_.rtc().screen = 3;
        sleepNow(0);                     // keys only, setup starts over on the next press
      }
    }
  } else if (idle > cfg_.idle_s * 1000) {
    if (mode_ == Mode::Check && ck_.back == Mode::WifiSetup) leaveCheck();   // setup has its own, longer timeout
    else goToSleep();
  }
  return !sleeping_;
}

void Device::onKey(Ev e) {
  static const char* const EV[] = {"none", "1", "2", "3", "4", "hold 1", "hold 4", "menu"};
  static const char* const MODE[] = {"wifi setup", "link",           "review",         "menu", "info",
                                     "done",       "idle",           "confirm unlink", "hardware check",
                                     "confirm erase"};
  char msg[48];
  snprintf(msg, sizeof msg, "key %s in %s", EV[(int)e], MODE[(int)mode_]);
  hal_.log(msg);
  if (e == Ev::Menu && (mode_ == Mode::Review || mode_ == Mode::Done || mode_ == Mode::Idle)) return enterMenu();
  switch (mode_) {
    case Mode::Review: return reviewKey(e);
    case Mode::Menu: return menuKey(e);
    case Mode::Info:
      return infoKey(e);
    case Mode::Done:
      if (e == Ev::K1) lookForMoreCards();
      return;
    case Mode::Idle:
      if (keyIndex(e) >= 0) wakeAndReview(true);
      return;
    case Mode::Link:
      if (e == Ev::K4) enterWifiSetup();
      return;
    case Mode::WifiSetup:
      if (e == Ev::K1) enterCheck(Mode::WifiSetup);   // a new build: check the wiring before going any further
      else if (e == Ev::K4 && !ssid_.empty() && !token_.empty()) leaveSetup();
      return;
    case Mode::ConfirmUnlink:
      if (e == Ev::K3) unlink();
      else if (e == Ev::K4) enterMenu();
      return;
    case Mode::Check:
      return checkKey(e);
    case Mode::ConfirmErase:
      if (e == Ev::K3) eraseDevice();
      else if (e == Ev::K4) enterMenu();
      return;
  }
}

// ------------------------------------------------------------------ setup and linking
void Device::enterWifiSetup(const std::string& error) {
  mode_ = Mode::WifiSetup;
  hal_.wifiOff();
  hal_.portalStart(ap_name_, ap_pass_, server_);
  drawSetup(error);
  show(Refresh::Fast, "wi-fi setup");
  hal_.rtc().screen = 3;
}

void Device::enterLink(const std::string& error) {
  mode_ = Mode::Link;
  code_ = LinkCode{};
  next_poll_ = hal_.millis() + 10000;   // retry in 10 s if anything below fails
  if (!ensureWifi(15000)) {
    enterWifiSetup("Couldn't connect to " + ssid_ + ".");
    return;
  }
  Net n = api_.requestCode(code_);
  if (n != Net::Ok) {
    code_ = LinkCode{};
    drawLink("Can't reach the server at " + server_ + ". Trying again...");
    show(Refresh::Fast, "link code");
    return;
  }
  next_poll_ = hal_.millis() + code_.interval_s * 1000;
  drawLink(error);
  show(Refresh::Fast, "link code");
  hal_.rtc().screen = 3;
}

void Device::leaveSetup() {             // Cancel on the setup screen: keep the saved network
  hal_.portalStop();
  hal_.log("wifi setup cancelled");
  syncOnScreen("Syncing", "syncing", true, 10000);
  enterReview(Refresh::Fast);
}

void Device::unlink() {
  showSyncing("Unlinking", "unlinking");   // the last upload and the unlink call
  if (ensureWifi(8000)) {
    if (log_.count()) uploadAll();   // last chance for unsynced reviews
    api_.unlink();                   // so the account stops listing it
  }
  size_t lost = log_.count();
  if (lost) {
    char msg[64];
    snprintf(msg, sizeof msg, "unlinked, %u unsynced reviews dropped", (unsigned)lost);
    hal_.log(msg);
  } else {
    hal_.log("unlinked");
  }
  log_.clear();                          // reviews and cards belong to the old account
  store_.clear();
  queue_.clear();
  pos_ = 0;
  token_.clear();
  hal_.setSetting("token", "");
  api_.configure(server_, token_);
  enterLink();
}

void Device::eraseDevice() {
  showSyncing("Erasing", "erasing");   // the last upload and the unlink call, as for Unlink
  if (!token_.empty() && ensureWifi(8000)) {
    if (log_.count()) uploadAll();
    api_.unlink();
  }
  char msg[64];
  snprintf(msg, sizeof msg, "erasing everything (%u unsynced reviews)", (unsigned)log_.count());
  hal_.log(msg);
  g_.clear();
  g_.text(FONT_TITLE, 48, 160, "Erased");
  g_.paragraph(FONT_BODY, 48, 214, 704, "Starting again from Wi-Fi setup.");
  show(Refresh::Fast, "erased");
  hal_.rtc().magic = 0;   // RAM that survives sleep goes too: the next boot is a power-on
  hal_.eraseAll();
  sleeping_ = true;       // the simulator returns from restart(); the board doesn't
  hal_.restart();
}

void Device::unauthorized() {
  hal_.log("server says this device is unlinked");
  token_.clear();
  hal_.setSetting("token", "");
  api_.configure(server_, token_);
  enterLink("This Rekadeck was unlinked. Link it again to keep syncing.");
}

// ------------------------------------------------------------------ sync
bool Device::radioAllowed() {
  PowerInfo p = hal_.power();
  if (p.usb || !p.gauge || p.mv >= RADIO_MIN_MV) {
    low_batt_ = false;
    return true;
  }
  if (!low_batt_) {
    char msg[64];
    snprintf(msg, sizeof msg, "battery at %d mV: no wi-fi until it's charged", p.mv);
    hal_.log(msg);
  }
  low_batt_ = true;
  return false;
}

bool Device::ensureWifi(uint32_t timeout_ms) {
  if (hal_.wifiConnected()) { wifi_pending_ = false; return true; }
  if (ssid_.empty() || !radioAllowed()) return false;
  if (wifi_pending_) {                   // a background join is already under way, wait for it
    uint32_t t0 = hal_.millis();
    while (!hal_.wifiConnected() && hal_.millis() - t0 < timeout_ms) hal_.delayMs(50);
    wifi_pending_ = false;
    if (hal_.wifiConnected()) { hal_.log("wifi connected"); return true; }
  }
  last_wifi_try_ = hal_.millis();
  bool ok = hal_.wifiConnect(ssid_, pass_, timeout_ms);
  hal_.log(ok ? "wifi connected" : "wifi failed");
  wifi_failed_at_ = ok ? 0 : (hal_.millis() | 1);
  return ok;
}

Net Device::uploadAll() {
  auto all = log_.all();
  for (size_t i = 0; i < all.size(); i += 50) {
    std::vector<Review> part(all.begin() + i, all.begin() + std::min(all.size(), i + 50));
    std::set<uint32_t> accepted;
    Net n = api_.upload(part, accepted);
    if (n != Net::Ok) return n;
    log_.remove(accepted);
    char msg[64];
    snprintf(msg, sizeof msg, "uploaded %u reviews", (unsigned)accepted.size());
    hal_.log(msg);
  }
  return Net::Ok;
}

void Device::syncSucceeded(const Hello& h) {
  if (!h.settings_json.empty() && h.settings_json != "null") {
    cfg_.parse(h.settings_json);
    hal_.setSetting("cfg", h.settings_json);
  }
  char msg[96];
  snprintf(msg, sizeof msg, "checked in: v%d%s, %u due now, %u on this device", h.version, h.stale ? " (batch is stale)" : "",
           (unsigned)h.due_now, (unsigned)h.due_device);
  hal_.log(msg);
  since_checkin_ = 0;
  checkin_due_ = false;
  checked_in_ = true;
  checked_in_at_ = hal_.millis() | 1;
  sync_failed_at_ = 0;
  hal_.rtc().fail_streak = 0;
  if (!ota_confirmed_) {   // a check-in got through: this firmware works, keep it (see PROTOCOL.md, Firmware updates)
    ota_confirmed_ = true;
    if (hal_.otaConfirm()) hal_.log("new firmware confirmed");
  }
  if (!h.fw_version.empty() && h.fw_version != FW_VERSION) {
    if (ota_.version != h.fw_version) {
      hal_.log(("firmware " + h.fw_version + (auto_update_ ? " is available, installs at sleep" : " is available, auto-update is off")).c_str());
      update_note_.clear();   // the offer is the news now, not an older "up to date"
    }
    ota_.version = h.fw_version;
    ota_.url = h.fw_url;
    ota_.sha256 = h.fw_sha256;
    ota_.size = h.fw_size;
  } else {
    ota_.version.clear();
  }
}

void Device::syncFailed(const char* why) {
  RtcState& rtc = hal_.rtc();
  rtc.fail_streak++;
  sync_failed_at_ = hal_.millis() | 1;
  char msg[80];
  snprintf(msg, sizeof msg, "sync failed: %s (%lu in a row)", why, (unsigned long)rtc.fail_streak);
  hal_.log(msg);
}

bool Device::syncFailedLately() {
  // signed: the stamp is millis | 1, so in the same millisecond it can be one ahead of millis
  return sync_failed_at_ && (int32_t)(hal_.millis() - sync_failed_at_) < (int32_t)WIFI_RETRY_MS;
}

bool Device::syncBlocking(bool download, uint32_t budget_ms) {
  uint32_t t0 = hal_.millis();
  if (!radioAllowed()) return false;   // not the server's fault: no failure counted, the sleep screen says why
  if (!ensureWifi(8000)) { syncFailed("wi-fi didn't connect"); return false; }
  Net n = uploadAll();
  if (n == Net::Unauthorized) { unauthorized(); return false; }
  if (n != Net::Ok) { syncFailed("couldn't upload reviews"); return false; }
  Hello h;
  n = api_.hello(store_.batchId(), store_.version(), cardsLeft(), (uint32_t)log_.count(), h);
  if (n == Net::Unauthorized) { unauthorized(); return false; }
  if (n != Net::Ok) { syncFailed("check-in didn't answer"); return false; }
  syncSucceeded(h);
  if (download && (dl_.active || h.stale)) {
    if (!dl_.active) startDownload();   // one already under way carries on: restarting it would lose its files
    while (dl_.active && hal_.millis() - t0 < budget_ms) downloadStep();
    if (swapped_ && (mode_ != Mode::Review || queue_.empty())) {   // nothing on screen to protect, use it now
      swapped_ = false;
      buildQueue();
    }
  }
  return true;
}

bool Device::syncOnScreen(const char* title, const char* why, bool download, uint32_t budget_ms) {
  showSyncing(title, why);
  bool ok = syncBlocking(download, budget_ms);
  // The board counts presses while it's busy and hands them over afterwards. Made at a screen saying "Syncing",
  // they were someone checking whether it was alive, not an answer to the card that comes up next.
  hal_.presses();
  keys_.ignoreUntilReleased();
  return ok;
}

void Device::startWifi() {
  if (ssid_.empty() || wifi_pending_ || hal_.wifiConnected() || !radioAllowed()) return;
  wifi_pending_ = true;
  wifi_started_ = last_wifi_try_ = hal_.millis();
  hal_.wifiBegin(ssid_, pass_);
}

void Device::wifiStep() {
  if (hal_.wifiConnected()) {
    wifi_pending_ = false;
    hal_.log("wifi connected");
  } else if (hal_.millis() - wifi_started_ > 12000) {
    wifi_pending_ = false;
    wifi_failed_at_ = hal_.millis() | 1;
    hal_.wifiOff();
    syncFailed("wi-fi didn't connect");
  }
}

void Device::startDownload() {
  std::string json;
  dl_failed_ = true;   // until the files are on their way
  if (api_.manifest(cfg_.batch_limit, json) != Net::Ok) return hal_.log("batch list didn't download");
  std::vector<CardMeta> cards;
  std::vector<std::pair<std::string, uint32_t>> assets;
  std::string batch_id;
  int version = 0;
  if (!CardStore::parseManifest(json, cards, batch_id, version, assets)) return hal_.log("batch list didn't parse");
  store_.beginStaging();   // empties the spare slot, so what's free now is what this batch can have

  // Only as many cards as the flash holds, earliest due first. LittleFS gives every file at least one 4 KB block,
  // and the reserve covers the panel picture, the review log growing, and the file system's own bookkeeping.
  const size_t BLOCK = 4096, RESERVE = 160 * 1024;
  auto blocks = [&](uint32_t n) { return ((n ? n : 6000) + BLOCK - 1) / BLOCK * BLOCK; };
  size_t free = hal_.freeBytes(), need = assets.size() * BLOCK + 2 * BLOCK, keep = 0;   // + manifest and done list
  while (keep < cards.size() && need + blocks(cards[keep].size) + RESERVE <= free) need += blocks(cards[keep++].size);
  if (keep < cards.size()) {
    char msg[96];
    snprintf(msg, sizeof msg, "flash holds %u of %u cards (%u KB free)", (unsigned)keep, (unsigned)cards.size(),
             (unsigned)(free / 1024));
    hal_.log(msg);
    if (!keep) return;
    JsonDocument doc;   // the manifest that gets committed lists only the cards that came down
    if (deserializeJson(doc, json)) return;
    JsonArray arr = doc["cards"].as<JsonArray>();
    while (arr.size() > keep) arr.remove(keep);
    json.clear();
    serializeJson(doc, json);
    cards.resize(keep);
  }

  dl_ = Download{};
  dl_.active = true;
  dl_failed_ = false;
  dl_.manifest = json;
  for (auto& a : assets) dl_.files.push_back({"asset/" + a.first, "a_" + a.first + ".z", "", a.second});
  for (auto& c : cards) dl_.files.push_back({"frame/" + c.id + "?rev=" + std::to_string(c.rev), "c_" + c.id + ".z", c.id, c.crc});
  char msg[80];
  snprintf(msg, sizeof msg, "downloading batch %s: %u cards", batch_id.c_str(), (unsigned)cards.size());
  hal_.log(msg);
}

void Device::downloadStep() {
  if (!dl_.active) return;
  if (dl_.next >= dl_.files.size()) {
    if (store_.commitStaging(dl_.manifest)) {
      for (auto& id : session_done_) store_.markDone(id);   // graded while this batch was downloading
      loadAssets();
      swapped_ = true;
      hal_.log("new batch ready");
    } else {
      dl_failed_ = true;
      hal_.log("new batch didn't save");
    }
    dl_.active = false;
    return;
  }
  auto& f = dl_.files[dl_.next];
  std::string z;
  bool reused = false;
  if (!f.card.empty()) {                     // unchanged card already on flash: copy it instead of downloading
    for (auto& c : store_.cards())
      if (c.id == f.card && c.crc == f.crc && store_.readFrame(c, z)) { reused = true; break; }
  } else if (store_.assetCrc(f.name.substr(2, f.name.size() - 4)) == f.crc) {
    reused = store_.readAsset(f.name.substr(2, f.name.size() - 4), z);
  }
  if (!reused && api_.blob(f.url, z) != Net::Ok) {
    hal_.log(("download failed: " + f.url).c_str());
    dl_.active = false;
    dl_failed_ = true;
    return;
  }
  if (!inflateFrame(z, f.crc, scratch_.data())) {
    hal_.log(("bad frame: " + f.name).c_str());
    dl_.active = false;
    dl_failed_ = true;
    return;
  }
  hal_.writeFile(store_.stagingPath(f.name).c_str(), z.data(), z.size());
  dl_.next++;
}

void Device::loadAssets() {
  strip_question_.clear();
  strip_choice_.clear();
  for (auto* pair : {&strip_question_, &strip_choice_}) {
    std::string name = pair == &strip_question_ ? "question" : "choice", z;
    if (store_.readAsset(name, z) && inflateFrame(z, store_.assetCrc(name), scratch_.data()))
      pair->assign(scratch_.begin() + STRIP_Y * ROW_BYTES, scratch_.end());
  }
}

// ------------------------------------------------------------------ reviewing
uint64_t Device::serverNow() {
  RtcState& rtc = hal_.rtc();
  if (!rtc.anchor_server) return 0;
  return rtc.anchor_server + (hal_.rtcMs() - rtc.anchor_rtc);
}

bool Device::isDue(const CardMeta& c) {
  uint64_t now = serverNow();
  return now == 0 || c.due_ms <= now + 60000;   // no clock yet: treat the whole batch as due
}

void Device::buildQueue() {
  queue_.clear();
  pos_ = 0;
  refill_asked_ = false;
  for (auto& c : store_.cards())
    if (!store_.isDone(c.id) && isDue(c)) queue_.push_back(c.id);
}

uint32_t Device::cardsLeft() {
  uint32_t n = 0;
  for (auto& c : store_.cards())
    if (!store_.isDone(c.id) && isDue(c)) n++;
  return n;
}

bool Device::openCard(const std::string& id) {
  for (auto& c : store_.cards()) {
    if (c.id != id) continue;
    std::string z;
    if (!store_.readFrame(c, z) || !inflateFrame(z, c.crc, frame_.data())) {
      hal_.log(("can't open card " + id).c_str());
      return false;
    }
    cur_ = c;
    return true;
  }
  return false;
}

void Device::enterReview(Refresh first) {
  mode_ = Mode::Review;
  if (swapped_) swapped_ = false;
  buildQueue();
  while (pos_ < queue_.size() && !openCard(queue_[pos_])) pos_++;
  if (pos_ >= queue_.size()) return enterDone(first);
  view_ = View::Question;
  showCurrent(first, "first card", first == Refresh::Partial);
}

void Device::drawQuestion() {
  g_.load(frame_.data());
  for (auto& band : cur_.hide) g_.clearRows(std::max(0, band.first), std::min(SCREEN_H, band.second));
  auto& strip = cur_.qstrip == "choice" ? strip_choice_ : strip_question_;
  if (!cur_.qstrip.empty() && !strip.empty())
    std::copy(strip.begin(), strip.end(), g_.fb + STRIP_Y * ROW_BYTES);
}

void Device::drawPicked() {
  g_.load(frame_.data());
  if (cur_.correct >= 0 && cur_.correct < 4) {
    int* b = cur_.boxes[cur_.correct];
    g_.invert(b[0] + 3, b[1] + 3, b[2] - 6, b[3] - 6);
  }
  if (picked_ >= 0 && picked_ < 4) {
    int* b = cur_.boxes[picked_];
    g_.frame(b[0] - 4, b[1] - 4, b[2] + 8, b[3] + 8, 5);
  }
}

void Device::showCurrent(Refresh mode, const char* why, bool page_turn) {
  if (view_ == View::Question) drawQuestion();
  else if (view_ == View::Answer) g_.load(frame_.data());
  else drawPicked();
  drawStatus();
  show(mode, why, page_turn);
  hal_.rtc().screen = 2;
  if (view_ == View::Question) shown_at_ = hal_.rtcMs();
}

void Device::reviewKey(Ev e) {
  int k = keyIndex(e);
  if (e == Ev::Hold1) return undo();
  if (e == Ev::Hold4) {
    if (view_ == View::Question) skip();
    return;
  }
  if (k < 0) return;
  if (view_ == View::Question) {
    revealed_at_ = hal_.rtcMs();
    if (cur_.choice) {
      picked_ = k;
      view_ = View::Picked;
      grade(k == cur_.correct ? 3 : 1, k);
      showCurrent(Refresh::Partial, "pick");
    } else {
      view_ = View::Answer;
      showCurrent(Refresh::Partial, "reveal answer");
    }
  } else if (view_ == View::Answer) {
    grade(k + 1, -1);
    advance();
  } else {
    advance();
  }
}

void Device::grade(int g, int choice) {
  Review r;
  r.card = cur_.id;
  r.grade = g;
  r.shown = shown_at_;
  r.revealed = revealed_at_;
  r.graded = hal_.rtcMs();
  r.choice = choice;
  r.boot = hal_.rtc().boot_id;
  log_.append(r);
  bool again = g == 1;
  history_.push_back({cur_.id, again});
  if (again) {
    size_t at = std::min(queue_.size(), pos_ + 1 + cfg_.again_gap);
    queue_.insert(queue_.begin() + at, cur_.id);
  } else {
    store_.markDone(cur_.id);
    session_done_.insert(cur_.id);
  }
  session_reviews_++;
  if (++since_checkin_ >= cfg_.checkin_every) checkin_due_ = true;
}

void Device::advance() {
  if (swapped_) {                      // a fresh batch arrived in the background
    swapped_ = false;
    buildQueue();
    pos_ = 0;
  } else {
    pos_++;
  }
  while (pos_ < queue_.size() && !openCard(queue_[pos_])) pos_++;
  if (pos_ >= queue_.size()) return lookForMoreCards();
  // Down to the last few: check in now, behind this card, so more cards are already on their way when the last one
  // is done. Asking only then meant a frozen screen while they downloaded, and "All caught up" if that ran long.
  if (!refill_asked_ && !dl_.active && queue_.size() - pos_ <= REFILL_AT &&
      (!checked_in_at_ || hal_.millis() - checked_in_at_ > REFILL_GAP_MS)) {
    refill_asked_ = true;
    checkin_due_ = true;
  }
  if (checkin_due_) {
    // Between cards, never over one: the answered card goes, "Syncing" comes up for the upload and check-in, then
    // the next card. A batch it starts downloading carries on behind the cards with the keys free.
    if (syncFailedLately() || !radioAllowed()) {   // offline a moment ago, or too weak for Wi-Fi: reviews wait
      checkin_due_ = false;
      since_checkin_ = 0;
    } else {
      syncOnScreen("Syncing", "syncing", true, 0);
      if (mode_ != Mode::Review) return;   // the server unlinked it: the link screen stays up
    }
  }
  view_ = View::Question;
  picked_ = -1;
  showCurrent(Refresh::Partial, "next card", true);
}

void Device::lookForMoreCards() {
  if (!dl_.active && radioAllowed()) {
    mode_ = Mode::Done;               // what follows, unless the server unlinks it meanwhile
    syncOnScreen("Looking for more cards", "looking for more cards", true, 0);   // a download carries on in the loop
    if (mode_ != Mode::Done) return;  // the link screen stays up
  }
  enterReview(Refresh::Partial);      // a card the check-in made due, or the done screen saying what comes next
}

void Device::undo() {
  if (history_.empty()) return toast("Nothing to undo");
  Graded last = history_.back();
  auto all = log_.all();
  if (all.empty() || all.back().card != last.id) return toast("Already synced, can't undo");
  log_.remove({all.back().rid});
  history_.pop_back();
  if (last.again) {                    // drop the copy that was queued for later
    for (size_t i = queue_.size(); i-- > pos_ + 1;)
      if (queue_[i] == last.id) { queue_.erase(queue_.begin() + i); break; }
  } else {
    store_.unmarkDone(last.id);
    session_done_.erase(last.id);
  }
  if (session_reviews_) session_reviews_--;
  if (mode_ != Mode::Review) mode_ = Mode::Review;
  queue_.insert(queue_.begin() + std::min(pos_, queue_.size()), last.id);
  if (!openCard(last.id)) return;
  picked_ = -1;
  view_ = cur_.choice ? View::Question : View::Answer;   // a basic card comes back on its answer, ready to regrade
  revealed_at_ = hal_.rtcMs();
  showCurrent(Refresh::Partial, "undo");   // the card coming back on its answer says it all, no note needed
}

void Device::skip() {
  Review r;
  r.card = cur_.id;
  r.grade = 0;
  r.shown = shown_at_;
  r.graded = hal_.rtcMs();
  r.boot = hal_.rtc().boot_id;
  log_.append(r);
  queue_.push_back(cur_.id);
  history_.clear();                    // a skip isn't undoable, and it breaks the chain behind it
  advance();
}

// ------------------------------------------------------------------ menu, info, done, sleep
void Device::enterMenu() {
  // coming back from a page the menu opened keeps where the menu itself came from, and its highlight
  if (mode_ != Mode::Menu && mode_ != Mode::Info && mode_ != Mode::ConfirmUnlink && mode_ != Mode::Check &&
      mode_ != Mode::ConfirmErase) {
    before_menu_ = mode_;
    menu_sel_ = 0;                       // a fresh menu always starts on "Back to cards"
    menu_note_ = SyncNote::None;
  }
  mode_ = Mode::Menu;
  drawMenu();
  show(Refresh::Partial, "menu");
  hal_.rtc().screen = 3;
}

void Device::menuKey(Ev e) {
  if (e == Ev::K1) menu_sel_ = (menu_sel_ + MENU_N - 1) % MENU_N;
  else if (e == Ev::K2) menu_sel_ = (menu_sel_ + 1) % MENU_N;
  else if (e == Ev::K4) {
    if (before_menu_ == Mode::Review) { mode_ = Mode::Review; showCurrent(Refresh::Partial, "back to card"); }
    else enterReview(Refresh::Partial);
    return;
  } else if (e == Ev::K3) {
    switch (menu_sel_) {
      case MI_BACK:
        if (before_menu_ == Mode::Review && !queue_.empty()) { mode_ = Mode::Review; showCurrent(Refresh::Partial, "back to card"); }
        else enterReview(Refresh::Partial);
        return;
      case MI_SYNC:
        menu_note_ = syncOnScreen("Syncing", "syncing", true, 30000) ? SyncNote::Ok : SyncNote::Failed;
        if (!dl_.active) hal_.wifiOff();   // a download still going carries on behind the cards
        break;
      case MI_INFO: return enterInfo();
      case MI_CHECK: return enterCheck(Mode::Menu);
      case MI_WIFI: return enterWifiSetup();
      case MI_UNLINK:
        mode_ = Mode::ConfirmUnlink;
        drawConfirmUnlink();
        show(Refresh::Partial, "confirm unlink");
        return;
      case MI_ERASE:
        mode_ = Mode::ConfirmErase;
        drawConfirmErase();
        show(Refresh::Partial, "confirm erase");
        return;
      case MI_SLEEP: return goToSleep();
    }
  } else {
    return;
  }
  drawMenu();
  show(Refresh::Partial, "menu");   // one refresh carries the new highlight, or the sync result with the badge gone
}

void Device::enterInfo() {
  mode_ = Mode::Info;
  drawInfo();
  show(Refresh::Partial, "device info");
}

void Device::enterDone(Refresh mode) {
  mode_ = Mode::Done;
  const char* why = drawDone();
  show(mode, why, mode == Refresh::Partial);
  hal_.rtc().screen = 3;
}

void Device::goToSleep(bool synced) {
  hal_.log("going to sleep");
  if (mode_ == Mode::Check) hal_.gaugeFast(false);
  // A check-in already went through and nothing happened since: the due count is fresh, no need to ask again.
  // One that failed a moment ago (the server is asleep or gone) isn't retried: that was 8 s of radio each time.
  bool fresh = synced || (checked_in_ && log_.count() == 0 && !dl_.active);
  bool online = fresh;
  if (!fresh && !syncFailedLately() && radioAllowed() && (dl_.active || log_.count() > 0 || hal_.wifiConnected()))
    online = syncOnScreen("Syncing", "syncing", true, 20000);   // the sleep screen follows, with what it brought
  if (!online) hal_.rtc().due_now = cardsLeft();   // best guess: what can be reviewed on board
  if (dl_.active) dl_.active = false;   // unfinished downloads start over next time
  if (online && auto_update_ && !ota_.version.empty() && installUpdate(false)) return;   // restarting into it
  drawSleep();
  RtcState& rtc = hal_.rtc();
  bool deep_clean = !rtc.last_full_rtc || hal_.rtcMs() - rtc.last_full_rtc > DEEP_CLEAN_MS;
  if (deep_clean) show(Refresh::Full, "sleep screen, daily deep clean");   // 4 s of flashing, while it's being put down
  else if (rtc.screen == 1) show(Refresh::Partial, "sleep screen", true);  // already up: just its numbers change
  else show(Refresh::Fast, "sleep screen");                                // a clean picture to sit there for hours
  rtc.screen = 1;
  hal_.wifiOff();
  if (hal_.power().usb) {              // on USB: stay awake with the sleep screen up, check in now and then
    mode_ = Mode::Idle;
    idle_checkin_ = usb_checked_ = hal_.millis();
    return;
  }
  // The quiet check-in. While the server can't be reached (a laptop that's shut or out of the house) each try is a
  // Wi-Fi join and a timeout for nothing, so back off: 4 h, then 8 h, then 12 h until one gets through. A random
  // extra (up to 10%, and up to 30 min on the midnight wake) keeps every device in a time zone from checking in at
  // the same moment, which would hit the server all at once every night at 12:01.
  uint64_t timer = cfg_.wake_every_min * 60000ULL * (1 + std::min<uint32_t>(rtc.fail_streak, 2));
  timer += hal_.random32() % (timer / 10 + 1);
  if (uint64_t now = serverNow()) {      // also wake just after midnight, so the date on the sleep screen turns over
    const uint64_t day = 86400000ULL;
    uint64_t local = now + (int64_t)cfg_.utc_offset_min * 60000;
    timer = std::min(timer, day - local % day + 60000 + hal_.random32() % (30 * 60000));
  }
  sleepNow((uint32_t)timer);
}

bool Device::installUpdate(bool asked) {
  RtcState& rtc = hal_.rtc();
  uint32_t tag = 2166136261u;   // FNV-1a of the version string (crc32 is a miniz macro outside gfx.cpp)
  for (unsigned char c : ota_.version) tag = (tag ^ c) * 16777619u;
  tag |= 1;
  if (!asked && rtc.ota_fail_hash == tag && rtc.ota_fail_rtc && hal_.rtcMs() - rtc.ota_fail_rtc < OTA_RETRY_MS) return false;
  // At sleep it has to know the battery will see the install through: a good reading, or USB power. The gauge runs
  // off the cell, so no reading at all with a computer on the port means a missing or dead cell and the board running
  // off USB (the USB test itself needs the gauge, to tell USB power from a laptop talking to a board on battery).
  // Asked for, only a battery known to be low stops it: an interrupted download leaves the old firmware running.
  PowerInfo p = hal_.power();
  bool low = p.gauge && (p.pct < OTA_MIN_PCT || p.mv < OTA_MIN_MV);
  if (!p.usb && (asked ? low : (p.gauge ? low : !p.usb_host))) {
    hal_.log(("firmware " + ota_.version + (p.gauge ? " waits for more charge" : " waits for a battery reading or USB")).c_str());
    update_note_ = "needs 40% charge or USB power";
    return false;
  }
  if (!ensureWifi(8000)) {
    update_note_ = "Wi-Fi didn't connect";
    return false;
  }
  g_.clear();
  g_.text(FONT_TITLE, 48, 160, "Updating");
  g_.paragraph(FONT_BODY, 48, 214, 704, "Installing Rekadeck " + ota_.version +
               ". It takes about a minute, then the Rekadeck restarts by itself. Your reviews are saved.");
  show(Refresh::Fast, "updating");
  hal_.log(("installing firmware " + ota_.version).c_str());
  std::string error;
  if (hal_.otaInstall(server_ + ota_.url, token_, ota_.size, ota_.sha256, error)) {
    hal_.log(("firmware " + ota_.version + " installed, restarting").c_str());
    savePanel();
    sleeping_ = true;   // the simulator returns from restart(); the board doesn't
    hal_.restart();
    return true;
  }
  hal_.log(("firmware update failed: " + error).c_str());
  update_note_ = ota_.version + " failed: " + error;
  rtc.ota_fail_hash = tag;   // not again for a day, or a broken release would cost a download every check-in
  rtc.ota_fail_rtc = hal_.rtcMs() | 1;
  ota_.version.clear();
  return false;
}

void Device::savePanel() {
  if (!panel_dirty_ || !shown_valid_) return;
  hal_.writeFile(PANEL_PATH, shown_.data(), shown_.size());
  panel_dirty_ = false;
}

void Device::sleepNow(uint32_t timer_ms) {
  RtcState& rtc = hal_.rtc();
  if (!ota_confirmed_) {   // got this far without crashing: keep this firmware even without a check-in (offline)
    ota_confirmed_ = true;
    if (hal_.otaConfirm()) hal_.log("new firmware confirmed");
  }
  savePanel();
  if (mode_ == Mode::Check) hal_.gaugeFast(false);
  rtc.awake_ms += hal_.millis();
  char msg[96];
  snprintf(msg, sizeof msg, "asleep after %lu ms awake (%lu s awake over %lu wakes since power-on)",
           (unsigned long)hal_.millis(), (unsigned long)(rtc.awake_ms / 1000), (unsigned long)rtc.wakes);
  hal_.log(msg);
  sleeping_ = true;
  hal_.deepSleep(timer_ms);
}

// ------------------------------------------------------------------ drawing
static inline uint32_t bits(uint8_t b) { return (uint32_t)__builtin_popcount((unsigned)b); }

bool Device::show(Refresh mode, const char* why, bool page_turn) {
  RtcState& rtc = hal_.rtc();
  uint32_t changed = 0, cleared = 0;
  if (shown_valid_) {
    for (size_t i = 0; i < FB_BYTES; i++) {
      uint8_t was = shown_[i], now = g_.fb[i];
      changed += bits(was ^ now);
      cleared += bits(was & ~now);         // black to white: the transition that leaves a grey ghost behind
    }
    if (mode == Refresh::Partial && changed == 0) return true;   // the panel already shows exactly this
  }
  uint32_t ppm = (uint32_t)((uint64_t)cleared * 1000000ULL / (SCREEN_W * SCREEN_H));
  if (page_turn && mode == Refresh::Partial && rtc.ink_ppm + ppm >= cfg_.clean_after_pct * 10000u) mode = Refresh::Fast;
  if (force_full_) {
    mode = Refresh::Full;
    force_full_ = false;
  }
  uint32_t t0 = hal_.millis();
  bool ok = hal_.display(g_.fb, shown_valid_ ? shown_.data() : nullptr, mode, why);
  last_refresh_ms_ = hal_.millis() - t0;
  panel_dirty_ = true;
  if (mode == Refresh::Partial) {
    rtc.partials++;
    rtc.ink_ppm += ppm;
  } else {
    rtc.partials = 0;
    rtc.ink_ppm = 0;
    if (mode == Refresh::Full) rtc.last_full_rtc = hal_.rtcMs() | 1;
  }
  shown_.assign(g_.fb, g_.fb + FB_BYTES);
  shown_valid_ = true;
  return ok;
}

// Status icons, # is ink. 14 rows at most, the height of the status line.
static const char* ICON_SYNC[14] = {"......###......", "....#######....", "..####...####..", "..##.......##..",
                                    "............##.", "..........#####", "..#........###.", ".###........#..",
                                    "#####..........", ".##............", "..##.......##..", "..####...####..",
                                    "....#######....", "......###......"};
static const char* ICON_CHECK[11] = {"............##", "...........###", "..........###.", ".........###..",
                                     "........###...", "##.....###....", "###...###.....", ".###.###......",
                                     "..#####.......", "...###........", "....#........."};
static const char* ICON_UPLOAD[12] = {".....##.....", "....####....", "...######...", "..########..", ".....##.....",
                                      ".....##.....", ".....##.....", ".....##.....", "##........##", "##........##",
                                      "############", "############"};

static void drawIcon(Gfx& g, const char* const* rows, int n, int right, int y) {   // right-aligned to just left of right
  int w = (int)std::strlen(rows[0]);
  for (int r = 0; r < n; r++)
    for (int c = 0; c < w; c++)
      if (rows[r][c] == '#') g.pixel(right - w + c, y + r, true);
}

// Two arcs chasing each other clockwise, each ending in an arrowhead: the sync icon, drawn at any size. Angles are
// degrees counterclockwise from 3 o'clock, as on paper (screen y grows down, so it's flipped here).
static void drawSyncGlyph(Gfx& g, int cx, int cy, int r, int thick) {
  const float PI_F = 3.14159265f;
  auto at = [&](float deg, float rad, float& x, float& y) {
    x = cx + rad * std::cos(deg * PI_F / 180);
    y = cy - rad * std::sin(deg * PI_F / 180);
  };
  struct Arc { float from, to; } arcs[2] = {{40, 165}, {220, 345}};   // each runs clockwise from `to` down to `from`
  int box = r + thick * 3;
  for (int y = cy - box; y <= cy + box; y++)
    for (int x = cx - box; x <= cx + box; x++) {
      float dx = (float)(x - cx), dy = (float)(cy - y), d = std::sqrt(dx * dx + dy * dy);
      if (d < r - thick / 2.0f || d > r + thick / 2.0f) continue;
      float a = std::atan2(dy, dx) * 180 / PI_F;
      if (a < 0) a += 360;
      for (auto& arc : arcs)
        if (a >= arc.from && a <= arc.to) g.pixel(x, y, true);
    }
  for (auto& arc : arcs) {   // an arrowhead at each arc's clockwise end, pointing on round the circle
    float bx1, by1, bx2, by2, tx, ty;
    at(arc.from, r - thick * 1.6f, bx1, by1);
    at(arc.from, r + thick * 1.6f, bx2, by2);
    at(arc.from - 24, (float)r, tx, ty);
    int x0 = (int)std::min({bx1, bx2, tx}), x1 = (int)std::max({bx1, bx2, tx}) + 1;
    int y0 = (int)std::min({by1, by2, ty}), y1 = (int)std::max({by1, by2, ty}) + 1;
    auto side = [](float ax, float ay, float bx, float by, float px, float py) {
      return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        float s1 = side(bx1, by1, bx2, by2, x, y), s2 = side(bx2, by2, tx, ty, x, y), s3 = side(tx, ty, bx1, by1, x, y);
        if ((s1 >= 0 && s2 >= 0 && s3 >= 0) || (s1 <= 0 && s2 <= 0 && s3 <= 0)) g.pixel(x, y, true);
      }
  }
}

void Device::showSyncing(const char* title, const char* why) {
  // A word in the status line went unnoticed: the card stayed up, stopped answering, and got pressed at. So the
  // whole screen changes, to a big sync icon and a line saying the keys come back.
  g_.clear();
  drawStatus();
  drawSyncGlyph(g_, SCREEN_W / 2, 150, 46, 10);
  g_.centered(FONT_TITLE, SCREEN_W / 2, 236, title);
  g_.centered(FONT_BODY, SCREEN_W / 2, 292, "The keys work again in a moment.");
  show(Refresh::Partial, why);
  hal_.rtc().screen = 3;   // not the sleep screen, nor a card
}

void Device::toast(const std::string& text) {
  g_.clearRows(STRIP_Y, SCREEN_H);       // a short note in the label strip, put back after 2.5 s
  g_.centered(FONT_SMALL_B, SCREEN_W / 2, STRIP_Y + 14, text);
  show(Refresh::Partial, ("note: " + text).c_str());
  toast_until_ = hal_.millis() + 2500;
}

void Device::drawStatus(bool asleep, SyncNote note) {
  // The date on the left, the icons on the right, between the cards' 48 px margins. It rides along with refreshes
  // the device makes anyway, never one of its own, so it's as fresh as the last refresh.
  static const char* DAYS[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  static const char* MONTHS[12] = {"January", "February", "March", "April", "May", "June", "July", "August",
                                   "September", "October", "November", "December"};
  static const char* BOLT[9] = {"....##", "...##.", "..##..", ".##...", "######", "...##.", "..##..", ".##...", "##...."};
  int x = 48;
  if (uint64_t now = serverNow()) {   // no date until the first check-in after power-on
    std::time_t t = (std::time_t)((int64_t)(now / 1000) + cfg_.utc_offset_min * 60);
    if (std::tm* d = std::gmtime(&t))
      x = g_.text(FONT_SMALL, 48, STATUS_Y - 3, std::string(DAYS[d->tm_wday]) + ", " + MONTHS[d->tm_mon] + " " + std::to_string(d->tm_mday)) + 28;
  }
  // The device's buffer: cards on it that can be reviewed right now. Down one per card done (an Again stays in), up
  // as soon as a batch downloaded in the background lands. The big count on the sleep screen is the account's.
  if (!token_.empty()) g_.text(FONT_SMALL, x, STATUS_Y - 3, std::to_string(cardsLeft()) + " ready");

  // Wi-Fi: a dot and two arcs fanning up from it. When the last try to reach the server failed, the outer arc gives
  // way to a small x. It's about the server, not the radio: the radio is off between check-ins on purpose.
  bool online = !asleep && hal_.rtc().fail_streak == 0;
  if (!asleep) {
    int cx = WIFI_X + 8, cy = STATUS_Y + 13;
    for (int dy = 0; dy < 14; dy++)
      for (int dx = -8; dx <= 8; dx++) {
        int d2 = dx * dx + dy * dy;
        bool arc = std::abs(dx) <= dy && ((d2 >= 25 && d2 <= 49) || (online && d2 >= 100 && d2 <= 144));
        if (d2 <= 4 || arc) g_.pixel(cx + dx, cy - dy, true);
      }
    if (!online)
      for (int i = 0; i < 5; i++) {
        g_.pixel(WIFI_X + 12 + i, STATUS_Y + i, true);
        g_.pixel(WIFI_X + 16 - i, STATUS_Y + i, true);
      }
  }

  // sync, left of Wi-Fi (of the battery while asleep): a check or a crossed-out sync after Sync now in the menu, and
  // an upload arrow with a count while reviews wait on the device to be sent. showSyncing() also draws here.
  sync_end_ = (asleep ? BATT_X : WIFI_X) - 10;
  size_t pending = asleep ? 0 : log_.count();   // the sleep screen says it in words instead
  if (note == SyncNote::Ok) {
    drawIcon(g_, ICON_CHECK, 11, sync_end_, STATUS_Y + 1);
  } else if (note == SyncNote::Failed) {
    drawIcon(g_, ICON_SYNC, 14, sync_end_, STATUS_Y);
    g_.fill(sync_end_ - 6, STATUS_Y, 6, 6, false);
    for (int i = 0; i < 5; i++) {         // the same small x as offline Wi-Fi
      g_.pixel(sync_end_ - 5 + i, STATUS_Y + i, true);
      g_.pixel(sync_end_ - 1 - i, STATUS_Y + i, true);
    }
  } else if (pending) {
    drawIcon(g_, ICON_UPLOAD, 12, sync_end_, STATUS_Y + 1);
    std::string n = pending > 999 ? "999+" : std::to_string(pending);
    g_.text(FONT_SMALL, sync_end_ - 12 - 6 - g_.width(FONT_SMALL, n), STATUS_Y - 3, n);
  }

  // battery: an outline with a nub, filled from the left by charge, with a bolt across it while on USB. With no
  // gauge reading it stays an empty outline rather than claim a full battery.
  PowerInfo p = hal_.power();
  if (!p.gauge) p.pct = 0;
  g_.frame(BATT_X, STATUS_Y, 30, 14, 2);
  g_.fill(BATT_X + 30, STATUS_Y + 4, 3, 6);
  g_.fill(BATT_X + 3, STATUS_Y + 3, (int)(24 * p.pct / 100 + 0.5f), 8);
  if (p.usb)
    for (int r = 0; r < 9; r++)
      for (int c = 0; c < 6; c++)
        if (BOLT[r][c] == '#') g_.invert(BATT_X + 12 + c, STATUS_Y + 2 + r, 1, 1);   // shows on filled and empty alike
}

std::string Device::agoText(uint64_t ms) {
  uint64_t m = ms / 60000;
  if (m < 1) return "just now";
  if (m < 60) return std::to_string(m) + " min ago";
  if (m < 48 * 60) return std::to_string(m / 60) + " h ago";
  return std::to_string(m / 1440) + " days ago";
}

std::string Device::clockText(uint64_t server_ms) {
  std::time_t t = (std::time_t)((int64_t)(server_ms / 1000) + cfg_.utc_offset_min * 60);
  std::tm* d = std::gmtime(&t);
  if (!d) return "";
  char buf[16];
  snprintf(buf, sizeof buf, "%d:%02d %s", d->tm_hour % 12 ? d->tm_hour % 12 : 12, d->tm_min, d->tm_hour < 12 ? "AM" : "PM");
  return buf;
}

std::string Device::syncedText() {
  // A clock time, not "5 min ago": the sleep screen sits unchanged for hours and an age would go stale on it
  static const char* MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  RtcState& rtc = hal_.rtc();
  if (!rtc.anchor_server) return rtc.fail_streak ? "Couldn't reach the server." : "Not synced yet.";
  uint64_t last = rtc.anchor_server, now = serverNow();
  auto day = [&](uint64_t ms) { return ((int64_t)(ms / 1000) + cfg_.utc_offset_min * 60) / 86400; };
  int64_t days = day(now) - day(last);
  std::string when = clockText(last);
  if (days <= 0) {
    when = "at " + when;
  } else if (days == 1) {
    when = "yesterday at " + when;
  } else {
    std::time_t t = (std::time_t)((int64_t)(last / 1000) + cfg_.utc_offset_min * 60);
    std::tm* d = std::gmtime(&t);
    when = (d ? std::string(MONTHS[d->tm_mon]) + " " + std::to_string(d->tm_mday) + " " : std::string()) + "at " + when;
  }
  if (low_batt_) return "Battery low, syncing waits for a charge. Last synced " + when + ".";
  if (rtc.fail_streak) return "Couldn't reach the server. Last synced " + when + ".";
  return "Synced " + when + ".";
}

static const char* MENU_HINT = "Hold keys 1 and 4 together for the menu.";

std::string Device::pendingText() {
  size_t n = log_.count();
  if (!n) return "";
  return std::to_string(n) + (n == 1 ? " review" : " reviews") + " waiting to upload.";
}

void Device::drawSleep() {
  RtcState& rtc = hal_.rtc();
  g_.clear();
  drawStatus(true);
  // The count is what this device can show: picture cards are counted apart, so reviewing everything here gets to
  // "All caught up" instead of leaving a number that never goes away
  uint32_t due = rtc.anchor_server ? rtc.due_now : cardsLeft(), app = rtc.anchor_server ? rtc.due_app : 0;
  int y;
  if (due > 0) {
    std::string n = due > 999 ? "999+" : std::to_string(due);
    int right = g_.text(FONT_BIG, 40, 56, n);
    // on the digits' baseline: both lines put it at their top plus the font's ascent
    g_.text(FONT_TITLE, right + 18, 56 + FONT_BIG.ascent - FONT_TITLE.ascent, due == 1 ? "card due" : "cards due");
    y = 196;
  } else {
    g_.text(FONT_TITLE, 48, 80, "All caught up");
    g_.text(FONT_BODY, 48, 132, "Nothing is due right now.");
    y = 176;
  }
  if (app > 0) {
    g_.text(FONT_BODY, 52, y, "Plus " + std::to_string(app) + (app == 1 ? " picture card" : " picture cards") + " in the app.");
    y += 40;
  } else {
    y += 8;
  }
  // Where the device stands with the server, so it's clear whether this count (and your reviews) made it across
  std::string sync = syncedText(), pending = pendingText();
  g_.paragraph(FONT_BODY, 52, y, 700, pending.empty() ? sync : sync + " " + pending);

  // The one thing to do here, as a button-sized pill rather than a line of small print, and how to get to the rest
  const char* cta = "Press any key to start";
  int w = g_.width(FONT_TITLE, cta) + 96, h = 64, top = SCREEN_H - h - 52;
  g_.pill((SCREEN_W - w) / 2, top, w, h);
  g_.centered(FONT_TITLE, SCREEN_W / 2, top + (h - FONT_TITLE.line_h) / 2 + 1, cta, false);
  g_.centered(FONT_SMALL, SCREEN_W / 2, SCREEN_H - 34, MENU_HINT);
}

void Device::drawSetup(const std::string& error) {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Set up Wi-Fi");
  drawStatus();
  int y = g_.paragraph(FONT_BODY, 48, 104, 460, "1. On your phone, join this Wi-Fi network:");
  g_.text(FONT_BODY, 76, y, "Network");
  g_.text(FONT_BODY_B, 196, y, ap_name_);
  g_.text(FONT_BODY, 76, y + 32, "Password");
  g_.text(FONT_BODY_B, 196, y + 32, ap_pass_);   // WPA2: only someone who can see this screen can join
  y = g_.paragraph(FONT_BODY, 48, y + 76, 460, "2. A setup page opens. If it doesn't, go to 192.168.4.1 in your browser.");
  g_.paragraph(FONT_BODY, 48, y + 10, 460, "3. Pick your home network and type its password.");
  int side = g_.qr("WIFI:T:WPA;S:" + ap_name_ + ";P:" + ap_pass_ + ";;", 560, 100, 7);
  if (side) g_.centered(FONT_SMALL, 560 + side / 2, 100 + side + 12, "scan to join");
  if (!error.empty()) g_.paragraph(FONT_BODY_B, 48, 368, 700, error);
  // A freshly built device lands here first, so the hardware check is one key away before anything else
  const char* labels[4] = {"Hardware check", "", "", !ssid_.empty() && !token_.empty() ? "Cancel" : ""};
  g_.strip(labels);
}

void Device::drawLink(const std::string& note) {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Link this Rekadeck");
  drawStatus();
  if (code_.user_code.empty()) {
    g_.paragraph(FONT_BODY, 48, 110, 700, note);
  } else {
    g_.text(FONT_BODY, 48, 104, "On your phone or laptop, go to");
    g_.text(FONT_BODY_B, 48, 136, code_.verify_url);
    g_.text(FONT_BODY, 48, 184, "and enter this code:");
    g_.text(FONT_CODE, 44, 214, code_.user_code);
    int side = g_.qr(code_.verify_url + "?code=" + code_.user_code, 580, 96, 5);
    if (side) g_.centered(FONT_SMALL, 580 + side / 2, 96 + side + 10, "or scan this");
    g_.paragraph(FONT_SMALL, 48, 330, 700, note.empty() ? "Waiting for the code..." : note);
  }
  // A server other than the one this firmware was built for gets named, so a link page that isn't the one you
  // expect stands out (the address can be changed in Wi-Fi setup)
  if (server_ != REKA_DEFAULT_SERVER) g_.paragraph(FONT_SMALL, 48, 380, 700, "Linking with " + server_);
  const char* labels[4] = {"", "", "", "Wi-Fi"};
  g_.strip(labels);
}

void Device::drawConfirmUnlink() {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Unlink this Rekadeck?");
  drawStatus();
  int y = g_.paragraph(FONT_BODY, 48, 104, 700,
                       "It stops syncing with your account and forgets the cards on board. You can link it again "
                       "with a new code.");
  size_t n = log_.count();
  if (n) {
    std::string w = std::to_string(n) + (n == 1 ? " review hasn't" : " reviews haven't") +
                    " synced yet. It will try to send them first, and they're lost if it can't.";
    g_.paragraph(FONT_BODY_B, 48, y + 20, 700, w);
  }
  const char* labels[4] = {"", "", "Unlink", "Cancel"};
  g_.strip(labels);
}

void Device::drawConfirmErase() {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Erase this Rekadeck?");
  drawStatus();
  int y = g_.paragraph(FONT_BODY, 48, 104, 700,
                       "It forgets your Wi-Fi network and its password, unlinks from your account, deletes every card "
                       "on board, and starts again from Wi-Fi setup. Do this before you give the device to someone.");
  size_t n = log_.count();
  if (n) {
    std::string w = std::to_string(n) + (n == 1 ? " review hasn't" : " reviews haven't") +
                    " synced yet. It will try to send them first, and they're lost if it can't.";
    g_.paragraph(FONT_BODY_B, 48, y + 20, 700, w);
  }
  const char* labels[4] = {"", "", "Erase", "Cancel"};
  g_.strip(labels);
}

void Device::drawMenu() {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Menu");
  drawStatus(false, menu_note_);
  for (int i = 0; i < MENU_N; i++) {
    int y = MENU_Y + i * MENU_ROW;
    g_.text(FONT_BODY_B, 64, y + 7, MENU_ITEMS[i]);
    if (i == MI_SYNC) {   // where syncing stands, on the row that does something about it
      RtcState& rtc = hal_.rtc();
      std::string s = rtc.anchor_server ? "Last synced " + agoText(hal_.rtcMs() - rtc.anchor_rtc) : "Not synced yet";
      if (rtc.fail_streak) s = "Couldn't reach the server";
      if (size_t n = log_.count()) s += ", " + std::to_string(n) + " to upload";
      g_.text(FONT_BODY, 744 - g_.width(FONT_BODY, s), y + 7, s);
    }
    if (i == menu_sel_) g_.invert(40, y, 720, MENU_ROW - 4);
  }
  const char* labels[4] = {"Up", "Down", "Select", "Back"};
  g_.strip(labels);
}

void Device::drawInfo() {
  RtcState& rtc = hal_.rtc();
  PowerInfo p = hal_.power();
  char buf[128];
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Info and updates");
  drawStatus();
  int y = 96, lh = 32;
  auto line = [&](const std::string& k, std::string v) {
    g_.text(FONT_BODY, 48, y, k);
    if (g_.width(FONT_BODY_B, v) > SCREEN_W - 48 - 300) {   // an error message can run long: cut it at the margin
      while (!v.empty() && g_.width(FONT_BODY_B, v + "...") > SCREEN_W - 48 - 300) v.pop_back();
      v += "...";
    }
    g_.text(FONT_BODY_B, 300, y, v);
    y += lh;
  };
  if (p.gauge)
    snprintf(buf, sizeof buf, "%d%%, %.2f V%s", (int)(p.pct + 0.5f), p.mv / 1000.0f, p.charging ? ", charging" : (p.usb ? ", on USB" : ""));
  else
    snprintf(buf, sizeof buf, "no reading, the gauge didn't answer");
  line("Battery", buf);
  {   // where the charge goes: time awake is what costs, deep sleep should be close to free
    uint64_t awake_s = (rtc.awake_ms + hal_.millis()) / 1000;
    snprintf(buf, sizeof buf, "%lu min awake, %lu wakes, %lu on the timer", (unsigned long)(awake_s / 60),
             (unsigned long)rtc.wakes, (unsigned long)rtc.timer_wakes);
    line("Since power-on", buf);
  }
  snprintf(buf, sizeof buf, "%u held, %u due now, %u KB free", (unsigned)store_.cards().size(), (unsigned)cardsLeft(),
           (unsigned)(hal_.freeBytes() / 1024));
  line("Cards", buf);
  snprintf(buf, sizeof buf, "%u", (unsigned)log_.count());
  line("Waiting to sync", buf);
  std::string last = rtc.anchor_server ? agoText(hal_.rtcMs() - rtc.anchor_rtc) : std::string("not this boot");
  if (rtc.fail_streak) last += ", then " + std::to_string(rtc.fail_streak) + " failed";
  line("Last sync", last);
  line("Wi-Fi", ssid_ + (hal_.wifiConnected() ? ", connected" : ""));
  line("Server", server_);
  HwInfo hw = hal_.hwInfo();
  if (hw.psram_total)
    snprintf(buf, sizeof buf, "%u KB PSRAM, %u KB on-chip free", (unsigned)(hw.psram_total / 1024), (unsigned)(hw.internal_free / 1024));
  else
    snprintf(buf, sizeof buf, "no PSRAM, %u KB on-chip free", (unsigned)(hw.internal_free / 1024));
  line("Memory", buf);
  snprintf(buf, sizeof buf, "%s in %s, boot %08lx", FW_VERSION, hw.app_slot.c_str(), (unsigned long)rtc.boot_id);
  line("Firmware", buf);
  // Updates: whether they install by themselves, then the news (an offer from any check-in this wake, or how the last
  // check or install from here went)
  std::string news = update_note_.empty() && !ota_.version.empty() ? ota_.version + " available" : update_note_;
  line("Updates", std::string(auto_update_ ? "Automatic" : "Off") + (news.empty() ? "" : ", " + news));
  const char* labels[4] = {ota_.version.empty() ? "Check" : "Install", "Auto-update", "", "Back"};
  g_.strip(labels);
}

void Device::infoKey(Ev e) {
  if (e == Ev::K1 && !ota_.version.empty()) {
    if (installUpdate(true)) return;   // restarting into it
    return enterInfo();                // why not, on the Updates line
  }
  if (e == Ev::K1) {
    bool ok = syncOnScreen("Checking for updates", "checking for updates", false, 15000);
    if (mode_ != Mode::Info) return;   // the server unlinked it: the link screen stays up
    update_note_ = !ok ? "couldn't reach the server" : ota_.version.empty() ? "up to date" : "";
    if (!dl_.active) hal_.wifiOff();
    return enterInfo();
  }
  if (e == Ev::K2) {
    auto_update_ = !auto_update_;
    hal_.setSetting("autoupd", auto_update_ ? "1" : "0");
    return enterInfo();
  }
  if (e == Ev::K3 || e == Ev::K4) enterMenu();
}

const char* Device::drawDone() {
  // No card up, and the headline says why. "All caught up" only when it's true: not while more cards are on their
  // way, and not while the account has cards due that just aren't ready for the device yet.
  RtcState& rtc = hal_.rtc();
  // Due here but not on the device. Only trusted from a check-in that came after every review was sent, or it
  // counts cards reviewed since.
  uint32_t not_ready = checked_in_ && !rtc.fail_streak && log_.count() == 0 ? rtc.due_now : 0;
  const char *title, *why;
  std::string line;
  if (dl_.active) {
    title = "Getting more cards";
    why = "getting more cards";
    line = "They'll show up here in a moment.";
  } else if (dl_failed_) {
    title = "Couldn't get more cards";
    why = "download failed";
    line = "The download broke off. Press Sync to try again.";
  } else if (not_ready) {
    title = "No more cards ready";
    why = "none ready";
    line = std::to_string(not_ready) + " more due, their questions aren't written yet.";
  } else {
    title = "All caught up";
    why = "all done";
    uint64_t next = 0, now = serverNow();
    for (auto& c : store_.cards())
      if (!store_.isDone(c.id) && (!next || c.due_ms < next)) next = c.due_ms;
    if (next && now && next > now)
      line = "The next card is due in " +
             (next - now < 3600000 ? std::to_string((next - now) / 60000 + 1) + " min" : std::to_string((next - now) / 3600000 + 1) + " h") + ".";
    else
      line = "Nothing else is due right now.";
  }
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, title);
  drawStatus();
  char buf[96];
  if (session_reviews_) {
    snprintf(buf, sizeof buf, "You reviewed %u card%s this session.", (unsigned)session_reviews_, session_reviews_ == 1 ? "" : "s");
    g_.text(FONT_BODY, 48, 110, buf);
  }
  if (!line.empty()) g_.text(FONT_BODY, 48, 150, line);
  {
    std::string sync = syncedText(), pending = pendingText();
    g_.paragraph(FONT_BODY, 48, 190, 704, pending.empty() ? sync : sync + " " + pending);
  }
  if (uint32_t app = rtc.anchor_server ? rtc.due_app : 0)
    g_.text(FONT_BODY, 48, 270, std::to_string(app) + (app == 1 ? " picture card is" : " picture cards are") + " due in the app.");
  g_.text(FONT_BODY, 48, 380, MENU_HINT);
  const char* labels[4] = {dl_.active ? "" : "Sync", "", "", ""};
  g_.strip(labels);
  return why;
}

}  // namespace reka
