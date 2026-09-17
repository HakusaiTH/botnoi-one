#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "audio_pipeline.h"

namespace voicebot_audio {

// AEC emits 512-sample frames; the network needs 320-sample/20ms packets.
// Retain the remainder instead of overwriting the first packet of each burst.
class MicrophonePacketizer {
 public:
  void reset() { used_ = 0; generation_ = 0; }

  template <typename Emit>
  bool append(const int16_t* pcm, size_t samples, uint32_t generation,
              uint32_t lastSampleAt, Emit emit) {
    if (!pcm || !generation) return false;
    if (generation != generation_) { reset(); generation_ = generation; }
    for (size_t i = 0; i < samples; ++i) {
      const uint16_t value = static_cast<uint16_t>(pcm[i]);
      frame_.pcm[used_++] = static_cast<uint8_t>(value);
      frame_.pcm[used_++] = static_cast<uint8_t>(value >> 8);
      if (used_ == kFrameBytes) {
        frame_.generation = generation;
        frame_.length = kFrameBytes;
        frame_.capturedAt = lastSampleAt - static_cast<uint32_t>((samples - i - 1) / 16);
        used_ = 0;
        if (!emit(frame_)) return false;
      }
    }
    return true;
  }

 private:
  AudioFrame frame_{};
  size_t used_ = 0;
  uint32_t generation_ = 0;
};

// One audio task owns this fixed storage and the AEC instance. DSP always runs,
// even when capture is not being uploaded, so speaker echo history stays valid.
// A discontinuity discards partial frames and briefly withholds output while
// the existing canceller adapts to its new history; it never reallocates DSP.
class CapturePipeline {
 public:
  static constexpr size_t kMaxFrameSamples = 512;
  bool configure(size_t frameSamples, bool aecEnabled) {
    if (!frameSamples || frameSamples > kMaxFrameSamples) return false;
    frameSamples_ = frameSamples;
    aecEnabled_ = aecEnabled;
    used_ = 0;
    recoveryFrames_ = 0;
    packetizer_.reset();
    return true;
  }

  template <typename Process, typename Emit>
  bool push(const int16_t* mic, const int16_t* reference, size_t samples,
            uint32_t lastSampleAt, uint32_t generation, bool upload,
            bool discontinuity, Process process, Emit emit) {
    if (!frameSamples_ || !mic || !reference || !generation) return false;
    if (discontinuity) {
      used_ = 0;
      packetizer_.reset();
      recoveryFrames_ = aecEnabled_ ? 4 : 0;
    }
    if (!upload || generation != frameGeneration_) packetizer_.reset();
    for (size_t i = 0; i < samples; ++i) {
      if (!used_) { frameGeneration_ = generation; frameUpload_ = upload; }
      frameUpload_ = frameUpload_ && upload && generation == frameGeneration_;
      mic_[used_] = mic[i];
      reference_[used_++] = reference[i];
      if (used_ != frameSamples_) continue;
      used_ = 0;
      if (!process(mic_, reference_, output_, frameSamples_)) return false;
      if (recoveryFrames_) {
        memset(output_, 0, frameSamples_ * sizeof(int16_t));
        --recoveryFrames_;
      }
      const uint32_t endedAt = lastSampleAt - static_cast<uint32_t>((samples - i - 1) / 16);
      if (frameUpload_ && !packetizer_.append(output_, frameSamples_, frameGeneration_, endedAt, emit)) return false;
    }
    return true;
  }

 private:
  int16_t mic_[kMaxFrameSamples]{};
  int16_t reference_[kMaxFrameSamples]{};
  int16_t output_[kMaxFrameSamples]{};
  MicrophonePacketizer packetizer_;
  size_t frameSamples_ = 0, used_ = 0;
  uint32_t frameGeneration_ = 0;
  uint8_t recoveryFrames_ = 0;
  bool frameUpload_ = false, aecEnabled_ = false;
};

}  // namespace voicebot_audio
