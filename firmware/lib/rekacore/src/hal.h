// The hardware abstraction layer. The device core only talks to the board through this interface, so the same
// core runs on the ESP32-S3 (platform_esp32) and inside the desktop simulator (sim/).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace reka {

constexpr int SCREEN_W = 800, SCREEN_H = 480, ROW_BYTES = SCREEN_W / 8, FB_BYTES = ROW_BYTES * SCREEN_H;

enum class Refresh : uint8_t { Full, Fast, Partial };

struct PowerInfo {
  bool gauge = true;      // false: the fuel gauge didn't answer, and the numbers below are guesses
  float pct = 100;        // state of charge from the MAX17048
  int mv = 4100;
  float crate = 0;        // charge rate in percent per hour, positive while charging
  bool charging = false;
  bool usb = false;       // USB power present: stay awake rather than deep sleep
  bool usb_host = false;  // a computer is talking over USB (data, which doesn't by itself prove power reaches the board)
};

// For the hardware check and Device info
struct HwInfo {
  bool gauge = false;            // the fuel gauge answered on I2C
  uint16_t gauge_version = 0;    // its VERSION register (0x001x on a MAX17048)
  bool fs = false;               // the flash file system mounted
  size_t fs_total = 0, fs_used = 0;
  uint32_t heap_free = 0;        // everything malloc can still hand out (PSRAM included)
  uint32_t internal_free = 0;    // on-chip RAM only: TLS and Wi-Fi need theirs from here
  uint32_t internal_largest = 0; // the biggest single block of it (a TLS connection wants about 40 KB at once)
  uint32_t psram_total = 0;      // 0: no PSRAM found, every buffer lives in on-chip RAM
  int cpu_mhz = 0;
  std::string app_slot;          // which app partition is running (app0 or app1)
};

struct HttpResult {
  int status = 0;         // 0 = no response (no Wi-Fi, timeout, refused)
  std::string body;       // may hold binary data
};

struct PortalSubmit {
  std::string ssid, pass, server;
};

// Survives deep sleep but not power loss (RTC_DATA_ATTR on the ESP32).
struct RtcState {
  uint32_t magic;
  uint32_t boot_id;        // new random value every power-on, tags review times with the clock they came from
  uint32_t wakes;
  uint32_t partials;       // partial refreshes since the last full one
  uint64_t anchor_rtc;     // device clock at the last check-in...
  uint64_t anchor_server;  // ...and the server's time then (0 = never synced this boot)
  uint32_t due_now;        // from the last check-in, for the sleep screen
  uint32_t screen;         // what the panel shows: 0 unknown, 1 sleep screen, 2 card, 3 other
  uint32_t ink_ppm;        // ink erased by partial refreshes since the last fast or full one, parts per million
  uint64_t last_full_rtc;  // device clock at the last slow full refresh (the deep clean)
  uint64_t awake_ms;       // time spent awake since power-on, for the battery budget
  uint32_t timer_wakes;    // wakes on the check-in timer since power-on (wakes counts all of them)
  uint32_t fail_streak;    // check-ins in a row that couldn't reach the server (0 = the last one worked)
  uint32_t due_app;        // due cards only the apps can show (pictures), from the last check-in
  uint32_t ota_fail_hash;  // a firmware version whose install failed...
  uint64_t ota_fail_rtc;   // ...and when, so it isn't retried within a day
};
constexpr uint32_t RTC_MAGIC = 0x52454b33;  // "REK3": bumped when RtcState changes, so an old layout starts over

class Hal {
 public:
  virtual ~Hal() = default;

  // time
  virtual uint32_t millis() = 0;      // since this boot or wake
  virtual uint64_t rtcMs() = 0;       // since power-on, keeps counting through deep sleep, drifts a few percent
  virtual void delayMs(uint32_t ms) = 0;
  virtual uint32_t random32() = 0;

  // keys: bit i is set while key i+1 is held down
  virtual uint8_t keys() = 0;
  // keys with a new press since the last call. A key let go and pressed again while the device was busy (a refresh
  // blocks for up to 4 s) reads as held throughout in keys(); this is how the second tap still counts.
  virtual uint8_t presses() = 0;
  virtual uint8_t wakeKeys() = 0;     // keys that woke the device from deep sleep, 0 if power-on or timer
  virtual bool wokeByTimer() = 0;

  // panel: 800 x 480, 1 bit per pixel, rows of 100 bytes, most significant bit first, 1 = black.
  // previous: what the panel shows now (a partial refresh only drives the differences), nullptr if unknown, which
  // turns a partial refresh into a fast one. Blocks until the refresh is done, then powers the panel down.
  // False if the panel never reported it was done. why: for the simulator's refresh list.
  virtual bool display(const uint8_t* fb, const uint8_t* previous, Refresh mode, const char* why = "") = 0;

  // power
  virtual PowerInfo power() = 0;
  virtual void gaugeFast(bool on) = 0;   // on: the gauge measures every 250 ms instead of dozing (charging check)
  virtual bool brownedOut() = 0;         // this boot followed a brownout: the battery sagged under load
  virtual void deepSleep(uint32_t timer_ms) = 0;  // 0 = wake on keys only. Never returns on the ESP32.
  virtual void restart() = 0;            // a fresh boot (power-on path). Never returns on the ESP32.
  virtual RtcState& rtc() = 0;

  // firmware updates: stream an app image into the spare slot, check its size and SHA-256 (hex), and make it the
  // one to boot. False with a reason in error, and the running firmware untouched, if anything is off.
  virtual bool otaInstall(const std::string& url, const std::string& bearer, uint32_t size, const std::string& sha256,
                          std::string& error) = 0;
  virtual bool otaConfirm() = 0;   // the running firmware works: cancel the bootloader's rollback. True if it was pending

  // flash files (LittleFS) and small settings (NVS)
  virtual bool readFile(const char* path, std::string& out) = 0;
  virtual bool writeFile(const char* path, const void* data, size_t n, bool append = false) = 0;
  virtual bool renameFile(const char* from, const char* to) = 0;
  virtual bool removeFile(const char* path) = 0;
  virtual void removeDir(const char* path) = 0;   // removes the files inside it
  virtual size_t freeBytes() = 0;
  virtual bool getSetting(const char* key, std::string& out) = 0;
  virtual void setSetting(const char* key, const std::string& value) = 0;
  virtual void eraseSettings() = 0;
  virtual void eraseAll() = 0;     // factory reset: every setting (Wi-Fi password, token) and every file

  // Wi-Fi and HTTP
  virtual void wifiBegin(const std::string& ssid, const std::string& pass) = 0;   // start joining, don't wait
  virtual bool wifiConnect(const std::string& ssid, const std::string& pass, uint32_t timeout_ms) = 0;
  virtual bool wifiConnected() = 0;
  virtual int wifiRssi() = 0;                     // dBm while connected, 0 otherwise
  virtual void wifiOff() = 0;
  virtual HttpResult http(const char* method, const std::string& url, const std::string& body,
                          const std::string& bearer, uint32_t timeout_ms) = 0;

  // first-time setup: a WPA2 access point (ap_pass, shown on the screen) serving PORTAL_HTML (portal_html.h) with the
  // network list at /scan
  virtual void portalStart(const std::string& ap_name, const std::string& ap_pass, const std::string& default_server) = 0;
  virtual bool portalPoll(PortalSubmit& out) = 0;   // call often, true once the form was sent
  virtual void portalStop() = 0;

  virtual HwInfo hwInfo() = 0;
  virtual void log(const char* line) = 0;
};

}  // namespace reka
