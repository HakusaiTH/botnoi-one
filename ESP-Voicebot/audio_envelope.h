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

  Envelope() : config_(speakerDefaults()) {}
  explicit Envelope(const Config& config) : config_(config) {}

  void reset() {
    level_ = 0;
    target_ = 0;
    started_ = false;
    haveTarget_ = false;
  }

  // Feeds one block of mono PCM16 and returns the smoothed level.
  uint8_t push(uint32_t now, const int16_t* pcm, size_t samples) {
    if (pcm && samples) {
      uint32_t sum = 0;
      for (size_t i = 0; i < samples; ++i) {
        const int32_t sample = pcm[i];  // Widened first: -32768 has no int16 magnitude.
        sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
      }
      target_ = scale(static_cast<uint32_t>(sum / samples));
      targetAt_ = now;
      haveTarget_ = true;
    }
    return step(now);
  }

  // Called by a producer that has no audio to submit. Without this the mouth
  // would hold the last block's amplitude through a gap between utterances.
  uint8_t decay(uint32_t now) { return step(now); }

  uint8_t level() const { return level_; }

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
      return level_;
    }
    if (haveTarget_ && static_cast<uint32_t>(now - targetAt_) >= config_.holdMs) {
      target_ = 0;
      haveTarget_ = false;
    }
    const uint32_t elapsed = static_cast<uint32_t>(now - lastAt_);
    lastAt_ = now;
    if (!elapsed || target_ == level_) return level_;
    const uint16_t span = target_ > level_ ? config_.attackMs : config_.releaseMs;
    // One step of `span` closes the whole remaining distance; repeated shorter
    // steps close a fraction each, which is the usual exponential approach.
    const uint32_t fraction = (!span || elapsed >= span) ? 255u : elapsed * 255u / span;
    const uint32_t distance = target_ > level_ ? static_cast<uint32_t>(target_ - level_)
                                              : static_cast<uint32_t>(level_ - target_);
    // Rounding up guarantees progress, so a slow poll cannot stall the level.
    const uint32_t move = (distance * fraction + 254u) / 255u;
    level_ = static_cast<uint8_t>(target_ > level_ ? level_ + move : level_ - move);
    return level_;
  }

  Config config_;
  uint8_t level_ = 0;
  uint8_t target_ = 0;
  bool started_ = false;
  bool haveTarget_ = false;
  uint32_t lastAt_ = 0;
  uint32_t targetAt_ = 0;
};

}  // namespace voicebot_face
