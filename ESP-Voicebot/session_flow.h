#pragma once

#include <atomic>
#include <stdint.h>

namespace voicebot_session {

// Capture publishes physical button intent. loop() consumes the latest version,
// so even Stop -> Start while TLS is busy replaces the old call. There is no
// command queue to overflow and an old connection cannot erase a newer press.
class SessionIntent {
 public:
  uint32_t snapshot() const { return state_.load(); }
  static bool requested(uint32_t state) { return (state & 1U) != 0; }
  bool requested() const { return requested(snapshot()); }
  bool current(uint32_t token) const { return requested(token) && snapshot() == token; }

  void publish(bool start) {
    uint32_t previous = state_.load();
    while (!state_.compare_exchange_weak(previous, next(previous, start))) {}
  }

  // Only the connection associated with this token may end its own intent.
  // If the user already pressed again, leave that newer request untouched.
  bool end(uint32_t token) {
    return state_.compare_exchange_strong(token, next(token, false));
  }

  // Owned exclusively by loop(). A changed version matters even if the final
  // requested bit is unchanged: an intervening Stop must close the old call.
  bool consume(uint32_t& token) {
    token = snapshot();
    if (token == consumed_) return false;
    consumed_ = token;
    return true;
  }

 private:
  static uint32_t next(uint32_t state, bool start) {
    return ((state + 2U) & ~1U) | (start ? 1U : 0U);
  }
  std::atomic<uint32_t> state_{0};
  uint32_t consumed_ = 0;
};

// Retry only while Start is still requested. A minute of successful operation
// resets the backoff; repeated short-lived openings do not create a tight loop.
class ReconnectBackoff {
 public:
  void reset(uint32_t now) { failures_ = 0; next_ = now; opened_ = false; }
  bool due(uint32_t now) const { return static_cast<int32_t>(now - next_) >= 0; }
  void opened(uint32_t now) { opened_ = true; openedAt_ = now; }
  void maintain(uint32_t now) {
    if (opened_ && static_cast<uint32_t>(now - openedAt_) >= 60000) failures_ = 0;
  }
  uint32_t failed(uint32_t now, uint32_t jitter) {
    opened_ = false;
    uint32_t wait = failures_ < 5 ? (1000U << failures_) : 30000U;
    if (failures_ < 5) ++failures_;
    wait += jitter % 251U;
    if (wait > 30000U) wait = 30000U;
    next_ = now + wait;
    return wait;
  }

 private:
  uint8_t failures_ = 0;
  bool opened_ = false;
  uint32_t next_ = 0, openedAt_ = 0;
};

}  // namespace voicebot_session
