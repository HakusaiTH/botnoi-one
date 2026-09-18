#pragma once

#include <stddef.h>
#include <stdint.h>

namespace voicebot_face {

// Amplitude follower that drives the animated mouth. Mean absolute magnitude is
// cheap enough to run inside the capture and playback tasks. Attack and release
// are exponential with their time constants in milliseconds, so the level rises
// and falls at the same rate whether it is fed 10 ms microphone blocks or 70 ms
// speaker blocks. One instance per producing task; the resulting 0..255 level is
// published through an atomic.
class Envelope {
 public:
  struct Config {
    uint16_t floorAmplitude;  // PCM16 magnitude still treated as silence.
    uint16_t spanAmplitude;   // Magnitude above the floor that maps to 255.
    uint16_t attackMs;
    uint16_t releaseMs;
    uint16_t holdMs;  // Without a fresh block the target falls back to silence.
  };

  static Config speakerDefaults() {
    Config config = {110, 5200, 20, 95, 140};
    return config;
  }

  static Config microphoneDefaults() {
    Config config = {170, 4200, 45, 240, 140};
    return config;
  }

  Envelope() : Envelope(speakerDefaults()) {}
  explicit Envelope(const Config& config)
      : config_(config), attackRetention_(retention(config.attackMs)),
        releaseRetention_(retention(config.releaseMs)) {}

  void reset() {
    levelQ16_ = 0;
    target_ = 0;
    started_ = false;
    haveTarget_ = false;
  }

  // Feeds one block of mono PCM16 and returns the smoothed level.
  uint8_t push(uint32_t now, const int16_t* pcm, size_t samples) {
    // Advance the old target first. A fresh block cannot retroactively keep an
    // expired target alive, or act as if it played throughout a preceding gap.
    step(now);
    if (pcm && samples) {
      uint64_t sum = 0;
      for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = pcm[i];  // Widened first: -32768 has no int16 magnitude.
        sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
      }
      target_ = scale(static_cast<uint32_t>(sum / samples));
      targetAt_ = now;
      haveTarget_ = true;
      const uint32_t targetQ16 = static_cast<uint32_t>(target_) << 16;
      const uint16_t span = targetQ16 > levelQ16_ ? config_.attackMs : config_.releaseMs;
      if (!span) levelQ16_ = targetQ16;
    }
    return level();
  }

  // Called by a producer that has no audio to submit. Without this the mouth
  // would hold the last block's amplitude through a gap between utterances.
  uint8_t decay(uint32_t now) { return step(now); }

  uint8_t level() const { return static_cast<uint8_t>((levelQ16_ + 32768u) >> 16); }

 private:
  uint8_t scale(uint32_t magnitude) const {
    if (!config_.spanAmplitude || magnitude <= config_.floorAmplitude) return 0;
    const uint32_t value = (magnitude - config_.floorAmplitude) * 255u / config_.spanAmplitude;
    return value > 255u ? 255 : static_cast<uint8_t>(value);
  }

  uint8_t step(uint32_t now) {
    if (!started_) {
      started_ = true;
      lastAt_ = now;
      return level();
    }
    const uint32_t elapsed = static_cast<uint32_t>(now - lastAt_);
    if (haveTarget_) {
      const uint32_t age = static_cast<uint32_t>(lastAt_ - targetAt_);
      const uint32_t remainingHold = age < config_.holdMs ? config_.holdMs - age : 0;
      if (elapsed >= remainingHold) {
        advance(target_, remainingHold);
        target_ = 0;
        haveTarget_ = false;
        advance(0, elapsed - remainingHold);
      } else advance(target_, elapsed);
    } else advance(0, elapsed);
    lastAt_ = now;
    return level();
  }

  // One millisecond of exponential retention in Q24. Raising the same factor
  // to elapsed time makes 20x1ms and 1x20ms follow the same curve. Q16 state
  // retains sub-level progress instead of rounding every poll to a whole level.
  // span/(span+1) is a bounded integer approximation of exp(-1/span).
  static uint32_t retention(uint16_t span) {
    return static_cast<uint32_t>((static_cast<uint64_t>(span) << 24) / (span + 1u));
  }

  static uint32_t power(uint32_t base, uint32_t exponent) {
    uint32_t result = 1u << 24;
    while (exponent) { // At most 32 iterations, including a long idle gap.
      if (exponent & 1u) result = static_cast<uint32_t>((static_cast<uint64_t>(result) * base) >> 24);
      exponent >>= 1;
      if (exponent) base = static_cast<uint32_t>((static_cast<uint64_t>(base) * base) >> 24);
    }
    return result;
  }

  void advance(uint8_t target, uint32_t elapsed) {
    const uint32_t targetQ16 = static_cast<uint32_t>(target) << 16;
    if (!elapsed || targetQ16 == levelQ16_) return;
    const bool rising = targetQ16 > levelQ16_;
    const uint32_t distance = rising ? targetQ16 - levelQ16_ : levelQ16_ - targetQ16;
    const uint32_t factor = power(rising ? attackRetention_ : releaseRetention_, elapsed);
    const uint32_t remaining = static_cast<uint32_t>((static_cast<uint64_t>(distance) * factor) >> 24);
    levelQ16_ = rising ? targetQ16 - remaining : targetQ16 + remaining;
  }

  Config config_;
  uint32_t attackRetention_, releaseRetention_;
  uint32_t levelQ16_ = 0;
  uint8_t target_ = 0;
  bool started_ = false;
  bool haveTarget_ = false;
  uint32_t lastAt_ = 0;
  uint32_t targetAt_ = 0;
};

}  // namespace voicebot_face
