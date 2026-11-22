#include "keys.h"

namespace reka {

void Keys::push(Ev e) {
  uint8_t n = (tail_ + 1) % 8;
  if (n == head_) return;  // full, drop
  queue_[tail_] = e;
  tail_ = n;
}

Ev Keys::next() {
  if (head_ == tail_) return Ev::None;
  Ev e = queue_[head_];
  head_ = (head_ + 1) % 8;
  return e;
}

void Keys::poll(uint32_t now, uint8_t raw, uint8_t presses) {
  raw &= 0x0F;
  // A key that was down when the device got busy, then let go and pressed again before it looked, reads as held
  // the whole time: no release, so the second tap vanished (tap 3 to reveal, tap 3 again during the refresh to grade).
  // A new press on a key already stable down is that release and press. Keys 2 and 3 act again; keys 1 and 4 finish
  // the tap they were in (unless it was a hold) and start a new one.
  uint8_t again = presses & stable_ & raw;
  if (again && !ignore_ && !combo_) {
    for (int i = 0; i < 4; i++) {
      if (!(again & (1 << i))) continue;
      if (i == 1 || i == 2) {
        push(i == 1 ? Ev::K2 : Ev::K3);
      } else {
        if (!consumed_[i]) push(i == 0 ? Ev::K1 : Ev::K4);
        consumed_[i] = false;
      }
      down_at_[i] = now;
      last_ = now;
    }
  }
  // The key that woke us was usually let go before the first poll (a tap is shorter than booting). Nothing is
  // held, so there is nothing left to ignore; waiting for a release here would swallow the next real tap instead.
  if (ignore_ && raw == 0 && stable_ == 0 && candidate_ == 0) ignore_ = false;
  if (raw != candidate_) {
    candidate_ = raw;
    candidate_since_ = now;
  }
  if (candidate_ != stable_ && now - candidate_since_ >= DEBOUNCE_MS) {
    uint8_t was = stable_;
    stable_ = candidate_;
    last_ = now;
    if (ignore_) {
      if (stable_ == 0) {                 // let go: the next press counts
        ignore_ = false;
        return;
      }
      if ((stable_ & 0x09) != 0x09) return;
      // The menu chord isn't a card answer, so the wake key doesn't swallow it. Held through the wake (the boot and
      // the first picture took longer than the hold), it opens the menu as soon as it's seen; joined to a key that
      // was already down, it's an ordinary 2 s hold from now. Letting go early fires nothing: combo_ is set.
      ignore_ = false;
      combo_ = true;
      // non-zero either way (0 means "fired"), and never past now: now - combo_at_ mustn't wrap
      combo_at_ = was == 0 ? (now - COMBO_MS - 1) | 1 : now;
    }
    for (int i = 0; i < 4; i++) {
      bool down = stable_ & (1 << i), wasDown = was & (1 << i);
      if (down && !wasDown) {
        down_at_[i] = now;
        consumed_[i] = false;
        if (i == 1 || i == 2) {                       // keys 2 and 3 act right away
          push(i == 1 ? Ev::K2 : Ev::K3);
          consumed_[i] = true;
        }
      } else if (!down && wasDown) {
        if (!consumed_[i] && !combo_) push(i == 0 ? Ev::K1 : Ev::K4);
        consumed_[i] = true;
      }
    }
    bool both = (stable_ & 0x09) == 0x09;
    if (both && !combo_) {
      combo_ = true;
      combo_at_ = now;
    }
    if (stable_ == 0) combo_ = false;
  }
  if (ignore_) return;
  // A change still settling (debounce) means the holds below would judge a stale picture: after a busy loop (a
  // refresh, a server call), key 4 of a 1+4 menu chord shows up here 20 ms after key 1's hold has "lasted" a second,
  // and the hold fired Undo instead of waiting for the chord.
  if (candidate_ != stable_) return;
  if (combo_) {
    if ((stable_ & 0x09) == 0x09 && combo_at_ && now - combo_at_ >= COMBO_MS) {
      push(Ev::Menu);
      combo_at_ = 0;                                    // once per hold
      consumed_[0] = consumed_[3] = true;
    }
    return;
  }
  if (stable_ == 0x01 && !consumed_[0] && now - down_at_[0] >= HOLD_MS) {
    push(Ev::Hold1);
    consumed_[0] = true;
  }
  if (stable_ == 0x08 && !consumed_[3] && now - down_at_[3] >= HOLD_MS) {
    push(Ev::Hold4);
    consumed_[3] = true;
  }
}

}  // namespace reka
