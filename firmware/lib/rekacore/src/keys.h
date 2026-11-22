// Turns raw key states into events: presses, the two single-key holds and the 1+4 menu hold.
//   Keys 2 and 3 fire as soon as they go down (they have no hold), keys 1 and 4 fire on release
//   unless they were held: 1 s on key 1 = undo, 1 s on key 4 = skip, 2 s on keys 1 and 4 together = menu.
#pragma once
#include <cstdint>

namespace reka {

enum class Ev : uint8_t { None, K1, K2, K3, K4, Hold1, Hold4, Menu };

inline int keyIndex(Ev e) { return (e >= Ev::K1 && e <= Ev::K4) ? (int)e - (int)Ev::K1 : -1; }

class Keys {
 public:
  static constexpr uint32_t HOLD_MS = 1000, COMBO_MS = 2000, DEBOUNCE_MS = 20;

  // raw: keys held now. presses: keys with a new press since the last poll (Hal::presses()), which catches a release
  // and a second press that both happened while the caller was busy.
  void poll(uint32_t now, uint8_t raw, uint8_t presses = 0);
  Ev next();                          // pops the oldest event, Ev::None if there is none
  bool busy() const { return stable_ != 0 || head_ != tail_; }
  void ignoreUntilReleased() { ignore_ = true; }
  uint32_t lastActivity() const { return last_; }

 private:
  void push(Ev e);
  uint8_t stable_ = 0, candidate_ = 0;
  uint32_t candidate_since_ = 0, down_at_[4] = {0, 0, 0, 0}, combo_at_ = 0, last_ = 0;
  bool consumed_[4] = {false, false, false, false}, combo_ = false, ignore_ = false;
  Ev queue_[8];
  uint8_t head_ = 0, tail_ = 0;
};

}  // namespace reka
