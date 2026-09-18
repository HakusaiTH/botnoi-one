#pragma once

#include <stddef.h>

// Shared by the physical I2S driver and display admission. Keep the GOOUUU
// ESP32-S3-CAM V1.5 audio map in one place so a wiring update cannot leave the
// screen blocked by an obsolete microphone pin.
namespace voicebot_hardware {

constexpr int kMicrophoneBclk = 42;
constexpr int kMicrophoneWs = 2;
constexpr int kMicrophoneData = 1;
constexpr int kSpeakerBclk = 38;
constexpr int kSpeakerWs = 39;
constexpr int kSpeakerData = 40;
constexpr int kStatusLed = 48;

constexpr bool audioUsesPin(int pin) {
  return pin == kMicrophoneBclk || pin == kMicrophoneWs ||
         pin == kMicrophoneData || pin == kSpeakerBclk ||
         pin == kSpeakerWs || pin == kSpeakerData;
}

// Negative display pins mean intentionally unconnected reset/backlight.
// Return the actual conflicting GPIO to make startup failures diagnosable.
inline int displayPinConflict(const int* pins, size_t count, int buttonPin,
                              int statusLedPin = kStatusLed) {
  for (size_t i = 0; i < count; ++i) {
    if (pins[i] >= 0 && (audioUsesPin(pins[i]) || pins[i] == buttonPin ||
                        pins[i] == statusLedPin)) return pins[i];
  }
  return -1;
}

}  // namespace voicebot_hardware
