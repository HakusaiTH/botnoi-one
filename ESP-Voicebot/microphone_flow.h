#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_pipeline.h"

namespace voicebot_audio {

// I2S writes may return shortly before the last DMA sample leaves the pin.
// Keep a small acoustic/DMA guard without adding a human-visible turn delay.
constexpr uint32_t kPlaybackDrainGuardMs = 120;

// Owned by loop(). The capture task reads the resulting pause decision through
// an atomic flag; recording intent remains separate so it survives a bot reply.
class ReplyGate {
 public:
  void reset() {
    playing_ = false;
    lastActivity_ = 0;
  }

  void onAudio(uint32_t now) {
    playing_ = true;
    lastActivity_ = now;
  }

  bool paused(uint32_t now, bool receivingBinary, bool speakerPending) {
    if (receivingBinary || speakerPending) {
      onAudio(now);
      return true;
    }
    if (!playing_) return false;
    if (elapsed(now, lastActivity_) < kPlaybackDrainGuardMs) return true;
    reset();
    return false;
  }

 private:
  static uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }
  bool playing_ = false;
  uint32_t lastActivity_ = 0;
};

// A one-slot FreeRTOS queue is used as a latest-frame mailbox. The capture task
// can xQueueOverwrite it atomically while loop() is busy in TLS, so recovery
// never starts by transmitting the beginning of an old backlog.
constexpr size_t kMicrophoneQueueFrames = 1;
constexpr uint32_t kMicrophoneFrameMs = 20;
constexpr uint32_t kMicrophoneMaxAgeMs = 2 * kMicrophoneFrameMs;

// Schedule from the start of each successful write. TLS time is therefore not
// added to every 20 ms packet interval. A write that takes longer than its PCM
// duration leaves the next packet immediately due instead of slowing the stream.
class MicrophonePacer {
 public:
  void reset() { scheduled_ = false; next_ = 0; }
  bool due(uint32_t now) const {
    return !scheduled_ || static_cast<int32_t>(now - next_) >= 0;
  }
  void sent(uint32_t sendStartedAt, size_t pcmBytes) {
    // PCM16, mono, 16 kHz = 32 bytes/ms. Division before rounding avoids an
    // addition overflow; actual packets are bounded by AudioFrame's 640 bytes.
    const size_t duration = pcmBytes / 32 + (pcmBytes % 32 != 0);
    next_ = sendStartedAt + static_cast<uint32_t>(duration);
    scheduled_ = true;
  }

 private:
  bool scheduled_ = false;
  uint32_t next_ = 0;
};

// xQueueOverwrite is valid only for a one-slot queue and is atomic against the
// loop task's xQueueReceive. The callback returns the real mailbox result.
template <typename Overwrite>
bool overwriteMicrophone(const AudioFrame& frame, Overwrite overwrite) {
  if (!frame.length || frame.length > kFrameBytes || (frame.length & 1)) return false;
  return overwrite(frame);
}

// Drop stale PCM after a transport pause even when capture has stopped and the
// latest-frame replacement policy is no longer advancing the mailbox.
inline bool microphoneFrameExpired(uint32_t now, uint32_t capturedAt,
                                   uint32_t maxAgeMs = kMicrophoneMaxAgeMs) {
  return static_cast<uint32_t>(now - capturedAt) >= maxAgeMs;
}

}  // namespace voicebot_audio
