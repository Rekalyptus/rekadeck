#include "hal_esp32.h"

#include <LittleFS.h>
#include <SPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <nvs_flash.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#include <sys/time.h>

#include <algorithm>
#include <cstring>

#include "ca_roots.h"
#include "pins.h"
#include "portal_html.h"
#include "vendor/ArduinoJson.h"

using reka::FB_BYTES;
using reka::SCREEN_H;
using reka::SCREEN_W;

// survives deep sleep, cleared on power-on
RTC_DATA_ATTR static reka::RtcState s_rtc;
// the access point we joined last, so the next join can skip the scan (about 0.5 s instead of 1.5 s or more)
RTC_DATA_ATTR static struct { uint8_t bssid[6]; int32_t channel; bool valid; } s_ap;

// rtcMs() counts from this power-on. The ESP-IDF system time keeps running through deep sleep, but it doesn't
// always start at 0: on the first real board it read about -46 years (a time base left in the RTC registers
// survives a reset), and rtc_ms went out as 2^64 - 1.46e12. So the first reading after power-on is the zero point.
// It sits in RTC memory next to s_rtc, so it resets exactly when the boot id does.
static const uint32_t CLOCK_MAGIC = 0x524B434C;
RTC_DATA_ATTR static struct { uint32_t magic; int64_t zero_us; } s_clock;

static int64_t systemUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

// Key presses are caught by interrupt, so a tap during a refresh or an HTTP call isn't lost. A press counts when the
// key goes down after this handler has seen it up for 25 ms, which ignores the bounce on both edges.
//
// "Up" is the level the handler last read, tracked as a state. The handler runs microseconds after the edge and a
// bouncing contact can already be back down, so the rising edge of a release often reads as down. Judging that read
// against "when did we last see it up" (the first 0.3.0) counted it as a press whenever the key had been held longer
// than 25 ms, which is nearly every press: a phantom press on many releases, which Keys' merged-tap rule then
// delivered as a second K1 to K4 right after the refresh (the next card revealing itself, a revealed card graded Good,
// the menu moving two rows).
static volatile uint32_t s_latched = 0;
static volatile bool s_up[4] = {true, true, true, true};   // the level this handler last read; set in begin()
static volatile uint32_t s_up_since[4] = {0, 0, 0, 0};     // when it last went from down to up
static volatile uint32_t s_presses[4] = {0, 0, 0, 0};      // every real press, for presses()
static void IRAM_ATTR onKeyEdge(void* arg) {
  int i = (int)(intptr_t)arg;
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  if ((REG_READ(GPIO_IN_REG) >> PIN_KEYS[i]) & 1) {   // up (or bouncing up)
    if (!s_up[i]) {
      s_up[i] = true;
      s_up_since[i] = now;
    }
  } else if (s_up[i]) {                                // down, from up
    s_up[i] = false;
    if (now - s_up_since[i] > 25) {                    // up long enough: a real press, not a bounce
      s_latched |= 1u << i;
      s_presses[i]++;
    }
  }
}

// Firmware updates: a new image boots "pending" and the bootloader rolls it back on the next reset unless it
// confirms itself first. Arduino confirms every boot straight away by default; returning true here leaves it to
// Device, which confirms once the new firmware has reached sleep or checked in (otaConfirm()).
extern "C" bool verifyRollbackLater() { return true; }

Esp32Hal::Esp32Hal() : panel_(PIN_EPD_SCK, PIN_EPD_MOSI, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY) {}

void Esp32Hal::begin() {
  if (s_clock.magic != CLOCK_MAGIC) {   // power-on: start the uptime counter here
    s_clock.magic = CLOCK_MAGIC;
    s_clock.zero_us = systemUs();
  }
  // 80 MHz is the lowest clock Wi-Fi runs at. The device spends its awake time waiting on the panel, the network or
  // a key, none of which go faster at 240 MHz, and the core draws roughly half as much.
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // Don't stall when no computer is listening. Not 0: in this core's HWCDC::write a 0 timeout makes its no-progress
  // counter (`tries--`) wrap to 4 billion, so once a computer has read the port (which sets `connected`) and then
  // stopped, the first log line that doesn't fit the 256-byte buffer blocks until something reads again: a frozen
  // device on a laptop's USB, keys dead, no check-ins. With 5 ms the counter runs out, HWCDC marks the port
  // disconnected, and later lines are dropped instead.
  Serial.setTxTimeoutMs(5);
#endif
  browned_out_ = esp_reset_reason() == ESP_RST_BROWNOUT;

  // why did we wake up?
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    for (int i = 0; i < 4; i++)
      if (pins & (1ULL << PIN_KEYS[i])) wake_keys_ |= 1 << i;
    if (!wake_keys_) wake_keys_ = 1;   // woke on a key that was already let go
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    woke_by_timer_ = true;
  }

  // the keys were RTC pins while asleep, hand them back to the normal GPIO matrix
  for (int i = 0; i < 4; i++) {
    rtc_gpio_deinit((gpio_num_t)PIN_KEYS[i]);
    pinMode(PIN_KEYS[i], INPUT_PULLUP);
    s_up[i] = digitalRead(PIN_KEYS[i]) == HIGH;   // a key still down from the wake press only counts once let go
    s_up_since[i] = 0;
    attachInterruptArg(PIN_KEYS[i], onKeyEdge, (void*)(intptr_t)i, CHANGE);
  }
  if (PIN_VBUS_SENSE >= 0) pinMode(PIN_VBUS_SENSE, INPUT);
  // deepSleep() held the panel's power switch low through the sleep; let go of it before driving it again
  rtc_gpio_hold_dis((gpio_num_t)PIN_EPD_PWR);
  rtc_gpio_deinit((gpio_num_t)PIN_EPD_PWR);
  pinMode(PIN_EPD_PWR, OUTPUT);
  digitalWrite(PIN_EPD_PWR, LOW);   // the panel only gets power when something is drawn
  if (PIN_RGB_LED >= 0) neopixelWrite(PIN_RGB_LED, 0, 0, 0);   // see pins.h: it may have latched a colour

  Wire.begin(PIN_SDA, PIN_SCL);
  fs_ok_ = LittleFS.begin(true);   // formats the partition the very first time
  if (!fs_ok_) Serial.println("LittleFS failed to mount");
  prefs_.begin("reka", false);
  WiFi.persistent(false);          // credentials live in our own settings, not the Wi-Fi driver's flash

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {   // a real boot: say what we're running on
    reka::HwInfo h = hwInfo();
    Serial.printf("[%8lu] boot: %s, PSRAM %lu KB, on-chip RAM %lu KB free (largest block %lu KB)%s\n",
                  (unsigned long)::millis(), h.app_slot.c_str(), (unsigned long)(h.psram_total / 1024),
                  (unsigned long)(h.internal_free / 1024), (unsigned long)(h.internal_largest / 1024),
                  browned_out_ ? ", after a brownout" : "");
  }
}

void Esp32Hal::idle() {
  ::delay(10);
}

// ------------------------------------------------------------------ time and keys
uint32_t Esp32Hal::millis() { return ::millis(); }

uint64_t Esp32Hal::rtcMs() {
  // the ESP-IDF system time keeps running through deep sleep on the RTC clock; measured from our own zero point
  int64_t us = systemUs() - s_clock.zero_us;
  return us > 0 ? (uint64_t)(us / 1000) : 0;
}

void Esp32Hal::delayMs(uint32_t ms) { ::delay(ms); }

uint32_t Esp32Hal::random32() { return esp_random(); }

uint8_t Esp32Hal::keys() {
  uint8_t held = 0;
  uint32_t now = ::millis();
  for (int i = 0; i < 4; i++) {
    uint8_t bit = 1 << i;
    if (digitalRead(PIN_KEYS[i]) == LOW) {
      held |= bit;
      latch_seen_[i] = 0;
    } else if (s_latched & bit) {
      // pressed and let go while we weren't looking: report it as held for 40 ms so the debounce sees a tap
      if (!latch_seen_[i]) latch_seen_[i] = now | 1;
      if (now - latch_seen_[i] > 40) {
        __atomic_fetch_and(&s_latched, ~(uint32_t)bit, __ATOMIC_SEQ_CST);
        latch_seen_[i] = 0;
      } else {
        held |= bit;
      }
    }
  }
  return held;
}

uint8_t Esp32Hal::presses() {
  uint8_t out = 0;
  for (int i = 0; i < 4; i++) {
    uint32_t n = s_presses[i];
    if (n != presses_seen_[i]) out |= 1 << i;
    presses_seen_[i] = n;
  }
  return out;
}

reka::RtcState& Esp32Hal::rtc() { return s_rtc; }

// ------------------------------------------------------------------ panel
void Esp32Hal::panelOn() {
  if (panel_on_) return;
  digitalWrite(PIN_EPD_PWR, HIGH);
  ::delay(10);
  panel_.begin();
  panel_on_ = true;
}

bool Esp32Hal::display(const uint8_t* fb, const uint8_t* previous, reka::Refresh mode, const char* why) {
  panelOn();
  uint32_t t0 = ::millis();
  bool ok = panel_.refresh(fb, previous, mode);
  char msg[96];   // real refresh times, to compare with the simulator's
  snprintf(msg, sizeof msg, "refresh %s %lu ms%s (%s)", mode == reka::Refresh::Full ? "full" : mode == reka::Refresh::Fast ? "fast" : "partial",
           (unsigned long)(::millis() - t0), ok ? "" : ", panel didn't answer in time", why ? why : "");
  log(msg);
  return ok;
}

// ------------------------------------------------------------------ power
bool Esp32Hal::readGauge(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(FUEL_GAUGE_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(FUEL_GAUGE_ADDR, (uint8_t)2) != 2) return false;
  uint8_t hi = Wire.read(), lo = Wire.read();
  value = ((uint16_t)hi << 8) | lo;
  return true;
}

bool Esp32Hal::writeGauge(uint8_t reg, uint16_t value) {
  Wire.beginTransmission(FUEL_GAUGE_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(value >> 8));
  Wire.write((uint8_t)(value & 0xFF));
  return Wire.endTransmission() == 0;
}

// A computer on the USB port sends a start-of-frame every millisecond, and the USB Serial/JTAG block counts them.
// The count moving means a host is there. A wall charger sends nothing, so it isn't seen here.
static bool usbHostPresent() {
  uint32_t a = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX;
  ::delay(3);
  return (REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX) != a;
}

reka::PowerInfo Esp32Hal::power() {
  // Several screens and the idle loop ask often; the gauge itself only measures every 250 ms at best
  uint32_t now = ::millis();
  if (power_at_ && (int32_t)(now - power_at_) < 400) return power_;
  reka::PowerInfo p;
  uint16_t soc = 0, vcell = 0, crate = 0;
  bool got_soc = readGauge(0x04, soc), got_v = readGauge(0x02, vcell);
  p.gauge = got_soc && got_v;
  if (got_soc) p.pct = std::min(100.0f, std::max(0.0f, soc / 256.0f));
  if (got_v) p.mv = (int)(vcell * 78.125f / 1000.0f);
  if (readGauge(0x16, crate)) p.crate = (int16_t)crate * 0.208f;   // percent per hour
  // 214 mA into even a 5 Ah cell is over 4 %/h; 1 %/h leaves room for the reading's noise
  p.charging = p.gauge && p.crate > 1.0f;
  // A computer on the port proves USB data, not USB power. The first board enumerated on a Mac while running from
  // its battery (no charging, 2.4 % gone in 50 min), and counting that as "on USB" would keep it awake on the sleep
  // screen draining the cell. So a host counts as power only while the battery isn't visibly draining.
  p.usb_host = usbHostPresent();
  if (PIN_VBUS_SENSE >= 0) p.usb = digitalRead(PIN_VBUS_SENSE) == HIGH;
  else p.usb = p.charging || (p.usb_host && p.gauge && p.crate > -1.0f);
  power_ = p;
  power_at_ = now | 1;
  return p;
}

void Esp32Hal::gaugeFast(bool on) {
  // HIBRT: 0x0000 never hibernates (a fresh VCELL every 250 ms), 0x8030 is the power-on default (dozes when the
  // battery is quiet, and then measures only every 45 s)
  writeGauge(0x0A, on ? 0x0000 : 0x8030);
  power_at_ = 0;
}

void Esp32Hal::deepSleep(uint32_t timer_ms) {
  // a key held down while we fall asleep would wake us straight back up, so wait for it to be let go
  uint32_t t0 = ::millis();
  while (keys() && ::millis() - t0 < 5000) ::delay(10);
  uint8_t stuck = keys();   // still held after 5 s: leave that key out of the wake sources

  portalStop();
  wifiOff();
  for (int p : PIN_KEYS) detachInterrupt(p);
  if (panel_on_) {
    panel_.sleep();
    SPI.end();
  }
  digitalWrite(PIN_EPD_PWR, LOW);
  // Deep sleep lets go of every pin, so on its own the HAT's power switch would float for the hours the board sleeps,
  // half on and half off. Hold it low in the RTC domain instead; begin() releases it on the next wake.
  {
    gpio_num_t g = (gpio_num_t)PIN_EPD_PWR;
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(g, 0);
    rtc_gpio_hold_en(g);
  }
  // don't feed the unpowered HAT through its signal lines
  for (int p : {PIN_EPD_SCK, PIN_EPD_MOSI, PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST}) pinMode(p, INPUT);

  uint64_t mask = 0;
  for (int i = 0; i < 4; i++) {
    if (stuck & (1 << i)) continue;
    gpio_num_t g = (gpio_num_t)PIN_KEYS[i];
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(g);
    rtc_gpio_pulldown_dis(g);
    mask |= 1ULL << PIN_KEYS[i];
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keeps the pull-ups and the hold alive
  if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (timer_ms) esp_sleep_enable_timer_wakeup((uint64_t)timer_ms * 1000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

void Esp32Hal::restart() {
  portalStop();
  wifiOff();
  if (panel_on_) panel_.sleep();
  digitalWrite(PIN_EPD_PWR, LOW);
  Serial.flush();
  ESP.restart();
}

// ------------------------------------------------------------------ firmware updates
bool Esp32Hal::otaInstall(const std::string& url, const std::string& bearer, uint32_t size, const std::string& sha256,
                          std::string& error) {
  if (!httpBegin(url, bearer, 15000)) { error = "couldn't start the download"; return false; }
  int code = http_->GET();
  if (code != 200) {
    if (code < 0) logTlsError("firmware download");
    error = "download answered " + std::to_string(code);
    httpClose();
    return false;
  }
  int length = http_->getSize();
  if (length != (int)size) {   // also catches a reply with no Content-Length
    error = "download is " + std::to_string(length) + " bytes, expected " + std::to_string(size);
    httpClose();
    return false;
  }
  if (!Update.begin(size, U_FLASH)) {   // too big for the spare slot, or no spare slot at all
    error = std::string("can't start the update: ") + Update.errorString();
    httpClose();
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient* in = http_->getStreamPtr();
  static uint8_t buf[2048];
  uint32_t got = 0, last = ::millis();
  bool wrote = true;
  while (got < size && wrote) {
    int avail = in->available();
    if (avail <= 0) {
      if (!in->connected() || ::millis() - last > 15000) break;   // cut off, or stalled for 15 s
      ::delay(2);
      continue;
    }
    int n = in->read(buf, std::min<int>({(int)sizeof buf, avail, (int)(size - got)}));
    if (n <= 0) continue;
    last = ::millis();
    mbedtls_sha256_update_ret(&sha, buf, n);
    wrote = Update.write(buf, n) == (size_t)n;
    got += n;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  httpClose();
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
  std::string want = sha256;
  std::transform(want.begin(), want.end(), want.begin(), ::tolower);
  if (!wrote || got != size || want != hex) {
    Update.abort();
    error = !wrote ? std::string("flash write failed: ") + Update.errorString()
          : got != size ? "download cut off at " + std::to_string(got) + " bytes"
                        : std::string("SHA-256 doesn't match");
    return false;
  }
  if (!Update.end(true)) {   // checks the image itself, then makes it the one to boot
    error = std::string("image rejected: ") + Update.errorString();
    return false;
  }
  return true;
}

bool Esp32Hal::otaConfirm() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY)
    return false;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

// ------------------------------------------------------------------ files and settings
bool Esp32Hal::readFile(const char* path, std::string& out) {
  if (!fs_ok_) return false;
  File f = LittleFS.open(path, "r");
  if (!f || f.isDirectory()) return false;
  size_t n = f.size();
  out.resize(n);
  size_t got = n ? f.read((uint8_t*)&out[0], n) : 0;
  f.close();
  return got == n;
}

bool Esp32Hal::writeFile(const char* path, const void* data, size_t n, bool append) {
  if (!fs_ok_) return false;
  File f = LittleFS.open(path, append ? "a" : "w", true);   // true: make the folders on the way
  if (!f) return false;
  size_t put = n ? f.write((const uint8_t*)data, n) : 0;
  f.close();
  return put == n;
}

bool Esp32Hal::renameFile(const char* from, const char* to) {
  if (!fs_ok_) return false;
  if (LittleFS.rename(from, to)) return true;   // LittleFS replaces the target in one step
  LittleFS.remove(to);
  return LittleFS.rename(from, to);
}

bool Esp32Hal::removeFile(const char* path) {
  return fs_ok_ && LittleFS.remove(path);
}

void Esp32Hal::removeDir(const char* path) {
  if (!fs_ok_) return;
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return;
  std::vector<std::string> names;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    names.push_back(f.path());
    f.close();
  }
  dir.close();
  for (auto& n : names) LittleFS.remove(n.c_str());
  LittleFS.rmdir(path);
}

size_t Esp32Hal::freeBytes() {
  return fs_ok_ ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
}

bool Esp32Hal::getSetting(const char* key, std::string& out) {
  if (!prefs_.isKey(key)) return false;
  out = prefs_.getString(key, "").c_str();
  return true;
}

void Esp32Hal::setSetting(const char* key, const std::string& value) {
  prefs_.putString(key, value.c_str());
}

void Esp32Hal::eraseSettings() { prefs_.clear(); }

void Esp32Hal::eraseAll() {
  // Everything a new owner shouldn't inherit: the home Wi-Fi password and the account token (NVS, all of it, the
  // Wi-Fi driver's own namespace included), and the cards and unsent reviews (LittleFS).
  httpClose();
  prefs_.end();
  nvs_flash_erase();
  nvs_flash_init();
  if (fs_ok_) LittleFS.format();
  prefs_.begin("reka", false);
}

// ------------------------------------------------------------------ Wi-Fi and HTTP
void Esp32Hal::startJoin(const std::string& ssid, const std::string& pass, bool use_cache) {
  WiFi.mode(portal_ ? WIFI_AP_STA : WIFI_STA);   // keep the setup network up while we try
  WiFi.setTxPower(WIFI_POWER_19_5dBm);            // the home router can be rooms away; setup turned it down
  const char* pw = pass.empty() ? nullptr : pass.c_str();
  if (use_cache && s_ap.valid) WiFi.begin(ssid.c_str(), pw, s_ap.channel, s_ap.bssid);
  else WiFi.begin(ssid.c_str(), pw);
}

void Esp32Hal::wifiBegin(const std::string& ssid, const std::string& pass) {
  startJoin(ssid, pass, true);
}

bool Esp32Hal::wifiConnect(const std::string& ssid, const std::string& pass, uint32_t timeout_ms) {
  startJoin(ssid, pass, !portal_);   // a network typed into the setup page gets a full scan
  uint32_t t0 = ::millis();
  while (::millis() - t0 < timeout_ms) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) return wifiConnected();
    if (::millis() - t0 > 1500 && (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL)) break;   // wrong password, or gone
    ::delay(50);
  }
  WiFi.disconnect();
  s_ap.valid = false;
  if (portal_) WiFi.setTxPower(WIFI_POWER_8_5dBm);   // wrong password: back to setup's quiet radio for the retry
  return false;
}

bool Esp32Hal::wifiConnected() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !s_ap.valid) {   // remember where we joined, for a quicker join after the next wake
    memcpy(s_ap.bssid, WiFi.BSSID(), 6);
    s_ap.channel = WiFi.channel();
    s_ap.valid = true;
  }
  return up;
}

int Esp32Hal::wifiRssi() {
  return WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0;
}

void Esp32Hal::wifiOff() {
  httpClose();
  if (portal_) return;   // portalStop() turns the radio off when setup is over
  if (WiFi.status() != WL_CONNECTED) s_ap.valid = false;   // a join that didn't work: scan properly next time
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

namespace {
// HTTPClient writes the response into a Stream, this one collects it in a std::string (card frames are binary)
class StringSink : public Stream {
 public:
  explicit StringSink(std::string& out) : out_(out) {}
  size_t write(uint8_t c) { out_.push_back((char)c); return 1; }
  size_t write(const uint8_t* buf, size_t n) { out_.append((const char*)buf, n); return n; }
  int available() { return 0; }
  int read() { return -1; }
  int peek() { return -1; }
  void flush() {}

 private:
  std::string& out_;
};
}  // namespace

void Esp32Hal::logTlsError(const char* what) {
  if (!net_tls_ || !net_) return;
  char err[100] = "";
  int code = static_cast<WiFiClientSecure*>(net_.get())->lastError(err, sizeof err);
  if (code) {
    char msg[160];
    snprintf(msg, sizeof msg, "%s: TLS error %d, %s", what, code, err);
    log(msg);
  }
}

void Esp32Hal::httpClose() {
  // HTTPClient only forgets its client when it closes the connection itself, which keep-alive skips. Ending it and
  // then deleting net_ left it pointing at freed memory, and the next request's end() called through that (a panic
  // or a watchdog reset on the second Wi-Fi session of a wake: unlink, Sync now, a mid-session check-in). Destroying
  // it first, while net_ is still alive, is the only way to make it let go.
  http_.reset();
  if (net_) net_->stop();
  net_.reset();
  net_origin_.clear();
}

bool Esp32Hal::httpBegin(const std::string& url, const std::string& bearer, uint32_t timeout_ms) {
  bool tls = url.rfind("https://", 0) == 0;
  size_t host_at = tls ? 8 : 7, path_at = url.find('/', host_at);
  std::string origin = url.substr(0, path_at);
  if (!net_ || origin != net_origin_) {   // a different server (or none yet): a new connection
    httpClose();
    if (tls) {
      auto* s = new WiFiClientSecure();
      s->setCACert(CA_ROOTS_PEM);   // ca_roots.h; dates aren't checked (no clock yet), see PROTOCOL.md
      s->setHandshakeTimeout(15);   // seconds; the library default is two minutes
      net_.reset(s);
    } else {
      net_.reset(new WiFiClient());
    }
    net_tls_ = tls;
    net_origin_ = origin;
  }
  if (!http_) http_.reset(new HTTPClient());   // one per connection, see httpClose()
  // A server that answers at all accepts the connection in well under a second. One that's asleep or gone (a laptop
  // that left the house) never does, and every second spent waiting here is a second of Wi-Fi on the battery.
  http_->setConnectTimeout((int32_t)std::min<uint32_t>(timeout_ms, 4000));
  http_->setTimeout((uint16_t)std::min<uint32_t>(timeout_ms, 65000));
  http_->setReuse(true);      // HTTP/1.1 keep-alive: the next request skips the TCP and TLS handshakes
  http_->useHTTP10(false);
  if (!http_->begin(*net_, String(url.c_str()))) return false;
  if (!bearer.empty()) http_->addHeader("Authorization", String("Bearer ") + bearer.c_str());
  return true;
}

reka::HttpResult Esp32Hal::http(const char* method, const std::string& url, const std::string& body,
                                const std::string& bearer, uint32_t timeout_ms) {
  reka::HttpResult r;
  if (!wifiConnected()) return r;
  for (int attempt = 0; attempt < 2; attempt++) {
    bool reused = net_ && net_->connected();
    if (!httpBegin(url, bearer, timeout_ms)) return r;
    if (!body.empty()) http_->addHeader("Content-Type", "application/json");
    int code = body.empty() ? http_->sendRequest(method)
                            : http_->sendRequest(method, (uint8_t*)body.data(), body.size());
    if (code > 0) {
      r.status = code;
      StringSink sink(r.body);
      if (http_->writeToStream(&sink) < 0) r.body.clear();
      http_->end();   // keeps the connection open when the server allows it
      return r;
    }
    // A kept-alive connection the server had already closed fails like this; one fresh try. A new connection
    // that fails is a real failure (no answer, or TLS refused: the log says which).
    logTlsError(url.c_str());
    httpClose();
    if (!reused) break;
  }
  return r;
}

// ------------------------------------------------------------------ setup portal
std::string Esp32Hal::scanNetworks() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  std::vector<std::string> seen;
  for (int i = 0; i < n; i++) {   // strongest first, each name once
    int best = -1;
    for (int j = 0; j < n; j++) {
      std::string name = WiFi.SSID(j).c_str();
      if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      if (best < 0 || WiFi.RSSI(j) > WiFi.RSSI(best)) best = j;
    }
    if (best < 0) break;
    seen.push_back(WiFi.SSID(best).c_str());
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = seen.back();
    o["secure"] = WiFi.encryptionType(best) != WIFI_AUTH_OPEN;
    o["rssi"] = WiFi.RSSI(best);
  }
  WiFi.scanDelete();
  std::string s;
  serializeJson(doc, s);
  return s;
}

void Esp32Hal::portalStart(const std::string& ap_name, const std::string& ap_pass, const std::string& default_server) {
  portalStop();
  portal_server_ = default_server;
  submitted_ = false;

  // scan first: scanning later would hop the access point's channel under the phone's feet
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  scan_json_ = scanNetworks();

  WiFi.mode(WIFI_AP_STA);
  // WPA2 with the password shown on the panel: someone nearby who can't see the screen can't join, so they can't
  // point the device at a server of their own (whose link page would then be the one on the screen)
  WiFi.softAP(ap_name.c_str(), ap_pass.size() >= 8 ? ap_pass.c_str() : nullptr);
  // At full power (19.5 dBm) an iPhone next to the board retried DHCP for 35 s on a rejoin: the board heard it,
  // it mostly didn't hear the board. The phone is close during setup, so a quieter radio costs nothing here.
  // startJoin() turns it back up for the home network.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  IPAddress ip = WiFi.softAPIP();
  portal_ = true;

  dns_.reset(new DNSServer());
  dns_->setErrorReplyCode(DNSReplyCode::NoError);
  dns_->start(53, "*", ip);   // every name points at us, so phones pop the setup page up by themselves

  web_.reset(new WebServer(80));
  web_->on("/", HTTP_GET, [this]() {
    std::string html = reka::PORTAL_HTML;
    size_t at = html.find("{{server}}");
    if (at != std::string::npos) html.replace(at, 10, portal_server_);
    web_->send(200, "text/html", html.c_str());
  });
  web_->on("/scan", HTTP_GET, [this]() { web_->send(200, "application/json", scan_json_.c_str()); });
  web_->on("/save", HTTP_POST, [this]() {
    submit_.ssid = web_->arg("ssid").c_str();
    submit_.pass = web_->arg("pass").c_str();
    submit_.server = web_->arg("server").c_str();
    submitted_ = true;
    web_->send(200, "text/html", reka::PORTAL_SAVED_HTML);
  });
  web_->onNotFound([this, ip]() {   // captive portal checks from iOS and Android land here
    web_->sendHeader("Location", String("http://") + ip.toString() + "/", true);
    web_->send(302, "text/plain", "");
  });
  web_->begin();
  log(("setup portal up on " + ap_name).c_str());
}

bool Esp32Hal::portalPoll(reka::PortalSubmit& out) {
  if (!portal_) return false;
  dns_->processNextRequest();
  web_->handleClient();
  if (!submitted_) return false;
  submitted_ = false;
  out = submit_;
  return true;
}

void Esp32Hal::portalStop() {
  if (!portal_) return;
  web_->stop();
  dns_->stop();
  web_.reset();
  dns_.reset();
  WiFi.softAPdisconnect(true);
  portal_ = false;
  WiFi.mode(WiFi.status() == WL_CONNECTED ? WIFI_STA : WIFI_OFF);
}

// ------------------------------------------------------------------ hardware info and log
reka::HwInfo Esp32Hal::hwInfo() {
  reka::HwInfo h;
  uint16_t version = 0;
  h.gauge = readGauge(0x08, version);
  h.gauge_version = version;
  h.fs = fs_ok_;
  if (fs_ok_) {
    h.fs_total = LittleFS.totalBytes();
    h.fs_used = LittleFS.usedBytes();
  }
  h.heap_free = ESP.getFreeHeap();
  h.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  h.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  h.psram_total = psramFound() ? ESP.getPsramSize() : 0;
  h.cpu_mhz = (int)getCpuFrequencyMhz();
  const esp_partition_t* running = esp_ota_get_running_partition();
  h.app_slot = running ? running->label : "?";
  return h;
}

void Esp32Hal::log(const char* line) {
  Serial.printf("[%8lu] %s\n", (unsigned long)::millis(), line);
}
