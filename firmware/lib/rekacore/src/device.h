// The Rekadeck's behaviour, written once and shared by the real board and the simulator.
//
// Life cycle, the same as an ESP32 in deep sleep: every wake is a fresh boot. setup() runs, then loop() until the
// device goes back to sleep. Anything that must survive sleep lives in RtcState, in files, or in settings.
#pragma once
#include <set>
#include <string>
#include <vector>

#include "api.h"
#include "gfx.h"
#include "hal.h"
#include "keys.h"
#include "store.h"

#ifndef REKA_DEFAULT_SERVER
#define REKA_DEFAULT_SERVER "http://192.168.1.100:8000"
#endif

namespace reka {

class Device {
 public:
  explicit Device(Hal& hal);
  void setup();
  bool loop();   // false once the device has gone into deep sleep

 private:
  enum class Mode { WifiSetup, Link, Review, Menu, Info, Done, Idle, ConfirmUnlink, Check, ConfirmErase };
  enum class View { Question, Answer, Picked };
  enum class SyncNote { None, Ok, Failed };   // how the last Sync now from the menu went

  // The hardware check (check.cpp): a guided walk through every part a builder wired up
  enum class CheckStep { Intro, Screen, Keys, ChargeAsk, ChargeWatch, Auto, Results };
  enum CheckPart { CK_SCREEN, CK_KEYS, CK_BATTERY, CK_CHARGING, CK_STORAGE, CK_WIFI, CK_SERVER, CK_N };
  enum class CheckState : uint8_t { NotRun, Ok, Problem, Skipped };
  struct CheckItem { CheckState state = CheckState::NotRun; std::string detail, fix; };   // fix: only for problems

  // flows
  void enterWifiSetup(const std::string& error = "");
  void enterLink(const std::string& error = "");
  void enterReview(Refresh first);
  void enterMenu();
  void enterInfo();
  void enterDone(Refresh mode = Refresh::Partial);
  void goToSleep(bool synced = false);   // synced: a check-in already ran this wake, don't repeat it
  void sleepNow(uint32_t timer_ms);      // every deep sleep goes through here, for the awake-time count
  void wakeAndReview(bool from_sleep_screen);
  void unauthorized();
  void unlink();
  void eraseDevice();                   // factory reset, then a fresh boot into Wi-Fi setup
  void leaveSetup();
  void chargeMe(const char* why);       // the battery can't safely run the radio: say so and sleep on keys only

  // power and updates
  bool radioAllowed();                  // false while the battery is too low for Wi-Fi's current bursts
  // True if it installed and restarted (on the ESP32 it never returns then). asked: Install on the info page, rather
  // than the automatic install at sleep; looser on power, no wait after a failure, and the outcome goes on the page.
  bool installUpdate(bool asked);
  void savePanel();                     // shown_ to flash, for the next wake's first partial refresh

  // input per mode
  void onKey(Ev e);
  void reviewKey(Ev e);
  void menuKey(Ev e);
  void infoKey(Ev e);                   // the info page: check for or install an update, auto-update on and off

  // sync
  bool ensureWifi(uint32_t timeout_ms);
  bool syncBlocking(bool download, uint32_t budget_ms);
  Net uploadAll();
  void startDownload();
  void downloadStep();
  void startWifi();
  void wifiStep();
  void syncSucceeded(const Hello& h);   // a check-in got through: settings, log line, failure count back to 0
  void syncFailed(const char* why);     // Wi-Fi or the server didn't answer
  bool syncFailedLately();              // within WIFI_RETRY_MS: don't spend more radio time on it yet

  // reviewing
  uint64_t serverNow();
  bool isDue(const CardMeta& c);
  void buildQueue();
  uint32_t cardsLeft();
  bool openCard(const std::string& id);
  void showCurrent(Refresh mode, const char* why, bool page_turn = false);
  void grade(int g, int choice);
  void advance();
  // Out of cards: send the last reviews and ask for more, on a screen that says so. A batch it starts downloading
  // carries on in the loop and replaces the done screen when it lands.
  void lookForMoreCards();
  void undo();
  void skip();
  void loadAssets();

  // screens
  // Every refresh goes through here. Partial refreshes that would change nothing are dropped. A page turn
  // (page_turn) becomes a fast refresh once partial refreshes have erased enough ink to leave visible ghosts.
  // False if the panel didn't report the refresh as done (last_refresh_ms_ has how long it took).
  bool show(Refresh mode, const char* why, bool page_turn = false);
  void drawQuestion();
  void drawPicked();
  void drawSleep();
  void drawSetup(const std::string& error);
  void drawLink(const std::string& note);
  void drawMenu();
  void drawInfo();
  const char* drawDone();   // returns which done screen it drew, for the refresh's log line
  void drawConfirmUnlink();
  void drawConfirmErase();
  // The date, then sync, Wi-Fi and battery icons along the top. Asleep the radio is off, so there's no Wi-Fi icon.
  void drawStatus(bool asleep = false, SyncNote note = SyncNote::None);
  // The whole screen: a big sync icon, the title, and that the keys are back in a moment. Up for every sync the keys
  // have to wait for, so nobody presses at a screen that has stopped answering.
  void showSyncing(const char* title, const char* why);
  // showSyncing(), then syncBlocking(); presses made at the frozen screen are dropped rather than acted on after
  bool syncOnScreen(const char* title, const char* why, bool download, uint32_t budget_ms);
  void toast(const std::string& text);
  std::string agoText(uint64_t ms);
  std::string clockText(uint64_t server_ms);   // "9:41 PM" in the account's time zone
  std::string syncedText();    // "Synced at 9:41 PM", stays true while the sleep screen sits there for hours
  std::string pendingText();   // "3 reviews waiting to upload", or "" when there are none

  // hardware check (check.cpp)
  void enterCheck(Mode back_to);
  void checkKey(Ev e);
  void checkLoop(uint32_t now);
  void checkScreen();
  void checkCharging();
  void runAutoChecks();
  void leaveCheck();
  void drawCheckKeys(uint8_t seen, uint8_t stuck);
  void drawCheckPage(const std::string& title, const std::string& body, const char* k3, const char* k4);
  void drawCheckResults();
  void setCheck(CheckPart part, CheckState state, const std::string& detail, const std::string& fix = "");

  Hal& hal_;
  Gfx g_;
  Keys keys_;
  CardStore store_;
  ReviewLog log_;
  Api api_;
  Settings cfg_;
  std::string ssid_, pass_, server_, token_, ap_name_, ap_pass_;

  Mode mode_ = Mode::Review, before_menu_ = Mode::Review;
  View view_ = View::Question;
  bool sleeping_ = false, swapped_ = false, checkin_due_ = false, wifi_pending_ = false;
  bool shown_valid_ = false, force_full_ = false, panel_dirty_ = false;
  // shown_: what the panel shows now. Partial refreshes need it, and it's saved to /panel.fb at sleep so the next wake
  // knows it too (the panel keeps its picture with the power off). The board layer used to keep a second copy.
  std::vector<uint8_t> frame_, scratch_, shown_, strip_question_, strip_choice_;
  bool low_batt_ = false, ota_confirmed_ = false;
  bool auto_update_ = true;     // install an offered update at sleep; the "autoupd" setting, toggled on the info page
  std::string update_note_;     // how the last check or install from the info page went, "" for nothing to say
  struct { std::string version, url, sha256; uint32_t size = 0; } ota_;   // a newer firmware the server offered
  CardMeta cur_;
  std::vector<std::string> queue_;
  size_t pos_ = 0;
  struct Graded { std::string id; bool again; };
  std::vector<Graded> history_;
  std::set<std::string> session_done_;
  uint64_t shown_at_ = 0, revealed_at_ = 0;
  uint8_t presses_ = 0;   // Hal::presses() from this loop pass
  int picked_ = -1, menu_sel_ = 0, sync_end_ = 0;   // sync_end_: just right of the sync icon's place on this screen
  uint32_t session_reviews_ = 0, since_checkin_ = 0, wifi_started_ = 0, wifi_failed_at_ = 0;   // 0 = no recent failure
  uint32_t toast_until_ = 0, next_poll_ = 0, last_wifi_try_ = 0, idle_checkin_ = 0;
  uint32_t sync_failed_at_ = 0, usb_checked_ = 0, last_refresh_ms_ = 0;
  bool checked_in_ = false;   // a check-in got through during this wake
  uint32_t checked_in_at_ = 0;   // millis of the last check-in that got through, 0 for none this wake
  bool refill_asked_ = false;    // this queue ran low and a check-in went out for more cards
  bool dl_failed_ = false;       // the last batch download broke off: not "all caught up"
  LinkCode code_;
  SyncNote menu_note_ = SyncNote::None;

  struct {
    CheckStep step = CheckStep::Intro;
    Mode back = Mode::Menu;
    CheckItem items[CK_N];
    uint32_t step_at = 0;                // millis when the current step started
    uint8_t seen = 0, raw_prev = 0, stuck = 0;
    uint32_t down_since[4] = {0, 0, 0, 0};
    int base_mv = 0, peak_mv = 0, rises = 0;   // rises: readings in a row above the baseline
    bool host_seen = false;                     // USB was seen while watching (a computer, or the charge rate)
    bool keys_done = false;                     // the key test is over, waiting for the keys to be let go
    uint32_t last_read = 0;
  } ck_;

  struct Download {
    bool active = false;
    std::string manifest;
    struct File { std::string url, name, card; uint32_t crc; };
    std::vector<File> files;
    size_t next = 0;
  } dl_;
};

}  // namespace reka
