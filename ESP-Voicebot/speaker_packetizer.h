#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_pipeline.h"

namespace voicebot_audio {

// Network chunks are unrelated to the speaker's 10 ms DMA blocks. Keep partial
// blocks across AudioFrames so short chunks cannot insert silence into speech.
// The player owns this object and its current AudioFrame/sample offset. After
// submit() succeeds, or playback is cancelled, it calls reset(). A failed submit
// leaves this buffer intact. Only the driver pads the final partial block, after
// the player has observed 20 ms without new PCM; this class never pads audio.
class SpeakerPacketizer {
 public:
  static constexpr size_t kCapacitySamples = 160;

  explicit SpeakerPacketizer(int gainPercent = 70)
      : gainPercent_(gainPercent < 0 ? 0 : gainPercent > 100 ? 100 : gainPercent) {}

  // A gain change takes effect at a packet boundary, never within pending PCM.
  bool setGainPercent(int gainPercent) {
    if (used_ || gainPercent < 0 || gainPercent > 100) return false;
    gainPercent_ = gainPercent;
    return true;
  }

  size_t append(const AudioFrame& frame, size_t sampleOffset) {
    if (!frame.length || frame.length > sizeof(frame.pcm) || (frame.length & 1)) return 0;
    const size_t frameSamples = frame.length / sizeof(int16_t);
    if (sampleOffset >= frameSamples || full()) return 0;
    if (used_ && frame.generation != generation_) return 0;

    const size_t available = frameSamples - sampleOffset;
    const size_t space = kCapacitySamples - used_;
    const size_t count = available < space ? available : space;
    if (!used_) generation_ = frame.generation;
    for (size_t i = 0; i < count; ++i) {
      const size_t byteOffset = (sampleOffset + i) * sizeof(int16_t);
      const uint16_t packed = static_cast<uint16_t>(frame.pcm[byteOffset]) |
          (static_cast<uint16_t>(frame.pcm[byteOffset + 1]) << 8);
      const int32_t sample = packed & 0x8000U ? static_cast<int32_t>(packed) - 65536 : packed;
      // Gain is clamped to 0..100. The product fits int32_t even for -32768,
      // and the result remains within PCM16 bounds without wrapping/clipping.
      samples_[used_ + i] = static_cast<int16_t>(sample * gainPercent_ / 100);
    }
    used_ += count;
    return count;
  }

  size_t size() const { return used_; }
  bool full() const { return used_ == kCapacitySamples; }
  uint32_t generation() const { return generation_; }
  int gainPercent() const { return gainPercent_; }
  const int16_t* data() const { return samples_; }

  void reset() {
    used_ = 0;
    generation_ = 0;
  }

 private:
  alignas(16) int16_t samples_[kCapacitySamples]{};
  size_t used_ = 0;
  uint32_t generation_ = 0;
  int gainPercent_;
};

}  // namespace voicebot_audio
