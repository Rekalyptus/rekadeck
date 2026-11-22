// The real-board side of the HAL: the e-paper panel (panel.h), LittleFS and NVS for storage, the ESP32's Wi-Fi and
// HTTP(S) client, a WPA2 soft-AP setup portal, the MAX17048 fuel gauge, over-the-air updates, and deep sleep with the
// keys and a timer as wake sources.
#pragma once
#include <Arduino.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFiClient.h>

#include <memory>
#include <string>
#include <vector>

#include "hal.h"
#include "panel.h"

class Esp32Hal : public reka::Hal {
 public:
  Esp32Hal();
  void begin();   // call first thing in setup()
  void idle();    // call at the end of every loop()

  // reka::Hal
  uint32_t millis() override;
  uint64_t rtcMs() override;
  void delayMs(uint32_t ms) override;
  uint32_t random32() override;
  uint8_t keys() override;
  uint8_t presses() override;
  uint8_t wakeKeys() override { return wake_keys_; }
  bool wokeByTimer() override { return woke_by_timer_; }
  bool display(const uint8_t* fb, const uint8_t* previous, reka::Refresh mode, const char* why) override;
  reka::PowerInfo power() override;
  void gaugeFast(bool on) override;
  bool brownedOut() override { return browned_out_; }
  void deepSleep(uint32_t timer_ms) override;
  void restart() override;
  reka::RtcState& rtc() override;
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
  int wifiRssi() override;
  void wifiOff() override;
  reka::HttpResult http(const char* method, const std::string& url, const std::string& body, const std::string& bearer,
                        uint32_t timeout_ms) override;
  void portalStart(const std::string& ap_name, const std::string& ap_pass, const std::string& default_server) override;
  bool portalPoll(reka::PortalSubmit& out) override;
  void portalStop() override;
  reka::HwInfo hwInfo() override;
  void log(const char* line) override;

 private:
  bool readGauge(uint8_t reg, uint16_t& value);
  bool writeGauge(uint8_t reg, uint16_t value);
  void panelOn();
  std::string scanNetworks();
  void startJoin(const std::string& ssid, const std::string& pass, bool use_cache);
  // The HTTP(S) connection, kept open between requests to the same server: a batch download is one TLS handshake,
  // not one per file. begin() prepares http_ for url, reusing the connection when it can.
  bool httpBegin(const std::string& url, const std::string& bearer, uint32_t timeout_ms);
  void httpClose();
  void logTlsError(const char* what);

  Panel panel_;
  Preferences prefs_;
  bool fs_ok_ = false, panel_on_ = false, browned_out_ = false;
  uint32_t latch_seen_[4] = {0, 0, 0, 0};
  uint32_t presses_seen_[4] = {0, 0, 0, 0};
  reka::PowerInfo power_;   // the last reading, reused for 400 ms
  uint32_t power_at_ = 0;

  uint8_t wake_keys_ = 0;
  bool woke_by_timer_ = false;

  std::unique_ptr<WiFiClient> net_;   // a WiFiClientSecure for https://
  std::string net_origin_;            // scheme://host:port that net_ is connected (or connecting) to
  bool net_tls_ = false;
  // Lives exactly as long as net_ and is destroyed first (httpClose(), and declaration order here): HTTPClient keeps
  // a raw pointer to net_ that a kept-alive end() doesn't clear, so it must never outlive the client it points to.
  std::unique_ptr<HTTPClient> http_;

  bool portal_ = false;
  std::string portal_server_, scan_json_;
  std::unique_ptr<WebServer> web_;
  std::unique_ptr<DNSServer> dns_;
  bool submitted_ = false;
  reka::PortalSubmit submit_;
};
