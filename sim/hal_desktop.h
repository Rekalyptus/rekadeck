// The desktop side of the HAL: a simulated panel, keys, battery, Wi-Fi, flash and deep sleep, all driven from the
// browser page in ui.html. Thread-safe: the device runs on its own thread, the web server reads and pokes this.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "hal.h"

class DesktopHal : public reka::Hal {
 public:
  // fast: partial refreshes and Wi-Fi joins take a tenth of their real time (for automated tests)
  explicit DesktopHal(std::string data_dir, bool fast = false);

  // reka::Hal
  uint32_t millis() override;
  uint64_t rtcMs() override;
  void delayMs(uint32_t ms) override;
  uint32_t random32() override;
  uint8_t keys() override;
  uint8_t presses() override;
  uint8_t wakeKeys() override;
  bool wokeByTimer() override;
  bool display(const uint8_t* fb, const uint8_t* previous, reka::Refresh mode, const char* why) override;
  reka::PowerInfo power() override;
  void gaugeFast(bool) override {}
  bool brownedOut() override;
  void deepSleep(uint32_t timer_ms) override;
  void restart() override;
  reka::RtcState& rtc() override { return rtc_; }
  bool otaInstall(const std::string& url, const std::string& bearer, uint32_t size, const std::string& sha256,
                  std::string& error) override;
  bool otaConfirm() override;
  bool readFile(const char* path, std::string& out) override;
  bool writeFile(const char* path, const void* data, size_t n, bool append) override;
  bool renameFile(const char* from, const char* to) override;
  bool removeFile(const char* path) override;
  void removeDir(const char* path) override;
  size_t freeBytes() override;
  bool getSetting(const char* key, std::string& out) override;
  void setSetting(const char* key, const std::string& value) override;
  void eraseSettings() override;
  void eraseAll() override;
  void wifiBegin(const std::string& ssid, const std::string& pass) override;
  bool wifiConnect(const std::string& ssid, const std::string& pass, uint32_t timeout_ms) override;
  bool wifiConnected() override;
  int wifiRssi() override { return wifiConnected() ? -58 : 0; }
  void wifiOff() override;
  reka::HttpResult http(const char* method, const std::string& url, const std::string& body, const std::string& bearer,
                        uint32_t timeout_ms) override;
  void portalStart(const std::string& ap_name, const std::string& ap_pass, const std::string& default_server) override;
  bool portalPoll(reka::PortalSubmit& out) override;
  void portalStop() override;
  reka::HwInfo hwInfo() override;
  void log(const char* line) override;

  // driven by the device thread in main.cpp
  void beginBoot(bool power_on);
  bool waitForWake();          // blocks while asleep; true on a normal wake, false if a reset was requested
  bool resetRequested() const { return reset_ != 0; }
  void performReset();         // pulls the battery (1), factory-resets (2), or restarts (3: what the firmware asked for)

  // driven by the web page
  void keyEvent(int k, bool down);
  std::string stateJson();
  std::vector<uint8_t> screen(bool previous);
  std::vector<uint8_t> frame(uint64_t seq);     // a recent refresh's picture, for the page to replay in order
  std::string refreshesJson(uint64_t after);
  std::string logJson(uint64_t after);
  void control(const std::string& json);
  bool portalActive();
  std::string portalPage();
  std::string scanJson();
  void portalSubmit(const reka::PortalSubmit& s);

  static constexpr size_t FLASH_BYTES = 1024 * 1024;   // the LittleFS partition in firmware/partitions.csv

 private:
  uint64_t realMs();
  uint64_t virtualMs() { return realMs() + skip_ms_; }
  void updateBattery();
  std::string fsPath(const char* p);
  void loadSettings();
  void saveSettings();

  std::mutex m_;
  std::string dir_;
  std::mt19937 rng_{std::random_device{}()};
  reka::RtcState rtc_{};

  // time
  uint64_t start_real_;
  int64_t skip_ms_ = 0;
  uint64_t boot_virtual_ = 0;
  double rtc_acc_ = 0, drift_ = 0.02;
  uint64_t rtc_last_real_ = 0;

  // keys
  uint8_t held_ = 0, latched_ = 0;
  uint64_t latch_seen_[4] = {0, 0, 0, 0};
  uint32_t press_count_[4] = {0, 0, 0, 0}, press_seen_[4] = {0, 0, 0, 0};

  // panel
  std::vector<uint8_t> screen_, prev_screen_;
  uint64_t seq_ = 0, refresh_until_ = 0, partials_ = 0, fulls_ = 0;
  reka::Refresh last_mode_ = reka::Refresh::Full;
  struct RefreshEvent {
    uint64_t seq, uptime_ms;
    reka::Refresh mode;
    std::string why;
    uint32_t ms, wave_ms, changed, cleared;   // cleared: pixels that went from black to white (they leave ghosts)
    std::vector<uint8_t> fb;
  };
  std::deque<RefreshEvent> events_;
  uint32_t wake_refreshes_ = 0, wake_panel_ms_ = 0, wake_by_mode_[3] = {0, 0, 0};
  double ink_cleared_ = 0;   // share of the panel cleared by partial refreshes since the last fast or full one

  // power
  double battery_ = 82.0;
  uint64_t batt_last_virtual_ = 0;
  bool usb_ = false;
  bool brownout_next_ = false, browned_out_ = false;   // a test switch: the next boot reports a brownout
  bool fast_ = false;

  // firmware: the version "installed" by the last update, and whether it has confirmed itself
  std::string installed_fw_;
  bool ota_pending_ = false;

  // sleep
  bool asleep_ = false, woke_by_timer_ = false;
  uint64_t wake_at_ = 0;
  uint8_t wake_keys_ = 0;
  std::atomic<int> reset_{0};

  // network
  bool router_ = true, wifi_ = false;
  std::string wifi_ssid_, join_ssid_, join_pass_;
  uint64_t join_at_ = 0;
  std::map<std::string, std::string> networks_;

  // setup portal
  bool portal_ = false;
  std::string portal_ap_, portal_server_;
  std::optional<reka::PortalSubmit> submitted_;
  bool skip_setup_ = true;   // fill in the setup page and approve the link code without anyone doing it by hand
  std::string link_code_;    // the user code the device last got, for skip_setup_ to approve

  std::map<std::string, std::string> settings_;
  std::deque<std::pair<uint64_t, std::string>> log_;
  uint64_t log_seq_ = 0;
};
