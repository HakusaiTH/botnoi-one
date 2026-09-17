#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "src/voice_activity/webrtc/common_audio/vad/vad_core.h"
}

// One instance belongs to one capture task. The fixed-point WebRTC classifier
// keeps its filter/noise model here; neither initialization nor frames allocate.
class VoiceActivity {
 public:
  static constexpr size_t kFrameSamples = 320;
  static constexpr int kSampleRate = 16000;

  // Modes 0..3 increase rejection of non-speech; 2 is the default compromise.
  // The optional eight-count peak floor only rejects near-digital silence
  // (~-72 dBFS). Set it to zero to disable the floor entirely.
  explicit VoiceActivity(uint8_t mode = 2, uint16_t digitalSilencePeak = 8)
      : mode_(mode), silencePeak_(digitalSilencePeak) {}

  bool begin() {
    ready_ = mode_ <= 3 && WebRtcVad_InitCore(&state_) == 0 &&
             WebRtcVad_set_mode_core(&state_, mode_) == 0;
    return ready_;
  }

  bool speech(const int16_t* pcm, size_t samples) {
    if (!ready_ || !pcm || samples != kFrameSamples) return false;
    // Always run the classifier, including silence, so its hangover and noise
    // adaptation age normally. The floor supplements the spectral classifier.
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
