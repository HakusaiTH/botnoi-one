#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace voicebot_audio {
constexpr size_t kFrameBytes = 640;
struct AudioFrame {
  uint32_t generation;
  uint32_t capturedAt;  // Microphone only: discard stale PCM after congestion.
  uint16_t length;  // Valid queued audio is nonzero, even-length PCM16.
  uint8_t pcm[kFrameBytes];
};

// PCM samples may straddle TCP chunks or WebSocket continuation frames.
// Only one byte is retained; complete samples enter a fixed queue.
class PcmAssembler {
 public:
  size_t capacity(size_t slots) const {
    return slots ? slots * kFrameBytes - (hasLowByte_ ? 1 : 0) : 0;
  }
  void reset() { hasLowByte_ = false; }
  template <typename Emit>
  bool append(const uint8_t* data, size_t length, uint32_t generation, Emit emit) {
    AudioFrame frame{};
    frame.generation = generation;
    while (length) {
      size_t used = 0;
      if (hasLowByte_) {
        frame.pcm[used++] = lowByte_;
        frame.pcm[used++] = *data++;
        --length;
        hasLowByte_ = false;
      }
      size_t copy = length < kFrameBytes - used ? length : kFrameBytes - used;
      copy &= ~size_t(1);
      memcpy(frame.pcm + used, data, copy);
      data += copy;
      length -= copy;
      used += copy;
      if (used) {
        frame.length = static_cast<uint16_t>(used);
        if (!emit(frame)) return false;
      }
      if (length == 1) {
        lowByte_ = *data++;
        hasLowByte_ = true;
        --length;
      }
    }
    return true;
  }
 private:
  uint8_t lowByte_ = 0;
  bool hasLowByte_ = false;
};

}  // namespace voicebot_audio
