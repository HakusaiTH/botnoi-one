#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "src/voice_activity/webrtc/common_audio/vad/vad_core.h"
}

// WebRTC Voice Activity Detector for ESP32-S3 (16kHz PCM16, 20ms frame = 320 samples)
class VoiceActivity {
 public:
  static constexpr size_t kFrameSamples = 320;
  static constexpr int kSampleRate = 16000;

  explicit VoiceActivity(uint8_t mode = 2, uint16_t digitalSilencePeak = 8)
      : mode_(mode), silencePeak_(digitalSilencePeak) {}

  bool begin() {
    ready_ = mode_ <= 3 && WebRtcVad_InitCore(&state_) == 0 &&
             WebRtcVad_set_mode_core(&state_, mode_) == 0;
    return ready_;
  }

  bool speech(const int16_t* pcm, size_t samples) {
    if (!ready_ || !pcm || samples != kFrameSamples) return false;
    const bool voiced = WebRtcVad_CalcVad16khz(&state_, pcm, kFrameSamples) > 0;
    if (!voiced || silencePeak_ == 0) return voiced;
    int32_t peak = 0;
    for (size_t i = 0; i < kFrameSamples; ++i) {
      const int32_t sample = pcm[i];
      const int32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > peak) peak = magnitude;
    }
    return peak > silencePeak_;
  }

  void reset() { begin(); }

 private:
  VadInstT state_{};
  uint8_t mode_;
  uint16_t silencePeak_;
  bool ready_ = false;
};
