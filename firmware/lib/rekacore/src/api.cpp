#include "api.h"

#include <algorithm>
#include <cstdio>

#include "vendor/ArduinoJson.h"

namespace reka {


template <typename T>
static T clampTo(JsonVariantConst v, T fallback, T lo, T hi) {
  if (!v.is<long long>() && !v.is<double>()) return fallback;   // missing, or not a number
  double d = v.as<double>();
  if (d < (double)lo) return lo;
  if (d > (double)hi) return hi;
  return (T)d;
}

void Settings::parse(const std::string& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  idle_s = clampTo<uint32_t>(doc["idle_s"], idle_s, 20, 900);
  again_gap = clampTo<uint32_t>(doc["again_gap"], again_gap, 1, 50);
  clean_after_pct = clampTo<uint32_t>(doc["clean_after_pct"], clean_after_pct, 5, 60);
  checkin_every = clampTo<uint32_t>(doc["checkin_every"], checkin_every, 3, 100);
  wake_every_min = clampTo<uint32_t>(doc["wake_every_min"], wake_every_min, 30, 1440);
  batch_limit = clampTo<uint32_t>(doc["batch_limit"], batch_limit, 10, 100);   // 1 MB of flash holds two of these
  utc_offset_min = clampTo<int32_t>(doc["utc_offset_min"], utc_offset_min, -840, 840);
}

Net Api::call(const char* method, const std::string& path, const std::string& body, std::string& out, uint32_t timeout_ms) {
  if (!hal_.wifiConnected()) return Net::Offline;
  HttpResult r = hal_.http(method, server_ + path, body, token_, timeout_ms);
  if (r.status == 0) return Net::Offline;
  if (r.status == 401) return Net::Unauthorized;
  if (r.status < 200 || r.status >= 300) {
    char msg[96];
    snprintf(msg, sizeof msg, "http %s %s -> %d", method, path.c_str(), r.status);
    hal_.log(msg);
    return Net::Failed;
  }
  out = std::move(r.body);
  return Net::Ok;
}

Net Api::requestCode(LinkCode& out) {
  std::string resp;
  JsonDocument req;
  req["hw"] = "rekadeck-1";
  req["fw"] = FW_VERSION;
  req["screen"]["w"] = SCREEN_W;    // the server renders cards for whatever screen asks
  req["screen"]["h"] = SCREEN_H;
  req["screen"]["bpp"] = 1;
  std::string body;
  serializeJson(req, body);
  Net n = call("POST", "/api/device/code", body, resp);
  if (n != Net::Ok) return n;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return Net::Failed;
  out.device_code = doc["device_code"] | "";
  out.user_code = doc["user_code"] | "";
  out.verify_url = doc["verify_url"] | "";
  out.interval_s = doc["interval_s"] | 3;
  out.expires_s = doc["expires_s"] | 600;
  return out.device_code.empty() ? Net::Failed : Net::Ok;
}

Net Api::pollToken(const std::string& device_code, std::string& token, bool& expired) {
  JsonDocument req;
  req["device_code"] = device_code;
  std::string body, resp;
  serializeJson(req, body);
  Net n = call("POST", "/api/device/token", body, resp);
  if (n != Net::Ok) return n;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return Net::Failed;
  std::string status = doc["status"] | "";
  expired = status == "expired";
  token = status == "ok" ? std::string(doc["token"] | "") : std::string();
  return Net::Ok;
}

Net Api::hello(const std::string& batch_id, int batch_version, uint32_t cards_left, uint32_t unsynced, Hello& out) {
  PowerInfo p = hal_.power();
  RtcState& rtc = hal_.rtc();
  JsonDocument req;
  req["fw"] = FW_VERSION;
  if (p.gauge) {   // without a reading these are placeholders (100%, 4100 mV), which the server would store as real
    req["battery"] = p.pct;
    req["charging"] = p.charging;
    // the battery budget since power-on, so the server log shows where a day's charge went
    req["mv"] = p.mv;
  }
  req["usb"] = p.usb;
  req["awake_s"] = (uint32_t)((rtc.awake_ms + hal_.millis()) / 1000);
  req["wakes"] = rtc.wakes;
  req["timer_wakes"] = rtc.timer_wakes;
  req["fail_streak"] = rtc.fail_streak;
  req["rtc_ms"] = hal_.rtcMs();
  req["boot"] = rtc.boot_id;
  req["batch_id"] = batch_id;
  req["batch_version"] = batch_version;
  req["cards_left"] = cards_left;
  req["unsynced"] = unsynced;
  std::string body, resp;
  serializeJson(req, body);
  uint64_t sent_rtc = hal_.rtcMs();
  Net n = call("POST", "/api/device/hello", body, resp);
  if (n != Net::Ok) return n;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return Net::Failed;
  out.server_ms = doc["server_ms"] | (uint64_t)0;
  out.version = doc["version"] | 0;
  out.stale = doc["stale"] | false;
  out.due_now = doc["due_now"] | 0u;
  out.due_today = doc["due_today"] | 0u;
  // an older server doesn't send due_device: then everything due counts as showable here, as before
  out.due_device = doc["due_device"].is<unsigned>() ? std::min(doc["due_device"].as<unsigned>(), out.due_now) : out.due_now;
  out.settings_json.clear();
  serializeJson(doc["settings"], out.settings_json);
  out.fw_version.clear();
  JsonObjectConst fw = doc["firmware"];
  if (!fw.isNull() && fw["version"].is<const char*>() && fw["url"].is<const char*>() && fw["sha256"].is<const char*>()) {
    out.fw_version = fw["version"].as<const char*>();
    out.fw_url = fw["url"].as<const char*>();
    out.fw_sha256 = fw["sha256"].as<const char*>();
    out.fw_size = fw["size"] | 0u;
    if (out.fw_size == 0 || out.fw_sha256.size() != 64 || out.fw_url.empty() || out.fw_url[0] != '/') out.fw_version.clear();
  }
  rtc.anchor_rtc = sent_rtc;          // the server stamped its time when our request arrived
  rtc.anchor_server = out.server_ms;
  rtc.due_now = out.due_device;       // the sleep screen's count: what can be reviewed here...
  rtc.due_app = out.due_now - out.due_device;   // ...and what's left for the apps
  return Net::Ok;
}

Net Api::manifest(uint32_t limit, std::string& json) {
  return call("GET", "/api/device/batch?limit=" + std::to_string(limit), "", json, 15000);
}

Net Api::blob(const std::string& path, std::string& out) { return call("GET", "/api/device/" + path, "", out); }

Net Api::ping() {
  // For the hardware check on a device that isn't linked yet: any HTTP answer at all means the address is right
  if (!hal_.wifiConnected()) return Net::Offline;
  HttpResult r = hal_.http("GET", server_ + "/api/health", "", "", 5000);
  return r.status ? Net::Ok : Net::Offline;
}

Net Api::unlink() {
  std::string resp;
  return call("POST", "/api/device/unlink", "{}", resp);
}

Net Api::upload(const std::vector<Review>& reviews, std::set<uint32_t>& accepted) {
  JsonDocument req;
  req["boot"] = hal_.rtc().boot_id;
  req["rtc_ms"] = hal_.rtcMs();
  JsonArray arr = req["reviews"].to<JsonArray>();
  for (auto& r : reviews) {
    JsonObject o = arr.add<JsonObject>();
    o["rid"] = r.rid;
    o["card"] = r.card;
    o["g"] = r.grade;
    o["shown"] = r.shown;
    o["revealed"] = r.revealed;
    o["graded"] = r.graded;
    o["choice"] = r.choice;
    o["boot"] = r.boot;
  }
  std::string body, resp;
  serializeJson(req, body);
  Net n = call("POST", "/api/device/reviews", body, resp);
  if (n != Net::Ok) return n;
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return Net::Failed;
  for (JsonVariant v : doc["accepted"].as<JsonArray>()) accepted.insert(v.as<uint32_t>());
  return Net::Ok;
}

}  // namespace reka
