// An in-memory board for unit tests: files and settings in maps, a clock the test moves, a battery the test sets,
// and HTTP answered by a function the test supplies. Everything the device asks of it is recorded.
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "hal.h"

struct FakeHal : reka::Hal {
  // what the test controls
  uint32_t now_ms = 0;            // millis()
  uint64_t rtc_ms = 1000000;      // rtcMs()
  reka::PowerInfo pwr;            // power()
  bool timer_wake = false, browned = false, wifi_ok = true;
  uint8_t wake_keys = 0, held = 0;
  std::function<reka::HttpResult(const std::string& method, const std::string& url, const std::string& body)> server;
  bool ota_ok = true;
  std::string ota_error = "SHA-256 doesn't match";

  // what the device did
  struct Call { std::string method, url, body; };
  std::vector<Call> calls;
  std::vector<std::string> logs;
  int refreshes = 0, wifi_joins = 0;
  bool slept = false, restarted = false, erased = false;
  int64_t sleep_timer = -1;
  struct Ota { std::string url, bearer, sha; uint32_t size; };
  std::vector<Ota> ota_calls;

  std::map<std::string, std::string> files, settings;
  reka::RtcState state{};
  bool connected = false;
  size_t fs_bytes = 1024 * 1024;

  uint32_t millis() override { return now_ms; }
  uint64_t rtcMs() override { return rtc_ms; }
  void delayMs(uint32_t ms) override { now_ms += ms; rtc_ms += ms; }
  uint32_t random32() override { return rng = rng * 1664525u + 1013904223u; }
  uint8_t keys() override { return held; }
  uint8_t pressed = 0;   // returned once by presses()
  uint8_t presses() override { uint8_t p = pressed; pressed = 0; return p; }
  uint8_t wakeKeys() override { return wake_keys; }
  bool wokeByTimer() override { return timer_wake; }
  std::vector<std::string> screens;   // each refresh's reason, in order
  std::vector<uint8_t> last_fb;       // the picture of the last refresh
  bool display(const uint8_t* fb, const uint8_t*, reka::Refresh, const char* why) override {
    refreshes++;
    screens.push_back(why ? why : "");
    last_fb.assign(fb, fb + reka::FB_BYTES);
    return true;
  }
  reka::PowerInfo power() override { return pwr; }
  void gaugeFast(bool) override {}
  bool brownedOut() override { return browned; }
  void deepSleep(uint32_t timer_ms) override { slept = true; sleep_timer = timer_ms; }
  void restart() override { restarted = true; }
  reka::RtcState& rtc() override { return state; }
  bool otaInstall(const std::string& url, const std::string& bearer, uint32_t size, const std::string& sha256,
                  std::string& error) override {
    ota_calls.push_back({url, bearer, sha256, size});
    if (!ota_ok) error = ota_error;
    return ota_ok;
  }
  bool otaConfirm() override { return false; }
  bool readFile(const char* path, std::string& out) override {
    auto it = files.find(path);
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool writeFile(const char* path, const void* data, size_t n, bool append) override {
    std::string s((const char*)data, n);
    if (append) files[path] += s; else files[path] = s;
    return true;
  }
  bool renameFile(const char* from, const char* to) override {
    auto it = files.find(from);
    if (it == files.end()) return false;
    files[to] = it->second;
    files.erase(it);
    return true;
  }
  bool removeFile(const char* path) override { return files.erase(path) > 0; }
  void removeDir(const char* path) override {
    std::string prefix = std::string(path) + "/";
    for (auto it = files.begin(); it != files.end();) it = it->first.rfind(prefix, 0) == 0 ? files.erase(it) : std::next(it);
  }
  size_t freeBytes() override {
    size_t used = 0;
    for (auto& f : files) used += (f.second.size() + 4095) / 4096 * 4096;
    return used >= fs_bytes ? 0 : fs_bytes - used;
  }
  bool getSetting(const char* key, std::string& out) override {
    auto it = settings.find(key);
    if (it == settings.end()) return false;
    out = it->second;
    return true;
  }
  void setSetting(const char* key, const std::string& value) override { settings[key] = value; }
  void eraseSettings() override { settings.clear(); }
  void eraseAll() override { settings.clear(); files.clear(); erased = true; }
  void wifiBegin(const std::string&, const std::string&) override { wifi_joins++; connected = wifi_ok; }
  bool wifiConnect(const std::string&, const std::string&, uint32_t) override {
    wifi_joins++;
    connected = wifi_ok;
    return connected;
  }
  bool wifiConnected() override { return connected; }
  int wifiRssi() override { return connected ? -60 : 0; }
  void wifiOff() override { connected = false; }
  reka::HttpResult http(const char* method, const std::string& url, const std::string& body, const std::string&,
                        uint32_t) override {
    calls.push_back({method, url, body});
    now_ms += 50;
    rtc_ms += 50;
    return server ? server(method, url, body) : reka::HttpResult{};
  }
  void portalStart(const std::string&, const std::string&, const std::string&) override {}
  bool portalPoll(reka::PortalSubmit&) override { return false; }
  void portalStop() override {}
  reka::HwInfo hwInfo() override {
    reka::HwInfo h;
    h.gauge = h.fs = true;
    h.psram_total = 2048 * 1024;
    h.internal_free = 160 * 1024;
    h.internal_largest = 100 * 1024;
    h.app_slot = "app0";
    return h;
  }
  void log(const char* line) override { logs.push_back(line); }

  // helpers for tests
  int count(const std::string& path_part) const {
    int n = 0;
    for (auto& c : calls) n += c.url.find(path_part) != std::string::npos;
    return n;
  }
  bool logged(const std::string& part) const {
    for (auto& l : logs)
      if (l.find(part) != std::string::npos) return true;
    return false;
  }
  uint32_t rng = 12345;
};
