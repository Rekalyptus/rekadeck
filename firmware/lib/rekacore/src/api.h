// Client for the Rekalyptus device API (see PROTOCOL.md). Every call is one short HTTP request.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "hal.h"
#include "store.h"

namespace reka {

enum class Net : uint8_t { Ok, Offline, Unauthorized, Failed };

struct LinkCode {
  std::string device_code, user_code, verify_url;
  uint32_t interval_s = 3, expires_s = 600;
};

struct Hello {
  uint64_t server_ms = 0;
  int version = 0;
  bool stale = false;
  uint32_t due_now = 0, due_today = 0;
  uint32_t due_device = 0;       // of due_now, the ones this device can show (the rest are picture cards)
  std::string settings_json;     // cached as-is in the "cfg" setting
  std::string fw_version, fw_url, fw_sha256;   // a newer firmware on offer, fw_version empty if none
  uint32_t fw_size = 0;
};

struct Settings {
  uint32_t idle_s = 120, again_gap = 5, clean_after_pct = 15, checkin_every = 10, wake_every_min = 240, batch_limit = 60;
  int32_t utc_offset_min = 0;   // the account's time zone, for the date on screen
  // Every value is clamped to a range the device can live with (PROTOCOL.md): the server is trusted, but a typo in
  // a device's settings shouldn't wake it every minute or ask for more cards than the flash holds
  void parse(const std::string& json);
};

constexpr const char* FW_VERSION = "0.3.4";

class Api {
 public:
  Api(Hal& hal) : hal_(hal) {}
  void configure(const std::string& server, const std::string& token) {
    server_ = server;
    token_ = token;
  }
  Net requestCode(LinkCode& out);
  Net pollToken(const std::string& device_code, std::string& token, bool& expired);  // Ok + empty token = pending
  Net hello(const std::string& batch_id, int batch_version, uint32_t cards_left, uint32_t unsynced, Hello& out);
  Net manifest(uint32_t limit, std::string& json);
  Net blob(const std::string& path, std::string& out);           // "frame/<id>?rev=N" or "asset/<name>"
  Net ping();                                                    // does anything answer at the server address?
  Net upload(const std::vector<Review>& reviews, std::set<uint32_t>& accepted);
  Net unlink();                                                  // tells the server to forget this device

 private:
  Net call(const char* method, const std::string& path, const std::string& body, std::string& out, uint32_t timeout_ms = 8000);
  Hal& hal_;
  std::string server_, token_;
};

}  // namespace reka
