#pragma once

#include <stdint.h>

// Capture-task-owned session and voice endpoint state. No Arduino, allocation,
// I/O or shared state. update() consumes one 20 ms VAD decision per call.
namespace byteplus {
namespace conversation {

enum class Action : uint8_t {
  StartSession = 1,
  StopSession = 2,
  Timeout = 4,
  StartUtterance = 8,
  EndUtterance = 16
};

struct Config {
  uint32_t idleTimeoutMs = 30000;
  uint32_t confirmSpeechMs = 100;
  uint32_t endSilenceMs = 400;
  uint32_t maxUtteranceMs = 20000;
  bool idleWhileListeningOnly = true;
};

struct Result {
  uint8_t actions = 0;
  bool enabled = false;
  bool listening = false;
  bool utteranceActive = false;

  bool has(Action action) const {
    return (actions & static_cast<uint8_t>(action)) != 0;
  }
};

class Controller {
 public:
  static constexpr uint32_t FrameMs = 20;
  static constexpr uint32_t MaxFrameGapMs = FrameMs * 3;

  explicit Controller(const Config& config = Config()) : config_(config) {}

  // buttonFallingEdge is an already-debounced edge, never a held-button level.
  // deviceReady covers Wi-Fi/clock/hardware readiness. An unavailable device
  // stops a session and cannot arm a new one. canStartUtterance gates NEW speech
  // only: after StartUtterance, ASR may close this gate while recording continues.
  // StopSession cancels input; only normal endpoint/max-length completion emits
  // EndUtterance. The caller owns PCM pre-roll, cloud cancellation and playback.
  // serverEndpoint is a confirmed ASR endpoint for the CURRENT capture
  // generation. It can end active input despite noisy local VAD; it cannot
  // start input, reactivate a session, or override cancellation/unavailability.
  Result update(uint32_t nowMs, bool buttonFallingEdge, bool deviceReady,
                bool canStartUtterance, bool speech, bool serverEndpoint = false) {
    const uint32_t elapsed = clockSet_ ? nowMs - previousMs_ : 0;
    previousMs_ = nowMs;
    clockSet_ = true;
    if (!deviceReady) return stop();

    uint8_t actions = 0;
    bool justStarted = false;
    if (buttonFallingEdge) {
      if (enabled_) return stop();
      enabled_ = true;
      justStarted = true;
      idleElapsedMs_ = speechMs_ = silenceMs_ = 0;
      previousIdleEligible_ = false;
      actions = bits(Action::StartSession);
    }
    if (!enabled_) return result(actions, false);

    // Do not stitch interrupted capture into a continuous speech or silence
    // run. Wall-clock idle/max-duration limits still account for the gap.
    if (elapsed > MaxFrameGapMs) speechMs_ = silenceMs_ = 0;

    const bool idleEligible = !config_.idleWhileListeningOnly ||
        utteranceActive_ || canStartUtterance;
    // Requiring both endpoints to be eligible excludes an unknown busy interval
    // when the gate reopens. Clock subtraction works across millis() rollover.
    if (!justStarted && previousIdleEligible_ && idleEligible) {
      idleElapsedMs_ = addSaturated(idleElapsedMs_, elapsed);
    }

    bool ended = false;
    if (utteranceActive_) {
      if (speech) {
        silenceMs_ = 0;
        idleElapsedMs_ = 0;
      } else {
        silenceMs_ = addSaturated(silenceMs_, FrameMs);
      }
      if (serverEndpoint || silenceMs_ >= config_.endSilenceMs ||
          nowMs - utteranceStartedMs_ >= config_.maxUtteranceMs) {
        utteranceActive_ = false;
        speechMs_ = silenceMs_ = 0;
        ended = true;
        actions |= bits(Action::EndUtterance);
      }
    } else if (canStartUtterance) {
      speechMs_ = speech ? addSaturated(speechMs_, FrameMs) : 0;
      if (speech && speechMs_ >= config_.confirmSpeechMs) {
        utteranceActive_ = true;
        utteranceStartedMs_ = nowMs;
        idleElapsedMs_ = speechMs_ = silenceMs_ = 0;
        actions |= bits(Action::StartUtterance);
      }
    } else {
      speechMs_ = 0;
    }

    // Only confirmed speech refreshes inactivity. Transient clicks cannot keep
    // the session alive. A zero inactivity timeout disables that timer.
    if (config_.idleTimeoutMs && idleElapsedMs_ >= config_.idleTimeoutMs) {
      Result stopped = stop();
      stopped.actions |= bits(Action::Timeout);
      return stopped;
    }
    const bool listening = utteranceActive_ || (canStartUtterance && !ended);
    previousIdleEligible_ = !config_.idleWhileListeningOnly || listening;
    return result(actions, listening);
  }

  // Hardware faults and explicit caller shutdown use this same cancellation
  // path. Repeated stop() is inert; availability alone never enables a session.
  Result stop() {
    const uint8_t actions = enabled_ ? bits(Action::StopSession) : 0;
    enabled_ = utteranceActive_ = previousIdleEligible_ = clockSet_ = false;
    idleElapsedMs_ = speechMs_ = silenceMs_ = 0;
    return result(actions, false);
  }

  bool enabled() const { return enabled_; }
  bool utteranceActive() const { return utteranceActive_; }

 private:
  Config config_;
  bool enabled_ = false, utteranceActive_ = false;
  bool clockSet_ = false, previousIdleEligible_ = false;
  uint32_t previousMs_ = 0, utteranceStartedMs_ = 0;
  uint32_t idleElapsedMs_ = 0, speechMs_ = 0, silenceMs_ = 0;

  static uint8_t bits(Action action) { return static_cast<uint8_t>(action); }

  static uint32_t addSaturated(uint32_t value, uint32_t increment) {
    return increment > UINT32_MAX - value ? UINT32_MAX : value + increment;
  }

  Result result(uint8_t actions, bool listening) const {
    Result value;
    value.actions = actions;
    value.enabled = enabled_;
    value.listening = enabled_ && listening;
    value.utteranceActive = utteranceActive_;
    return value;
  }
};

}  // namespace conversation
}  // namespace byteplus
