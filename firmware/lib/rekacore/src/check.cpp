// The hardware check, from the menu or the Wi-Fi setup screen: a guided pass over every part of a hand-built
// Rekadeck. The screen and the keys need someone to look and press, charging needs a cable plugged in, and storage,
// Wi-Fi and the server check themselves. Every result also goes to the log.
#include <algorithm>
#include <cstdio>

#include "device.h"

namespace reka {

static const char* const CHECK_NAMES[] = {"Screen", "Keys", "Battery", "Charging", "Storage", "Wi-Fi", "Server"};
static constexpr uint32_t KEYS_TIMEOUT_MS = 30000;   // a key that hasn't registered by then counts as not working
static constexpr uint32_t STUCK_MS = 4000;           // held this long without anyone pressing it: shorted
static constexpr uint32_t CHARGE_WATCH_MS = 40000;
// Plugging in at 214 mA lifts a LiPo's voltage by its internal resistance times the current: 30 mV or more on a
// small cell, nearer 10 to 15 mV on a big one, then it keeps creeping up. Three readings in a row above this count.
static constexpr int CHARGE_RISE_MV = 15;
static constexpr int FULL_MV = 4150;                 // above this the charger tapers off and there's nothing to see

void Device::setCheck(CheckPart part, CheckState state, const std::string& detail, const std::string& fix) {
  ck_.items[part].state = state;
  ck_.items[part].detail = detail;
  ck_.items[part].fix = fix;
  static const char* const WORD[] = {"not run", "ok", "problem", "skipped"};
  std::string line = "check: " + std::string(CHECK_NAMES[part]) + " " + WORD[(int)state] + ", " + detail;
  if (!fix.empty()) line += ". " + fix;
  hal_.log(line.c_str());
}

void Device::enterCheck(Mode back_to) {
  if (back_to == Mode::WifiSetup) hal_.portalStop();
  mode_ = Mode::Check;
  ck_ = decltype(ck_){};
  ck_.back = back_to;
  ck_.step = CheckStep::Intro;
  drawCheckPage("Hardware check",
                "Goes through the screen, all four keys, the battery, charging, storage, Wi-Fi and the server. It "
                "takes about a minute. Have a USB-C cable nearby for the charging step.",
                "Start", "Back");
  show(Refresh::Partial, "hardware check", true);
  hal_.rtc().screen = 3;
}

void Device::leaveCheck() {
  hal_.gaugeFast(false);
  keys_.ignoreUntilReleased();
  if (ck_.back == Mode::WifiSetup) return enterWifiSetup();
  enterMenu();
}

void Device::checkKey(Ev e) {
  switch (ck_.step) {
    case CheckStep::Intro:
      if (e == Ev::K3) checkScreen();
      else if (e == Ev::K4) leaveCheck();
      return;
    case CheckStep::Screen: {
      if (e != Ev::K3 && e != Ev::K4) return;
      char ms[24];
      snprintf(ms, sizeof ms, "%lu ms", (unsigned long)last_refresh_ms_);
      CheckItem& it = ck_.items[CK_SCREEN];
      if (it.state != CheckState::Problem) {   // a fault measured in checkScreen() outranks what the eye says
        if (e == Ev::K4)
          setCheck(CK_SCREEN, CheckState::Problem, "Picture looked wrong",
                   "Reseat the ribbon cable in the HAT's connector, close its latch, and run this again.");
        else
          setCheck(CK_SCREEN, CheckState::Ok, std::string("Refreshed in ") + ms);
      }
      // the keys page goes up with a fast refresh, which also wipes the checkerboard's ghost
      ck_.step = CheckStep::Keys;
      ck_.step_at = hal_.millis();
      ck_.raw_prev = hal_.keys() & 0x0F;   // a key still down from answering doesn't count until pressed again
      presses_ = 0;                        // nor does the press that answered
      ck_.seen = ck_.stuck = 0;
      ck_.keys_done = false;
      std::fill(ck_.down_since, ck_.down_since + 4, 0u);
      drawCheckKeys(0, 0);
      show(Refresh::Fast, "key check");
      return;
    }
    case CheckStep::Keys:
      return;   // every key is under test here, checkLoop() reads them directly
    case CheckStep::ChargeAsk:
      if (e == Ev::K3) {
        ck_.step = CheckStep::ChargeWatch;
        drawCheckPage("Charging", "Plug the USB-C cable in now. Watching the battery for up to 40 seconds...", "", "Skip");
        show(Refresh::Partial, "charge watch");
        ck_.step_at = ck_.last_read = hal_.millis();
        ck_.base_mv = ck_.peak_mv = hal_.power().mv;
        ck_.rises = 0;
        ck_.host_seen = false;
      } else if (e == Ev::K4) {
        setCheck(CK_CHARGING, CheckState::Skipped, "Skipped");
        runAutoChecks();
      }
      return;
    case CheckStep::ChargeWatch:
      if (e == Ev::K4) {
        setCheck(CK_CHARGING, CheckState::Skipped, "Skipped");
        runAutoChecks();
      }
      return;
    case CheckStep::Auto:
      return;
    case CheckStep::Results:
      if (e == Ev::K1) enterCheck(ck_.back);
      else if (e == Ev::K4) leaveCheck();
      return;
  }
}

// ------------------------------------------------------------------ screen
void Device::checkScreen() {
  ck_.step = CheckStep::Screen;
  g_.clear();
  const char* labels[4] = {"", "", "Yes", "No"};
  g_.strip(labels);
  g_.frame(0, 0, SCREEN_W, SCREEN_H, 6);   // on the panel's very edge: a shifted or cropped picture loses part of it
  g_.text(FONT_TITLE, 40, 20, "Screen");
  for (int r = 0; r < 6; r++)
    for (int c = 0; c < 18; c++)
      if ((r + c) & 1) g_.fill(40 + c * 40, 80 + r * 40, 40, 40);
  g_.paragraph(FONT_BODY, 40, 336, 720,
               "Is the black border there on all four edges, and the checkerboard crisp, with nothing missing or "
               "faded?");
  bool ok = show(Refresh::Fast, "screen check");
  // A real fast refresh takes over a second (1.8 s measured). Much quicker means the BUSY line read "done" without
  // the panel ever saying "busy", which is what a loose BUSY wire looks like.
  char ms[24];
  snprintf(ms, sizeof ms, "%lu ms", (unsigned long)last_refresh_ms_);
  // Fix texts on the results page get two lines, so each one stays under about 110 characters
  if (!ok)
    setCheck(CK_SCREEN, CheckState::Problem, "Panel never finished",
             "The panel never reported done. Check the ribbon is locked into the HAT, and the BUSY and PWR wires.");
  else if (last_refresh_ms_ < 600)
    setCheck(CK_SCREEN, CheckState::Problem, std::string("Too fast, ") + ms,
             "That quick means the BUSY signal isn't reaching the board. Check the BUSY wire.");
}

// ------------------------------------------------------------------ keys
void Device::drawCheckKeys(uint8_t seen, uint8_t stuck) {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Keys");
  drawStatus();
  g_.paragraph(FONT_BODY, 48, 104, 704,
               "Press each of the four keys once. Any key that hasn't registered after 30 seconds is marked as not "
               "working.");
  for (int i = 0; i < 4; i++) {
    int x = KEY_X[i] - 60, y = 220;
    std::string n = std::to_string(i + 1);
    if (seen & (1 << i)) {
      g_.fill(x, y, 120, 120);
      g_.centered(FONT_TITLE, KEY_X[i], y + 39, n, false);
    } else {
      g_.frame(x, y, 120, 120, 3);
      g_.centered(FONT_TITLE, KEY_X[i], y + 39, n);
    }
    if (stuck & (1 << i)) g_.centered(FONT_BODY_B, KEY_X[i], y + 132, "Stuck down");
  }
  g_.stripText("");
}

void Device::checkLoop(uint32_t now) {
  if (ck_.step == CheckStep::Keys) {
    // A new press is a key that went down since the last look, or one the board counted while the device was
    // drawing (still down from "Yes", let go and tapped again during the 1.4 s refresh: down in both looks)
    uint8_t raw = hal_.keys() & 0x0F, pressed = presses_ & 0x0F;
    presses_ = 0;
    for (int i = 0; i < 4; i++) {
      uint8_t bit = 1 << i;
      if (raw & bit) {
        if (!ck_.down_since[i]) ck_.down_since[i] = now | 1;
        if ((int32_t)(now - ck_.down_since[i]) > (int32_t)STUCK_MS) ck_.stuck |= bit;   // signed: now | 1 can be ahead
        if (!(ck_.raw_prev & bit)) pressed |= bit;
      } else {
        ck_.down_since[i] = 0;
      }
    }
    ck_.raw_prev = raw;
    uint8_t before_stuck = ck_.stuck;
    bool changed = (pressed & ~ck_.seen) != 0;
    ck_.seen |= pressed;
    if (changed || ck_.stuck != before_stuck) {
      drawCheckKeys(ck_.seen, ck_.stuck);
      show(Refresh::Partial, "key check");
    }
    if (!ck_.keys_done && ck_.seen != 0x0F && (int32_t)(now - ck_.step_at) < (int32_t)KEYS_TIMEOUT_MS) return;
    // Before the next page: every working key let go and its events through (onKey drops them during this step),
    // so the last press can't act on the next page and nothing pressed after it gets swallowed
    if (!ck_.keys_done) ck_.keys_done = true, ck_.step_at = now;
    bool settled = (raw & ~ck_.stuck) == 0 && (ck_.stuck || !keys_.busy());
    if (!settled && (int32_t)(now - ck_.step_at) < 3000) return;

    uint8_t missing = 0x0F & ~ck_.seen;
    if (!missing && !ck_.stuck) {
      setCheck(CK_KEYS, CheckState::Ok, "All four respond");
    } else {
      auto list = [](uint8_t mask) {
        std::string s;
        for (int i = 0; i < 4; i++)
          if (mask & (1 << i)) s += (s.empty() ? "" : ", ") + std::to_string(i + 1);
        return s;
      };
      std::string what;
      if (missing) what = (__builtin_popcount(missing) > 1 ? "No press from keys " : "No press from key ") + list(missing);
      if (ck_.stuck) what += (what.empty() ? "" : "; ") + std::string("stuck: ") + list(ck_.stuck);
      setCheck(CK_KEYS, CheckState::Problem, what,
               "Check that switch's solder joints and wires. A key stuck down is shorted to ground.");
    }
    checkCharging();
  } else if (ck_.step == CheckStep::ChargeWatch) {
    if ((int32_t)(now - ck_.last_read) < 500) return;
    ck_.last_read = now;
    PowerInfo p = hal_.power();
    if (p.usb || p.usb_host) ck_.host_seen = true;
    ck_.peak_mv = std::max(ck_.peak_mv, p.mv);
    ck_.rises = p.mv >= ck_.base_mv + CHARGE_RISE_MV ? ck_.rises + 1 : 0;
    char buf[64];
    if (ck_.rises >= 3 || p.charging) {
      snprintf(buf, sizeof buf, "Up %d mV with USB in", p.mv - ck_.base_mv);
      setCheck(CK_CHARGING, CheckState::Ok, buf);
      return runAutoChecks();
    }
    if ((int32_t)(now - ck_.step_at) < (int32_t)CHARGE_WATCH_MS) return;
    if (ck_.host_seen)
      setCheck(CK_CHARGING, CheckState::Problem, "USB in, battery not charging",
               "USB power arrives but the battery isn't taking it. Is it in the board's own JST socket? Is CHG lit?");
    else
      setCheck(CK_CHARGING, CheckState::Problem, "No rise in 40 s",
               "Try another cable and charger. Is the battery in the board's own JST socket? Does CHG light up?");
    runAutoChecks();
  }
}

// ------------------------------------------------------------------ battery and charging
void Device::checkCharging() {
  hal_.gaugeFast(true);   // a fresh voltage every 250 ms while this runs; leaveCheck() and sleep put it back
  PowerInfo p = hal_.power();
  char buf[64];
  if (!p.gauge) {
    setCheck(CK_BATTERY, CheckState::Problem, "Gauge didn't answer",
             "The board's own battery gauge didn't answer. Check nothing else is wired to SDA or SCL.");
    setCheck(CK_CHARGING, CheckState::Skipped, "Needs the battery gauge");
    return runAutoChecks();
  }
  if (p.mv < 3000 || p.mv > 4400) {
    snprintf(buf, sizeof buf, "Reads %.2f V", p.mv / 1000.0f);
    setCheck(CK_BATTERY, CheckState::Problem, buf,
             "Outside a LiPo's range. Check the battery is in the board's JST socket the right way round.");
  } else {
    snprintf(buf, sizeof buf, "%d%%, %.2f V", (int)(p.pct + 0.5f), p.mv / 1000.0f);
    setCheck(CK_BATTERY, CheckState::Ok, buf);
  }
  if (p.mv >= FULL_MV) {
    setCheck(CK_CHARGING, CheckState::Skipped, "Nearly full, can't tell");
    return runAutoChecks();
  }
  ck_.step = CheckStep::ChargeAsk;
  if (p.usb || p.usb_host)
    drawCheckPage("Charging",
                  "Unplug the USB-C cable, wait a moment, then press 3. If the device stops responding when you "
                  "unplug it, the battery isn't powering the board: check it's in the board's own battery socket.",
                  "Continue", "Skip");
  else
    drawCheckPage("Charging", "Have a USB-C cable ready. Press 3, then plug it in.", "Continue", "Skip");
  show(Refresh::Partial, "charging check", true);
}

// ------------------------------------------------------------------ storage, Wi-Fi, server
void Device::runAutoChecks() {
  ck_.step = CheckStep::Auto;
  drawCheckPage("Hardware check", "Checking storage, Wi-Fi and the server...", "", "");
  show(Refresh::Partial, "checking", true);
  char buf[96];

  HwInfo hw = hal_.hwInfo();
  std::string probe(4096, '\0'), back;
  for (size_t i = 0; i < probe.size(); i++) probe[i] = (char)(i * 31 + 7);
  bool stored = hw.fs && hal_.writeFile("/check.tmp", probe.data(), probe.size()) &&
                hal_.readFile("/check.tmp", back) && back == probe;
  hal_.removeFile("/check.tmp");
  // RAM rides on this row: an HTTPS connection needs about 40 KB of on-chip RAM in one piece, and without the
  // board's PSRAM the screen buffers take most of it
  if (stored && hw.internal_largest && hw.internal_largest < 40 * 1024) {
    snprintf(buf, sizeof buf, "Only %u KB of RAM in one piece", (unsigned)(hw.internal_largest / 1024));
    setCheck(CK_STORAGE, CheckState::Problem, buf,
             hw.psram_total ? "Too little RAM left for HTTPS. Note what Device info says and report it."
                            : "No PSRAM found, so HTTPS may fail. This board should be the version with 2 MB PSRAM.");
  } else if (stored) {
    snprintf(buf, sizeof buf, "%u KB free, %s", (unsigned)(hal_.freeBytes() / 1024),
             hw.psram_total ? (std::to_string(hw.psram_total / 1024) + " KB PSRAM").c_str() : "no PSRAM");
    setCheck(CK_STORAGE, CheckState::Ok, buf);
  } else {
    setCheck(CK_STORAGE, CheckState::Problem, hw.fs ? "Test file didn't read back" : "Didn't mount",
             "The flash didn't hold a test file. Flash the firmware again; if it keeps failing, the flash is faulty.");
  }

  if (ssid_.empty()) {
    setCheck(CK_WIFI, CheckState::Skipped, "Not set up yet");
  } else if (ensureWifi(12000)) {
    std::string name = ssid_.size() > 18 ? ssid_.substr(0, 17) + "..." : ssid_;
    snprintf(buf, sizeof buf, "%s, %d dBm", name.c_str(), hal_.wifiRssi());
    setCheck(CK_WIFI, CheckState::Ok, buf);
  } else {
    syncFailed("hardware check, wi-fi");   // the status line's Wi-Fi mark should agree with this page
    setCheck(CK_WIFI, CheckState::Problem, "Couldn't join",
             "Couldn't join " + ssid_ + ". Check it's up and in range, or change it in Wi-Fi setup.");
  }

  if (!hal_.wifiConnected()) {
    setCheck(CK_SERVER, CheckState::Skipped, "Needs Wi-Fi");
  } else if (token_.empty()) {
    if (api_.ping() == Net::Ok)
      setCheck(CK_SERVER, CheckState::Ok, "Answers, not linked yet");
    else
      setCheck(CK_SERVER, CheckState::Problem, "No answer",
               "Nothing answered at " + server_ + ". Is it running, and reachable from this network?");
  } else {
    Hello h;
    Net n = api_.hello(store_.batchId(), store_.version(), cardsLeft(), (uint32_t)log_.count(), h);
    if (n == Net::Ok) {
      syncSucceeded(h);
      snprintf(buf, sizeof buf, "Checked in, %u due", (unsigned)h.due_now);
      setCheck(CK_SERVER, CheckState::Ok, buf);
    } else if (n == Net::Unauthorized) {
      setCheck(CK_SERVER, CheckState::Problem, "Not linked any more",
               "The server doesn't know this device any more. Unlink it from the menu and link it again.");
    } else {
      syncFailed("hardware check");
      if (n == Net::Failed)
        setCheck(CK_SERVER, CheckState::Problem, "Answered with an error",
                 server_ + " answered with an error. Its log will say why.");
      else
        setCheck(CK_SERVER, CheckState::Problem, "No answer",
                 "Nothing answered at " + server_ + ". Is it running, and reachable from this network?");
    }
  }
  hal_.wifiOff();

  snprintf(buf, sizeof buf, "check: %u KB heap free (%u KB on-chip, largest %u KB), PSRAM %u KB, CPU %d MHz, gauge %04x",
           (unsigned)(hw.heap_free / 1024), (unsigned)(hw.internal_free / 1024), (unsigned)(hw.internal_largest / 1024),
           (unsigned)(hw.psram_total / 1024), hw.cpu_mhz, (unsigned)hw.gauge_version);
  hal_.log(buf);
  ck_.step = CheckStep::Results;
  drawCheckResults();
  show(Refresh::Partial, "check results", true);
}

// ------------------------------------------------------------------ drawing
void Device::drawCheckPage(const std::string& title, const std::string& body, const char* k3, const char* k4) {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, title);
  drawStatus();
  g_.paragraph(FONT_BODY, 48, 104, 704, body);
  const char* labels[4] = {"", "", k3, k4};
  g_.strip(labels);
}

void Device::drawCheckResults() {
  g_.clear();
  g_.text(FONT_TITLE, 48, 40, "Hardware check");
  drawStatus();
  int y = 96, problems = 0;
  const CheckItem* first = nullptr;
  for (int i = 0; i < CK_N; i++) {
    const CheckItem& it = ck_.items[i];
    g_.text(FONT_BODY_B, 48, y, CHECK_NAMES[i]);
    if (it.state == CheckState::Problem) {   // the one thing on this page that has to stand out
      int w = g_.width(FONT_BODY_B, "Problem") + 24;
      g_.pill(178, y - 3, w, 32);
      g_.text(FONT_BODY_B, 190, y, "Problem", false);
      if (!problems++) first = &it;
    } else {
      g_.text(FONT_BODY, 190, y, it.state == CheckState::Ok ? "OK" : it.state == CheckState::Skipped ? "Skipped" : "-");
    }
    g_.text(FONT_BODY, 330, y, it.detail);
    y += 36;
  }
  if (first) {   // how to fix the first problem; once it's fixed, running again shows the next
    std::string name = CHECK_NAMES[first - ck_.items];
    g_.paragraph(FONT_BODY, 48, y + 6, 704, name + ": " + first->fix);
  } else {
    g_.paragraph(FONT_BODY, 48, y + 6, 704, "Everything checked out.");
  }
  const char* labels[4] = {"Run again", "", "", "Done"};
  g_.strip(labels);
}

}  // namespace reka
