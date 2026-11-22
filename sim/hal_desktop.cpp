#include "hal_desktop.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "portal_html.h"
#include "sha256.h"
#include "vendor/ArduinoJson.h"
#include "vendor/httplib.h"

namespace fs = std::filesystem;
using reka::Refresh;

static uint64_t steadyMs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

DesktopHal::DesktopHal(std::string data_dir, bool fast) : dir_(std::move(data_dir)) {
  fast_ = fast;
  start_real_ = steadyMs();
  rtc_last_real_ = realMs();
  batt_last_virtual_ = virtualMs();
  screen_.assign(reka::FB_BYTES, 0);
  prev_screen_.assign(reka::FB_BYTES, 0);
  fs::create_directories(dir_ + "/fs");
  networks_ = {{"Home WiFi", "flashcards"}, {"Mayo Guest", ""}, {"Neighbor 5G", "x7kP9qrt"}};
  loadSettings();
}

uint64_t DesktopHal::realMs() { return steadyMs() - start_real_; }

// ------------------------------------------------------------------ time
uint32_t DesktopHal::millis() {
  std::lock_guard<std::mutex> l(m_);
  return (uint32_t)(virtualMs() - boot_virtual_);
}

uint64_t DesktopHal::rtcMs() {
  // runs in real time (time skips don't move it), stretched by the drift setting like a cheap RC oscillator
  std::lock_guard<std::mutex> l(m_);
  uint64_t now = realMs();
  rtc_acc_ += (double)(now - rtc_last_real_) * (1.0 + drift_);
  rtc_last_real_ = now;
  return (uint64_t)rtc_acc_;
}

void DesktopHal::delayMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

uint32_t DesktopHal::random32() {
  std::lock_guard<std::mutex> l(m_);
  return rng_();
}

// ------------------------------------------------------------------ keys
uint8_t DesktopHal::keys() {
  // A tap that starts and ends while the device is blocked (a refresh, a sync) still shows up as a short press.
  std::lock_guard<std::mutex> l(m_);
  uint64_t now = realMs();
  for (int i = 0; i < 4; i++) {
    uint8_t bit = 1 << i;
    if (!(latched_ & bit) || (held_ & bit)) continue;
    if (!latch_seen_[i]) latch_seen_[i] = now;
    else if (now - latch_seen_[i] > 40) latched_ &= ~bit;
  }
  return held_ | latched_;
}

uint8_t DesktopHal::presses() {
  std::lock_guard<std::mutex> l(m_);
  uint8_t out = 0;
  for (int i = 0; i < 4; i++) {
    if (press_count_[i] != press_seen_[i]) out |= 1 << i;
    press_seen_[i] = press_count_[i];
  }
  return out;
}

uint8_t DesktopHal::wakeKeys() {
  std::lock_guard<std::mutex> l(m_);
  return wake_keys_;
}

bool DesktopHal::wokeByTimer() {
  std::lock_guard<std::mutex> l(m_);
  return woke_by_timer_;
}

void DesktopHal::keyEvent(int k, bool down) {
  if (k < 1 || k > 4) return;
  std::lock_guard<std::mutex> l(m_);
  uint8_t bit = 1 << (k - 1);
  if (down) {
    held_ |= bit;
    latched_ |= bit;
    latch_seen_[k - 1] = 0;
    press_count_[k - 1]++;
  } else {
    held_ &= ~bit;
  }
}

// ------------------------------------------------------------------ panel
bool DesktopHal::display(const uint8_t* fb, const uint8_t* /*previous*/, Refresh mode, const char* why) {
  // Timings of the first real board (Waveshare 7.5" V2, UC8179), measured over the whole refresh on 2026-11-16:
  // full 2.9 s, fast 1.8 s, partial 0.75 s (medians of 24 to 41 refreshes each). Of that, about 0.3 s comes before
  // the waveform (reset, powering up the booster, 96 KB over SPI) and 40 ms after it (powering down). The page only
  // animates the waveform, the part you can see. These replace GxEPD2's published figures (full 4 s), which this
  // panel beats comfortably at room temperature.
  const uint32_t before = 300, after = 40;
  uint32_t wave = mode == Refresh::Full ? 2555 : mode == Refresh::Fast ? 1475 : 405;
  uint32_t ms = before + wave + after;
  // --fast speeds up partial refreshes (the frequent ones) but not fast and full ones: those are rare, and the
  // hardware check rightly calls a sub-second one a panel that never reported busy
  if (fast_ && mode == Refresh::Partial) ms /= 10;
  {
    std::lock_guard<std::mutex> l(m_);
    uint32_t changed = 0, cleared = 0;
    for (size_t i = 0; i < reka::FB_BYTES; i++) {
      uint8_t was = screen_[i], now = fb[i];
      changed += __builtin_popcount((unsigned)(was ^ now));
      cleared += __builtin_popcount((unsigned)(was & ~now & 0xFF));
    }
    prev_screen_ = screen_;
    screen_.assign(fb, fb + reka::FB_BYTES);
    seq_++;
    last_mode_ = mode;
    refresh_until_ = realMs() + ms;
    if (mode == Refresh::Partial) partials_++;
    else { partials_ = 0; fulls_++; }

    RefreshEvent ev{seq_, virtualMs() - boot_virtual_, mode, why ? why : "", ms, wave, changed, cleared, screen_};
    events_.push_back(std::move(ev));
    while (events_.size() > 64) events_.pop_front();
    wake_refreshes_++;
    wake_panel_ms_ += ms;
    wake_by_mode_[(int)mode]++;
    if (mode == Refresh::Partial) ink_cleared_ += (double)cleared / (reka::SCREEN_W * reka::SCREEN_H);
    else ink_cleared_ = 0;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return true;
}

std::vector<uint8_t> DesktopHal::frame(uint64_t seq) {
  std::lock_guard<std::mutex> l(m_);
  for (auto& e : events_)
    if (e.seq == seq) return e.fb;
  return screen_;
}

std::string DesktopHal::refreshesJson(uint64_t after) {
  std::lock_guard<std::mutex> l(m_);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : events_) {
    if (e.seq <= after) continue;
    JsonObject o = arr.add<JsonObject>();
    o["seq"] = e.seq;
    o["mode"] = e.mode == Refresh::Full ? "full" : e.mode == Refresh::Fast ? "fast" : "partial";
    o["why"] = e.why;
    o["ms"] = e.ms;
    o["wave_ms"] = e.wave_ms;
    o["changed"] = e.changed;
    o["cleared"] = e.cleared;
    o["at_ms"] = e.uptime_ms;
  }
  std::string s;
  serializeJson(doc, s);
  return s;
}

std::vector<uint8_t> DesktopHal::screen(bool previous) {
  std::lock_guard<std::mutex> l(m_);
  return previous ? prev_screen_ : screen_;
}

// ------------------------------------------------------------------ power and sleep
void DesktopHal::updateBattery() {
  // caller holds m_. Rough draws: deep sleep 0.12 mA, awake 28 mA, Wi-Fi on 70 mA, charging at 214 mA.
  uint64_t now = virtualMs();
  double hours = (double)(now - batt_last_virtual_) / 3600000.0;
  batt_last_virtual_ = now;
  double ma = asleep_ ? 0.12 : (wifi_ ? 70.0 : 28.0);
  if (usb_) ma -= 214.0;
  battery_ -= ma * hours / 2500.0 * 100.0;
  battery_ = std::max(0.0, std::min(100.0, battery_));
}

reka::PowerInfo DesktopHal::power() {
  std::lock_guard<std::mutex> l(m_);
  updateBattery();
  reka::PowerInfo p;
  p.pct = (float)battery_;
  p.usb = p.usb_host = usb_;
  p.charging = usb_ && battery_ < 99.5;
  p.crate = p.charging ? 8.5f : -0.4f;
  p.mv = (int)(3300 + battery_ * 9) + (p.charging ? 60 : 0);   // charge current lifts the cell's voltage
  return p;
}

reka::HwInfo DesktopHal::hwInfo() {
  reka::HwInfo h;
  h.gauge = true;
  h.gauge_version = 0x0012;
  h.fs = true;
  h.fs_total = FLASH_BYTES;
  h.fs_used = FLASH_BYTES - freeBytes();
  h.psram_total = 2048 * 1024;   // what the Thing Plus should report
  h.internal_free = 160 * 1024;
  h.internal_largest = 110 * 1024;
  h.app_slot = "sim";
  return h;
}

void DesktopHal::deepSleep(uint32_t timer_ms) {
  std::lock_guard<std::mutex> l(m_);
  updateBattery();
  asleep_ = true;
  wifi_ = false;
  join_at_ = 0;
  portal_ = false;
  wake_at_ = timer_ms ? virtualMs() + timer_ms : 0;
  log_.emplace_back(++log_seq_, timer_ms ? "deep sleep, timer set for " + std::to_string(timer_ms / 60000) + " min"
                                         : std::string("deep sleep, keys only"));
}

void DesktopHal::beginBoot(bool power_on) {
  std::lock_guard<std::mutex> l(m_);
  boot_virtual_ = virtualMs();
  wake_refreshes_ = wake_panel_ms_ = 0;
  wake_by_mode_[0] = wake_by_mode_[1] = wake_by_mode_[2] = 0;
  wifi_ = false;
  if (power_on) {
    wake_keys_ = 0;
    woke_by_timer_ = false;
  }
  browned_out_ = power_on && brownout_next_;   // a brownout resets the chip, so it shows up as a power-on
  brownout_next_ = false;
}

bool DesktopHal::brownedOut() {
  std::lock_guard<std::mutex> l(m_);
  return browned_out_;
}

void DesktopHal::restart() {
  std::lock_guard<std::mutex> l(m_);
  reset_ = 3;   // the device thread stops and boots again (the caller marks itself asleep so loop() returns)
}

bool DesktopHal::otaInstall(const std::string& url, const std::string& bearer, uint32_t size, const std::string& sha256,
                            std::string& error) {
  // Download it from the (mock) server like the board would, and check it the same way. Nothing gets "flashed":
  // the simulator records the version so a test can see the restart came from an update.
  reka::HttpResult r = http("GET", url, "", bearer, 15000);
  if (r.status != 200) { error = "download answered " + std::to_string(r.status); return false; }
  if (r.body.size() != size) {
    error = "download is " + std::to_string(r.body.size()) + " bytes, expected " + std::to_string(size);
    return false;
  }
  std::string want = sha256;
  for (auto& c : want) c = (char)tolower(c);
  if (sha256Hex(r.body) != want) { error = "SHA-256 doesn't match"; return false; }
  std::lock_guard<std::mutex> l(m_);
  installed_fw_ = url.substr(url.rfind('/') + 1);
  ota_pending_ = true;
  log_.emplace_back(++log_seq_, "sim: firmware " + installed_fw_ + " written, " + std::to_string(size) + " bytes");
  return true;
}

bool DesktopHal::otaConfirm() {
  std::lock_guard<std::mutex> l(m_);
  bool was = ota_pending_;
  ota_pending_ = false;
  return was;
}

bool DesktopHal::waitForWake() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    std::lock_guard<std::mutex> l(m_);
    if (reset_) return false;
    if (held_) {
      wake_keys_ = held_;
      woke_by_timer_ = false;
      break;
    }
    if (wake_at_ && virtualMs() >= wake_at_) {
      wake_keys_ = 0;
      woke_by_timer_ = true;
      break;
    }
  }
  std::lock_guard<std::mutex> l(m_);
  updateBattery();
  asleep_ = false;
  wake_at_ = 0;
  log_.emplace_back(++log_seq_, woke_by_timer_ ? "woke on the timer" : "woke on a key");
  return true;
}

void DesktopHal::performReset() {
  std::lock_guard<std::mutex> l(m_);
  int kind = reset_.exchange(0);
  rtc_ = reka::RtcState{};
  rtc_acc_ = 0;
  rtc_last_real_ = realMs();
  asleep_ = false;
  wifi_ = false;
  portal_ = false;
  wake_at_ = 0;
  if (kind == 2) {
    fs::remove_all(dir_ + "/fs");
    fs::create_directories(dir_ + "/fs");
    settings_.clear();
    saveSettings();
    std::fill(screen_.begin(), screen_.end(), 0);
    seq_++;
  }
  log_.emplace_back(++log_seq_, kind == 2 ? "factory reset, powering on"
                                : kind == 3 ? "restarting" : "battery pulled, powering on");
}

// ------------------------------------------------------------------ flash
std::string DesktopHal::fsPath(const char* p) { return dir_ + "/fs" + (p[0] == '/' ? "" : "/") + p; }

bool DesktopHal::readFile(const char* path, std::string& out) {
  std::ifstream f(fsPath(path), std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), {});
  return true;
}

bool DesktopHal::writeFile(const char* path, const void* data, size_t n, bool append) {
  std::string p = fsPath(path);
  fs::create_directories(fs::path(p).parent_path());
  std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
  if (!f) return false;
  f.write((const char*)data, (std::streamsize)n);
  return (bool)f;
}

bool DesktopHal::renameFile(const char* from, const char* to) {
  std::error_code ec;
  fs::rename(fsPath(from), fsPath(to), ec);
  return !ec;
}

bool DesktopHal::removeFile(const char* path) {
  std::error_code ec;
  return fs::remove(fsPath(path), ec);
}

void DesktopHal::removeDir(const char* path) {
  std::error_code ec;
  fs::remove_all(fsPath(path), ec);
}

size_t DesktopHal::freeBytes() {
  size_t used = 0;
  std::error_code ec;
  for (auto& e : fs::recursive_directory_iterator(dir_ + "/fs", ec))
    if (e.is_regular_file()) used += (size_t)e.file_size();
  return used >= FLASH_BYTES ? 0 : FLASH_BYTES - used;
}

void DesktopHal::loadSettings() {
  std::ifstream f(dir_ + "/settings.json");
  if (!f) return;
  std::string s((std::istreambuf_iterator<char>(f)), {});
  JsonDocument doc;
  if (deserializeJson(doc, s)) return;
  for (JsonPair kv : doc.as<JsonObject>()) settings_[kv.key().c_str()] = kv.value().as<std::string>();
}

void DesktopHal::saveSettings() {
  JsonDocument doc;
  for (auto& kv : settings_) doc[kv.first] = kv.second;
  std::string s;
  serializeJsonPretty(doc, s);
  std::ofstream(dir_ + "/settings.json") << s;
}

bool DesktopHal::getSetting(const char* key, std::string& out) {
  std::lock_guard<std::mutex> l(m_);
  auto it = settings_.find(key);
  if (it == settings_.end()) return false;
  out = it->second;
  return true;
}

void DesktopHal::setSetting(const char* key, const std::string& value) {
  std::lock_guard<std::mutex> l(m_);
  settings_[key] = value;
  saveSettings();
}

void DesktopHal::eraseSettings() {
  std::lock_guard<std::mutex> l(m_);
  settings_.clear();
  saveSettings();
}

void DesktopHal::eraseAll() {
  std::lock_guard<std::mutex> l(m_);
  std::error_code ec;
  fs::remove_all(dir_ + "/fs", ec);
  fs::create_directories(dir_ + "/fs");
  settings_.clear();
  saveSettings();
}

// ------------------------------------------------------------------ Wi-Fi and HTTP
// An ESP32 waking from deep sleep takes about 1.5 s to join and get an address from DHCP.
static const uint32_t JOIN_MS = 1500;   // a tenth of it in --fast mode

void DesktopHal::wifiBegin(const std::string& ssid, const std::string& pass) {
  std::lock_guard<std::mutex> l(m_);
  join_ssid_ = ssid;
  join_pass_ = pass;
  join_at_ = realMs() + (fast_ ? JOIN_MS / 10 : JOIN_MS);
}

bool DesktopHal::wifiConnect(const std::string& ssid, const std::string& pass, uint32_t timeout_ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(fast_ ? JOIN_MS / 10 : JOIN_MS));
  bool ok;
  {
    std::lock_guard<std::mutex> l(m_);
    auto it = networks_.find(ssid);
    ok = router_ && it != networks_.end() && (it->second.empty() || it->second == pass);
    if (ok) {
      wifi_ = true;
      wifi_ssid_ = ssid;
    }
  }
  if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint32_t>(timeout_ms, 2500)));
  return ok;
}

bool DesktopHal::wifiConnected() {
  std::lock_guard<std::mutex> l(m_);
  if (join_at_ && realMs() >= join_at_) {   // a background join finishing
    join_at_ = 0;
    auto it = networks_.find(join_ssid_);
    if (router_ && it != networks_.end() && (it->second.empty() || it->second == join_pass_)) {
      wifi_ = true;
      wifi_ssid_ = join_ssid_;
    }
  }
  return wifi_ && router_;
}

void DesktopHal::wifiOff() {
  std::lock_guard<std::mutex> l(m_);
  wifi_ = false;
  join_at_ = 0;
}

reka::HttpResult DesktopHal::http(const char* method, const std::string& url, const std::string& body,
                                  const std::string& bearer, uint32_t timeout_ms) {
  reka::HttpResult out;
  if (!wifiConnected()) return out;
  // http://host:port/path
  std::string rest = url.rfind("http://", 0) == 0 ? url.substr(7) : url;
  size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
  std::string host = hostport;
  int port = 80;
  size_t colon = hostport.find(':');
  if (colon != std::string::npos) {
    host = hostport.substr(0, colon);
    port = std::atoi(hostport.c_str() + colon + 1);
  }
  httplib::Client cli(host, port);
  cli.set_connection_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
  cli.set_read_timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
  std::string approve;
  {
    std::lock_guard<std::mutex> l(m_);
    if (skip_setup_ && path == "/api/device/token") approve.swap(link_code_);
  }
  if (!approve.empty()) {   // type the code into the server's link page, as you would on the dashboard
    cli.Post("/link", httplib::Params{{"code", approve}, {"name", "Rekadeck simulator"}});
    log(("sim: approved link code " + approve + " on the server").c_str());
  }
  httplib::Headers h;
  if (!bearer.empty()) h.emplace("Authorization", "Bearer " + bearer);
  httplib::Result r = std::string(method) == "POST" ? cli.Post(path, h, body, "application/json") : cli.Get(path, h);
  if (!r) return out;
  out.status = r->status;
  out.body = r->body;
  if (path == "/api/device/code" && r->status == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, r->body)) {
      std::lock_guard<std::mutex> l(m_);
      link_code_ = doc["user_code"] | "";
    }
  }
  return out;
}

// ------------------------------------------------------------------ setup portal
void DesktopHal::portalStart(const std::string& ap_name, const std::string& /*ap_pass*/, const std::string& default_server) {
  std::lock_guard<std::mutex> l(m_);
  portal_ = true;
  portal_ap_ = ap_name;
  portal_server_ = default_server;
  submitted_.reset();
}

bool DesktopHal::portalPoll(reka::PortalSubmit& out) {
  std::lock_guard<std::mutex> l(m_);
  if (!submitted_ && skip_setup_ && portal_ && router_)   // send the form as if from a phone
    submitted_ = reka::PortalSubmit{"Home WiFi", networks_["Home WiFi"], portal_server_};
  if (!submitted_) return false;
  out = *submitted_;
  submitted_.reset();
  return true;
}

void DesktopHal::portalStop() {
  std::lock_guard<std::mutex> l(m_);
  portal_ = false;
}

bool DesktopHal::portalActive() {
  std::lock_guard<std::mutex> l(m_);
  return portal_;
}

std::string DesktopHal::portalPage() {
  std::lock_guard<std::mutex> l(m_);
  std::string html = reka::PORTAL_HTML;
  size_t at = html.find("{{server}}");
  if (at != std::string::npos) html.replace(at, 10, portal_server_);
  return html;
}

std::string DesktopHal::scanJson() {
  std::lock_guard<std::mutex> l(m_);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  if (router_)
    for (auto& n : networks_) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = n.first;
      o["secure"] = !n.second.empty();
    }
  std::string s;
  serializeJson(doc, s);
  return s;
}

void DesktopHal::portalSubmit(const reka::PortalSubmit& s) {
  std::lock_guard<std::mutex> l(m_);
  if (portal_) submitted_ = s;
}

// ------------------------------------------------------------------ log and the web page's view of things
void DesktopHal::log(const char* line) {
  std::lock_guard<std::mutex> l(m_);
  log_.emplace_back(++log_seq_, line);
  if (log_.size() > 400) log_.pop_front();
  std::cout << "[device] " << line << std::endl;
}

std::string DesktopHal::logJson(uint64_t after) {
  std::lock_guard<std::mutex> l(m_);
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& e : log_)
    if (e.first > after) {
      JsonObject o = arr.add<JsonObject>();
      o["n"] = e.first;
      o["t"] = e.second;
    }
  std::string s;
  serializeJson(doc, s);
  return s;
}

std::string DesktopHal::stateJson() {
  std::lock_guard<std::mutex> l(m_);
  updateBattery();
  uint64_t now = realMs();
  JsonDocument doc;
  doc["seq"] = seq_;
  doc["mode"] = last_mode_ == Refresh::Full ? "full" : last_mode_ == Refresh::Fast ? "fast" : "partial";
  doc["refreshing_ms"] = refresh_until_ > now ? refresh_until_ - now : 0;
  doc["partials"] = partials_;
  doc["asleep"] = asleep_;
  doc["wake_in_ms"] = asleep_ && wake_at_ ? (int64_t)wake_at_ - (int64_t)virtualMs() : -1;
  doc["battery"] = battery_;
  doc["usb"] = usb_;
  doc["router"] = router_;
  doc["wifi"] = wifi_ && router_;
  doc["ssid"] = wifi_ssid_;
  doc["portal"] = portal_;
  doc["skip_setup"] = skip_setup_;
  doc["ap"] = portal_ap_;
  doc["drift"] = drift_;
  doc["uptime_ms"] = virtualMs() - boot_virtual_;
  doc["rtc_ms"] = (uint64_t)rtc_acc_;
  doc["skip_ms"] = skip_ms_;
  doc["log_seq"] = log_seq_;
  doc["installed_fw"] = installed_fw_;
  JsonObject w = doc["wake"].to<JsonObject>();   // panel work since the last wake
  w["refreshes"] = wake_refreshes_;
  w["panel_ms"] = wake_panel_ms_;
  w["full"] = wake_by_mode_[(int)Refresh::Full];
  w["fast"] = wake_by_mode_[(int)Refresh::Fast];
  w["partial"] = wake_by_mode_[(int)Refresh::Partial];
  doc["ink_cleared"] = ink_cleared_;
  JsonObject nets = doc["networks"].to<JsonObject>();
  for (auto& n : networks_) nets[n.first] = n.second;
  std::string s;
  serializeJson(doc, s);
  return s;
}

void DesktopHal::control(const std::string& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  std::lock_guard<std::mutex> l(m_);
  updateBattery();
  if (doc["router"].is<bool>()) {
    router_ = doc["router"];
    if (!router_) wifi_ = false;
  }
  if (doc["usb"].is<bool>()) usb_ = doc["usb"];
  if (doc["skip_setup"].is<bool>()) skip_setup_ = doc["skip_setup"];
  if (doc["battery"].is<double>()) battery_ = doc["battery"];
  if (doc["drift"].is<double>()) {
    uint64_t now = realMs();
    rtc_acc_ += (double)(now - rtc_last_real_) * (1.0 + drift_);
    rtc_last_real_ = now;
    drift_ = doc["drift"];
  }
  if (doc["skip_ms"].is<int64_t>()) skip_ms_ += doc["skip_ms"].as<int64_t>();
  if (doc["wake_now"] | false) {
    if (asleep_ && wake_at_) skip_ms_ += (int64_t)wake_at_ - (int64_t)virtualMs() + 10;
  }
  if (doc["brownout"] | false) {   // the battery sags under a Wi-Fi burst: the board resets, and says why
    brownout_next_ = true;
    reset_ = 1;
  }
  if (doc["power_cycle"] | false) reset_ = 1;
  if (doc["factory_reset"] | false) reset_ = 2;
}
