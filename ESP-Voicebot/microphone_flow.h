#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio_pipeline.h"

namespace voicebot_audio {

// Owned by loop(). The capture task reads the resulting pause decision through
// an atomic flag; recording intent remains separate so it survives a bot reply.
class ReplyGate {
 public:
  void reset() {
    state_ = State::Idle;
    lastActivity_ = 0;
  }

  void onReplyText(uint32_t now) {
    if (state_ == State::Idle ||
        (state_ == State::Playing && elapsed(now, lastActivity_) >= kPlaybackTailMs)) {
      state_ = State::WaitingForAudio;
      lastActivity_ = now;
    }
    // Repeated text before first audio must not indefinitely extend the wait.
    // Text during actual playback does not replace the playback tail with 10 s.
  }

  void onAudio(uint32_t now) {
    state_ = State::Playing;
    lastActivity_ = now;
  }

  bool paused(uint32_t now, bool receivingBinary, bool speakerPending) {
    if (receivingBinary || speakerPending) {
      onAudio(now);
      return true;
    }
    if (state_ == State::Idle) return false;
    const uint32_t limit = state_ == State::WaitingForAudio ? kFirstAudioWaitMs : kPlaybackTailMs;
    if (elapsed(now, lastActivity_) < limit) return true;
    reset();
    return false;
  }

 private:
  enum class State : uint8_t { Idle, WaitingForAudio, Playing };
  static constexpr uint32_t kFirstAudioWaitMs = 10000;
  static constexpr uint32_t kPlaybackTailMs = 400;
  static uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }
  State state_ = State::Idle;
  uint32_t lastActivity_ = 0;
};

// Schedule from completion of each successful write. A late write can never
// trigger a catch-up burst, which would compete with the incoming bot audio.
class MicrophonePacer {
 public:
  void reset() { scheduled_ = false; next_ = 0; }
  bool due(uint32_t now) const {
    return !scheduled_ || static_cast<int32_t>(now - next_) >= 0;
  }
  void sent(uint32_t now, size_t pcmBytes) {
    // PCM16, mono, 16 kHz = 32 bytes/ms. Division before rounding avoids an
    // addition overflow; actual packets are bounded by AudioFrame's 640 bytes.
    const size_t duration = pcmBytes / 32 + (pcmBytes % 32 != 0);
    next_ = now + static_cast<uint32_t>(duration);
    scheduled_ = true;
  }

 private:
  bool scheduled_ = false;
  uint32_t next_ = 0;
};

// Exactly one capture producer. The loop is the only queue consumer. If the
// network stalls, drop the newest PCM rather than removing/reordering queued
// frames or resetting the session. Always leave one slot for the ordered end
// marker. Both callbacks must be nonblocking; send reports the real queue result.
template <typename Spaces, typename Send>
bool enqueueMicrophone(const AudioFrame& frame, Spaces spaces, Send send) {
  if (frame.length > kFrameBytes || (frame.length & 1)) return false;
  const size_t reserved = frame.length ? 1 : 0;
  if (static_cast<size_t>(spaces()) <= reserved) return false;
  return send(frame);
}

// Apply to PCM only. A zero-length end marker must still be consumed even if
// delayed, otherwise recording could remain stuck in its finishing state.
inline bool microphoneFrameExpired(uint32_t now, uint32_t capturedAt, uint32_t maxAgeMs = 250) {
  return static_cast<uint32_t>(now - capturedAt) >= maxAgeMs;
}

}  // namespace voicebot_audio
