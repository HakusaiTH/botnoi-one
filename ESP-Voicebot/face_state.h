#pragma once

#include <stdint.h>

namespace voicebot_face {

// What the face is currently expressing. Published by loop() from state it
// already owns, so the render task never inspects the session or the socket.
enum class Mood : uint8_t {
  Boot,        // Lids opening once after power-up.
  Connecting,  // Wi-Fi, TLS or the Voicebot session is not usable yet.
  Idle,        // No call requested. Slow gaze drift, blinks and an occasional wink.
  Listening,   // Call open, microphone streaming. Eyes react to the user's voice.
  Thinking,    // A final transcript arrived and no reply audio has started.
  Speaking,    // Reply audio is playing. The mouth follows the speaker envelope.
  Error,       // Latched audio/pipeline fault or a firmware that never got ready.
};

// One resolved animation frame in device-independent units. FaceRenderer turns
// these into pixels; nothing here depends on the panel or its resolution.
struct FaceFrame {
  uint8_t leftEyeOpen = 255;   // 0 = closed lid line, 255 = fully open.
  uint8_t rightEyeOpen = 255;
  int8_t gazeX = 0;            // Pixel offset applied to both eyes.
  int8_t gazeY = 0;
  uint8_t mouthOpen = 0;       // 0 = closed lip line, 255 = widest.
  int8_t mouthCurve = 104;     // > 0 smiles, < 0 frowns.
};

// Sine over a 128-step phase returning -127..127. The render task carries no
// floating point, which keeps its stack and its worst-case frame time small.
inline int16_t sine(uint8_t phase) {
  static const uint8_t kQuadrant[33] = {
      0, 6, 12, 19, 25, 31, 37, 43, 49, 54, 60, 65, 71, 76, 81, 85, 90,
      94, 98, 102, 106, 109, 112, 115, 117, 120, 122, 123, 125, 126, 126, 127, 127};
  const uint8_t step = static_cast<uint8_t>(phase & 0x7F);
  const uint8_t index = static_cast<uint8_t>(step & 0x1F);
  switch (step >> 5) {
    case 0: return kQuadrant[index];
    case 1: return kQuadrant[32 - index];
    case 2: return static_cast<int16_t>(-kQuadrant[index]);
    default: return static_cast<int16_t>(-kQuadrant[32 - index]);
  }
}

// Phase of a periodic motion, as a 128-step value suitable for sine().
inline uint8_t phaseOf(uint32_t elapsedMs, uint32_t periodMs) {
  if (!periodMs) return 0;
  return static_cast<uint8_t>(static_cast<uint64_t>(elapsedMs % periodMs) * 128u / periodMs) & 0x7F;
}

// Turns a mood plus two audio envelopes into a FaceFrame. Pure apart from its
// own blink schedule, so the same inputs at the same times always draw the same
// face and the whole animation is testable on the host.
class FaceAnimator {
 public:
  struct Config {
    uint16_t wakeMs;            // Boot lid-open ramp.
    uint16_t blinkMs;           // One full close/open blink.
    uint16_t winkMs;            // A held one-eyed wink.
    uint16_t minBlinkGapMs;
    uint16_t blinkGapSpreadMs;  // Random extra gap, 0 makes blinks periodic.
    uint8_t winkEveryNthBlink;  // 0 disables winking.
    uint8_t speakingMinMouthOpen;
  };

  static Config defaults() {
    Config config = {700, 150, 520, 2600, 3400, 4, 38};
    return config;
  }

  FaceAnimator() : config_(defaults()) { reset(0); }
  explicit FaceAnimator(const Config& config) : config_(config) { reset(0); }

  // Mixes hardware entropy into the blink cadence so two devices side by side
  // do not blink in lockstep. Zero is ignored to keep the xorshift non-zero.
  void seed(uint32_t value) {
    if (value) random_ = value;
  }

  void reset(uint32_t now) {
    mood_ = Mood::Boot;
    moodAt_ = now;
    blinking_ = false;
    winking_ = false;
    blinkCount_ = 0;
    blinkStartedAt_ = now;
    scheduleBlink(now);
  }

  FaceFrame frame(uint32_t now, Mood mood, uint8_t speakerLevel, uint8_t microphoneLevel) {
    if (mood != mood_) {
      mood_ = mood;
      moodAt_ = now;
      // A mood change already redraws the eyes; do not also blink immediately.
      blinking_ = false;
      winking_ = false;
      scheduleBlink(now);
    }
    const uint32_t since = static_cast<uint32_t>(now - moodAt_);
    FaceFrame out;
    switch (mood) {
      case Mood::Boot: {
        const uint32_t open = !config_.wakeMs || since >= config_.wakeMs
                                  ? 255u
                                  : since * 255u / config_.wakeMs;
        out.leftEyeOpen = out.rightEyeOpen = static_cast<uint8_t>(open);
        out.mouthOpen = 0;
        out.mouthCurve = static_cast<int8_t>(open / 3);  // Settles into a small smile.
        break;
      }
      case Mood::Connecting:
        // Scanning left and right reads as "looking for the network".
        out.leftEyeOpen = out.rightEyeOpen = 215;
        out.gazeX = scaled(sine(phaseOf(since, 1800)), 12);
        out.gazeY = -2;
        out.mouthOpen = 0;
        out.mouthCurve = 32;
        break;
      case Mood::Idle:
        out.leftEyeOpen = out.rightEyeOpen = 240;
        out.gazeX = scaled(sine(phaseOf(since, 5200)), 6);
        out.gazeY = scaled(sine(phaseOf(since + 1300, 7100)), 4);
        out.mouthOpen = 0;
        out.mouthCurve = 104;
        break;
      case Mood::Listening:
        // Eyes widen and lift with the user's voice, the mouth stays a smile.
        out.leftEyeOpen = out.rightEyeOpen = 255;
        out.gazeX = scaled(sine(phaseOf(since, 3400)), 4);
        out.gazeY = static_cast<int8_t>(-2 - microphoneLevel / 64);
        out.mouthOpen = 0;
        out.mouthCurve = static_cast<int8_t>(100 + microphoneLevel / 10);
        break;
      case Mood::Thinking:
        out.leftEyeOpen = out.rightEyeOpen = 190;
        out.gazeX = scaled(sine(phaseOf(since, 1500)), 10);
        out.gazeY = -6;
        out.mouthOpen = 10;
        out.mouthCurve = 16;
        break;
      case Mood::Speaking: {
        out.leftEyeOpen = out.rightEyeOpen = 250;
        const uint32_t range = 255u - config_.speakingMinMouthOpen;
        out.mouthOpen = static_cast<uint8_t>(config_.speakingMinMouthOpen +
                                             speakerLevel * range / 255u);
        out.mouthCurve = 88;
        // A small bob on loud syllables keeps the head from looking frozen.
        out.gazeY = static_cast<int8_t>(speakerLevel / 96);
        break;
      }
      case Mood::Error:
        out.leftEyeOpen = out.rightEyeOpen = 72;
        out.gazeY = 4;
        out.mouthOpen = 255;  // A frown at its full bounded depth.
        out.mouthCurve = -80;
        break;
    }
    applyBlink(now, mood, out);
    return out;
  }

 private:
  static int8_t scaled(int16_t unitSine, int16_t amplitude) {
    return static_cast<int8_t>(unitSine * amplitude / 127);
  }

  static bool blinks(Mood mood) {
    return mood == Mood::Idle || mood == Mood::Listening || mood == Mood::Speaking ||
           mood == Mood::Thinking;
  }

  void applyBlink(uint32_t now, Mood mood, FaceFrame& out) {
    if (!blinks(mood)) {
      blinking_ = false;
      winking_ = false;
      return;
    }
    if (blinking_) {
      const uint32_t span = winking_ ? config_.winkMs : config_.blinkMs;
      if (static_cast<uint32_t>(now - blinkStartedAt_) >= span) {
        blinking_ = false;
        winking_ = false;
        scheduleBlink(now);
      }
    } else if (static_cast<int32_t>(now - nextBlinkAt_) >= 0) {
      blinking_ = true;
      blinkStartedAt_ = now;
      ++blinkCount_;
      winking_ = config_.winkEveryNthBlink && mood == Mood::Idle &&
                 blinkCount_ % config_.winkEveryNthBlink == 0;
    }
    if (!blinking_) return;
    const uint8_t lid = lidScale(now);
    // A wink closes one eye only, which is the product's signature expression.
    out.rightEyeOpen = static_cast<uint8_t>(out.rightEyeOpen * lid / 255);
    if (!winking_) out.leftEyeOpen = static_cast<uint8_t>(out.leftEyeOpen * lid / 255);
  }

  // 255 while open, 0 while held shut, with a linear ramp on each side. The
  // same shape serves a fast blink and a slow wink.
  uint8_t lidScale(uint32_t now) const {
    const uint32_t span = winking_ ? config_.winkMs : config_.blinkMs;
    const uint32_t elapsed = static_cast<uint32_t>(now - blinkStartedAt_);
    if (elapsed >= span) return 255;
    uint32_t ramp = span / 3;
    if (ramp > 110) ramp = 110;
    if (!ramp) return 0;
    if (elapsed < ramp) return static_cast<uint8_t>(255u - elapsed * 255u / ramp);
    if (elapsed >= span - ramp) return static_cast<uint8_t>((elapsed - (span - ramp)) * 255u / ramp);
    return 0;
  }

  void scheduleBlink(uint32_t now) {
    uint32_t gap = config_.minBlinkGapMs;
    if (config_.blinkGapSpreadMs) gap += nextRandom() % config_.blinkGapSpreadMs;
    nextBlinkAt_ = now + gap;
  }

  uint32_t nextRandom() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    return random_;
  }

  Config config_;
  Mood mood_ = Mood::Boot;
  uint32_t moodAt_ = 0;
  uint32_t nextBlinkAt_ = 0;
  uint32_t blinkStartedAt_ = 0;
  uint32_t random_ = 0x1F35A2C7u;  // Fixed seed keeps host tests reproducible.
  uint32_t blinkCount_ = 0;
  bool blinking_ = false;
  bool winking_ = false;
};

}  // namespace voicebot_face
