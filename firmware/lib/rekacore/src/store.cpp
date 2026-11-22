#include "store.h"

#include <cstdio>
#include <cstdlib>

#include "vendor/ArduinoJson.h"

namespace reka {

static const char* LOG_PATH = "/reviews.log";

int CardStore::activeSlot() {
  std::string s;
  return hal_.getSetting("slot", s) && s == "1" ? 1 : 0;
}

bool CardStore::parseManifest(const std::string& json, std::vector<CardMeta>& cards, std::string& batch_id,
                              int& version, std::vector<std::pair<std::string, uint32_t>>& assets) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  cards.clear();
  assets.clear();
  batch_id = doc["batch_id"] | "";
  version = doc["version"] | 0;
  for (JsonObject a : doc["assets"].as<JsonArray>()) assets.emplace_back(a["name"] | "", a["crc"] | 0u);
  for (JsonObject c : doc["cards"].as<JsonArray>()) {
    CardMeta m;
    m.id = c["id"] | "";
    m.rev = c["rev"] | 0;
    m.choice = std::string(c["kind"] | "basic") == "choice";
    m.crc = c["crc"] | 0u;
    m.size = c["size"] | 0u;
    m.due_ms = c["due_ms"] | (uint64_t)0;
    m.qstrip = c["qstrip"] | "";
    for (JsonArray h : c["hide"].as<JsonArray>()) m.hide.emplace_back(h[0] | 0, h[1] | 0);
    if (m.choice) {
      int i = 0;
      for (JsonArray b : c["choices"]["boxes"].as<JsonArray>()) {
        if (i >= 4) break;
        for (int k = 0; k < 4; k++) m.boxes[i][k] = b[k] | 0;
        i++;
      }
      m.correct = c["choices"]["correct"] | -1;
    }
    cards.push_back(std::move(m));
  }
  return true;
}

bool CardStore::load() {
  cards_.clear();
  done_.clear();
  batch_id_.clear();
  version_ = 0;
  std::string dir = slotDir(activeSlot()), json, done;
  if (!hal_.readFile((dir + "/manifest.json").c_str(), json)) return false;
  if (!parseManifest(json, cards_, batch_id_, version_, assets_)) return false;
  if (hal_.readFile((dir + "/done.txt").c_str(), done)) {
    size_t a = 0;
    while (a < done.size()) {
      size_t b = done.find('\n', a);
      if (b == std::string::npos) b = done.size();
      if (b > a) done_.insert(done.substr(a, b - a));
      a = b + 1;
    }
  }
  return true;
}

void CardStore::clear() {
  hal_.removeDir(slotDir(0).c_str());
  hal_.removeDir(slotDir(1).c_str());
  hal_.setSetting("slot", "0");
  load();
}

void CardStore::markDone(const std::string& id) {
  if (!done_.insert(id).second) return;
  std::string line = id + "\n";
  hal_.writeFile((slotDir(activeSlot()) + "/done.txt").c_str(), line.data(), line.size(), true);
}

void CardStore::unmarkDone(const std::string& id) {
  if (!done_.erase(id)) return;
  std::string all;
  for (auto& d : done_) all += d + "\n";
  hal_.writeFile((slotDir(activeSlot()) + "/done.txt").c_str(), all.data(), all.size());
}

bool CardStore::readFrame(const CardMeta& c, std::string& z) {
  return hal_.readFile((slotDir(activeSlot()) + "/c_" + c.id + ".z").c_str(), z);
}

bool CardStore::readAsset(const std::string& name, std::string& z) {
  return hal_.readFile((slotDir(activeSlot()) + "/a_" + name + ".z").c_str(), z);
}

uint32_t CardStore::assetCrc(const std::string& name) const {
  for (auto& a : assets_)
    if (a.first == name) return a.second;
  return 0;
}

std::string CardStore::stagingPath(const std::string& file) const {
  std::string s;
  int staging = (hal_.getSetting("slot", s) && s == "1") ? 0 : 1;
  return slotDir(staging) + "/" + file;
}

void CardStore::beginStaging() {
  hal_.removeDir(stagingPath("").c_str());
}

bool CardStore::commitStaging(const std::string& manifest_json) {
  std::string path = stagingPath("manifest.json");
  if (!hal_.writeFile(path.c_str(), manifest_json.data(), manifest_json.size())) return false;
  hal_.removeFile(stagingPath("done.txt").c_str());
  hal_.setSetting("slot", activeSlot() ? "0" : "1");
  return load();
}

// ------------------------------------------------------------------ review log, one JSON object per line
std::string ReviewLog::encode(const Review& r) {
  char buf[200];
  snprintf(buf, sizeof buf,
           "{\"rid\":%lu,\"card\":\"%s\",\"g\":%d,\"shown\":%llu,\"revealed\":%llu,\"graded\":%llu,\"choice\":%d,\"boot\":%lu}\n",
           (unsigned long)r.rid, r.card.c_str(), r.grade, (unsigned long long)r.shown, (unsigned long long)r.revealed,
           (unsigned long long)r.graded, r.choice, (unsigned long)r.boot);
  return buf;
}

bool ReviewLog::decode(const std::string& line, Review& r) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;
  r.rid = doc["rid"] | 0u;
  r.card = doc["card"] | "";
  r.grade = doc["g"] | 0;
  r.shown = doc["shown"] | (uint64_t)0;
  r.revealed = doc["revealed"] | (uint64_t)0;
  r.graded = doc["graded"] | (uint64_t)0;
  r.choice = doc["choice"] | -1;
  r.boot = doc["boot"] | 0u;
  return r.rid != 0;
}

void ReviewLog::append(Review r) {
  std::string s;
  uint32_t rid = hal_.getSetting("rid", s) ? (uint32_t)strtoul(s.c_str(), nullptr, 10) : 0;
  r.rid = ++rid;
  hal_.setSetting("rid", std::to_string(rid));
  std::string line = encode(r);
  hal_.writeFile(LOG_PATH, line.data(), line.size(), true);
}

std::vector<Review> ReviewLog::all() {
  std::vector<Review> out;
  std::string data;
  if (!hal_.readFile(LOG_PATH, data)) return out;
  size_t a = 0;
  while (a < data.size()) {
    size_t b = data.find('\n', a);
    if (b == std::string::npos) b = data.size();
    Review r;
    if (b > a && decode(data.substr(a, b - a), r)) out.push_back(r);
    a = b + 1;
  }
  return out;
}

void ReviewLog::remove(const std::set<uint32_t>& rids) {
  std::string keep;
  for (auto& r : all())
    if (!rids.count(r.rid)) keep += encode(r);
  if (keep.empty()) {
    hal_.removeFile(LOG_PATH);
  } else {
    hal_.writeFile("/reviews.tmp", keep.data(), keep.size());
    hal_.renameFile("/reviews.tmp", LOG_PATH);
  }
}

bool ReviewLog::popLast(Review& out) {
  auto rs = all();
  if (rs.empty()) return false;
  out = rs.back();
  remove({out.rid});
  return true;
}

size_t ReviewLog::count() {
  // Asked on every status line, so count the lines rather than parse each one
  std::string data;
  if (!hal_.readFile(LOG_PATH, data)) return 0;
  size_t n = 0, a = 0;
  while (a < data.size()) {
    size_t b = data.find('\n', a);
    if (b == std::string::npos) b = data.size();
    if (b > a + 1) n++;
    a = b + 1;
  }
  return n;
}

void ReviewLog::clear() { hal_.removeFile(LOG_PATH); }

}  // namespace reka
