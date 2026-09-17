#pragma once

#include <stddef.h>
#include <stdint.h>
#include "microphone_flow.h"

namespace voicebot_audio {

enum class PlaybackAction : uint8_t { None, Started, Completed };

// Loop-owned protocol state. I2S progress is supplied as a snapshot rather than
// accessed here. Generation zero means no samples have entered I2S; the caller
// must use nonzero playback generations and clear progress after completion.
class PlaybackFlow {
 public:
  static constexpr uint32_t kReceiveIdleMs = 600;

  void reset() {
    active_ = false;
    cancelling_ = false;
    startedObserved_ = false;
    startedAcknowledged_ = false;
    endHint_ = false;
    generation_ = 0;
    lastAudioAt_ = 0;
    offered_ = PlaybackAction::None;
  }

  // False means this PCM must remain unread: cancellation has not completed,
  // or the caller attempted to replace an unfinished generation.
  bool onAudio(uint32_t now, uint32_t generation) {
    if (!generation || cancelling_ || (active_ && generation != generation_)) return false;
    active_ = true;
    generation_ = generation;
    lastAudioAt_ = now;
    // An end hint received before this new audio cannot seal these samples.
    endHint_ = false;
    offered_ = PlaybackAction::None;
    return true;
  }

  void onEndHint() {
    // Greeting text can precede the first audio byte and is not an audio end.
    if (active_ && !cancelling_) endHint_ = true;
  }

  // The caller clears queued audio and starts its drain guard only when true.
  // Repeated barge-in events retain the original generation and control state.
  bool cancel(uint32_t oldGeneration) {
    if (cancelling_) return false;
    if (!active_) generation_ = oldGeneration;
    active_ = true;
    cancelling_ = true;
    offered_ = PlaybackAction::None;
    return true;
  }

  PlaybackAction nextAction(uint32_t now, uint32_t generationActuallyStarted,
                            size_t pendingFrames, bool drainedValid,
                            uint32_t drainedAt, bool receivingAudio) {
    offered_ = PlaybackAction::None;
    if (!active_) return offered_;
    if (generationActuallyStarted && generationActuallyStarted == generation_) {
      startedObserved_ = true;
    }
    const bool drained = !pendingFrames && drainedValid &&
        static_cast<uint32_t>(now - drainedAt) >= kPlaybackDrainGuardMs;

    if (cancelling_) {
      // Incoming binary may be paused mid-frame by blocksAudio(). Waiting for
      // receivingAudio=false here would deadlock cancellation and TCP reads.
      if (!drained) return offered_;
      if (!startedObserved_) {
        reset();  // Nothing entered I2S: no fabricated playback control pair.
        return PlaybackAction::None;
      }
      offered_ = startedAcknowledged_ ? PlaybackAction::Completed : PlaybackAction::Started;
      return offered_;
    }

    if (startedObserved_ && !startedAcknowledged_) {
      offered_ = PlaybackAction::Started;
      return offered_;
    }
    const bool ended = endHint_ || static_cast<uint32_t>(now - lastAudioAt_) >= kReceiveIdleMs;
    if (drained && !receivingAudio && ended) {
      if (!startedObserved_) {
        reset();
        return PlaybackAction::None;
      }
      offered_ = PlaybackAction::Completed;
    }
    return offered_;
  }

  // Call only after the complete control message was successfully sent. A
  // blocked socket leaves the same action pending and the microphone paused.
  bool acknowledge(PlaybackAction action) {
    if (!active_ || action == PlaybackAction::None || action != offered_) return false;
    if (action == PlaybackAction::Started) {
      startedAcknowledged_ = true;
      offered_ = PlaybackAction::None;
    } else {
      reset();
    }
    return true;
  }

  bool blocksAudio() const { return cancelling_; }
  bool micPaused() const { return active_; }

 private:
  bool active_ = false;
  bool cancelling_ = false;
  bool startedObserved_ = false;
  bool startedAcknowledged_ = false;
  bool endHint_ = false;
  uint32_t generation_ = 0;
  uint32_t lastAudioAt_ = 0;
  PlaybackAction offered_ = PlaybackAction::None;
};

}  // namespace voicebot_audio
