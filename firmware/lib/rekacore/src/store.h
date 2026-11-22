// On-flash state: the current batch of cards (two slots, so a new batch only replaces the old one once every
// frame in it checked out) and the review log (kept until the server confirms each review).
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "hal.h"

namespace reka {

struct CardMeta {
  std::string id;
  int rev = 0;
  bool choice = false;
  uint32_t crc = 0;
  uint32_t size = 0;                 // compressed frame bytes, from the manifest (0 if it didn't say)
  uint64_t due_ms = 0;               // server time
  std::vector<std::pair<int, int>> hide;  // row bands blanked in the question view
  std::string qstrip;                // asset pasted over the strip in the question view ("" for none)
  int boxes[4][4] = {};              // choice cards: x, y, w, h of each option, left to right
  int correct = -1;
};

class CardStore {
 public:
  explicit CardStore(Hal& hal) : hal_(hal) {}
  bool load();                                    // the active slot's manifest and done list
  void clear();                                   // forget every card, used when the device is unlinked
  const std::vector<CardMeta>& cards() const { return cards_; }
  const std::string& batchId() const { return batch_id_; }
  int version() const { return version_; }
  bool isDone(const std::string& id) const { return done_.count(id) > 0; }
  void markDone(const std::string& id);
  void unmarkDone(const std::string& id);
  bool readFrame(const CardMeta& c, std::string& z);
  bool readAsset(const std::string& name, std::string& z);
  uint32_t assetCrc(const std::string& name) const;

  // downloading into the other slot
  std::string stagingPath(const std::string& file) const;
  void beginStaging();
  bool commitStaging(const std::string& manifest_json);   // parses, writes the manifest, flips the active slot

  static bool parseManifest(const std::string& json, std::vector<CardMeta>& cards, std::string& batch_id, int& version,
                            std::vector<std::pair<std::string, uint32_t>>& assets);

 private:
  std::string slotDir(int slot) const { return slot ? "/s1" : "/s0"; }
  int activeSlot();
  Hal& hal_;
  std::vector<CardMeta> cards_;
  std::vector<std::pair<std::string, uint32_t>> assets_;
  std::set<std::string> done_;
  std::string batch_id_;
  int version_ = 0;
};

struct Review {
  uint32_t rid = 0;
  std::string card;
  int grade = 0;            // 1 Again, 2 Hard, 3 Good, 4 Easy, 0 skipped
  uint64_t shown = 0, revealed = 0, graded = 0;   // device clock (rtcMs)
  int choice = -1;
  uint32_t boot = 0;
};

class ReviewLog {
 public:
  explicit ReviewLog(Hal& hal) : hal_(hal) {}
  void append(Review r);                       // assigns the next review id
  std::vector<Review> all();
  void remove(const std::set<uint32_t>& rids);
  bool popLast(Review& out);                   // for undo, only works while the review hasn't synced
  size_t count();
  void clear();

 private:
  static std::string encode(const Review& r);
  static bool decode(const std::string& line, Review& r);
  Hal& hal_;
};

}  // namespace reka
