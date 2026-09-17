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

// A 32ms DSP frame can complete two 20ms network packets at once. Four fixed
// slots preserve that burst while loop() services TLS. A full queue evicts its
// oldest packet, and the consumer still enforces the independent age cap.
constexpr size_t kMicrophoneQueueFrames = 4;
constexpr uint32_t kMicrophoneFrameMs = 20;
// A packet's timestamp is its last captured sample, before 32ms AEC batching
// and processing. 40ms rejects healthy packets when DSP takes 10ms; 60ms leaves
// processing/pacing room while still preventing stale congestion playback.
constexpr uint32_t kMicrophoneMaxAgeMs = 3 * kMicrophoneFrameMs;

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

// There is one producer. If a concurrent consumer already made room after the
// first failed send, dropping may return false; the one retry is still valid.
template <typename Send, typename DropOldest>
bool enqueueRecentMicrophone(const AudioFrame& frame, Send send, DropOldest dropOldest) {
  if (!frame.length || frame.length > kFrameBytes || (frame.length & 1)) return false;
  if (send(frame)) return true;
  dropOldest();
  return send(frame);
}

// Drop stale PCM after a transport pause even when capture has stopped and the
// recent-frame replacement policy is no longer advancing the queue.
inline bool microphoneFrameExpired(uint32_t now, uint32_t capturedAt,
                                   uint32_t maxAgeMs = kMicrophoneMaxAgeMs) {
  return static_cast<uint32_t>(now - capturedAt) >= maxAgeMs;
}

}  // namespace voicebot_audio
