#include "../microphone_flow.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace voicebot_audio;

// A genuinely fixed queue for the regression model: no extra room appears when
// the network is stalled. Production uses the same policy with FreeRTOS calls.
class FixedQueue {
 public:
  size_t spaces() const { return frames_.size() - count_; }
  size_t size() const { return count_; }
  bool overwrite(const AudioFrame& frame) {
    if (count_) {
      frames_[head_] = frame;
      return true;
    }
    if (!spaces()) return false;
    frames_[(head_ + count_) % frames_.size()] = frame;
    ++count_;
    return true;
  }
  bool receive(AudioFrame& frame) {
    if (!count_) return false;
    frame = frames_[head_];
    head_ = (head_ + 1) % frames_.size();
    --count_;
    return true;
  }
  bool enqueue(const AudioFrame& frame) {
    return overwriteMicrophone(frame, [&](const AudioFrame& value) { return overwrite(value); });
  }
 private:
  std::array<AudioFrame, kMicrophoneQueueFrames> frames_{};
  size_t head_ = 0, count_ = 0;
};

static AudioFrame microphoneFrame(uint32_t now, uint32_t generation = 17) {
  AudioFrame frame{};
  frame.generation = generation;
  frame.capturedAt = now;
  frame.length = kFrameBytes;
  return frame;
}

static void testStalledNetworkKeepsLatestAudio() {
  FixedQueue queue;
  MicrophonePacer pacer;
  const uint32_t sessionGeneration = 17;
  size_t stale = 0, captured = 0, replacements = 0, peakDepth = 0;
  std::vector<uint32_t> sentCapturedAt;

  // The consumer cannot run for 800 ms. Each new capture replaces the old
  // mailbox entry, so recovery can immediately send current rather than stale PCM.
  for (uint32_t now = 0; now <= 2000; ++now) {
    if (now % 20 == 0) {
      ++captured;
      if (!queue.spaces()) ++replacements;
      assert(queue.enqueue(microphoneFrame(now, sessionGeneration)));
      if (queue.size() > peakDepth) peakDepth = queue.size();
      assert(queue.size() <= kMicrophoneQueueFrames);
    }
    const bool networkStalled = now >= 200 && now < 1000;
    if (!networkStalled && pacer.due(now)) {
      AudioFrame frame;
      if (queue.receive(frame)) {
        if (microphoneFrameExpired(now, frame.capturedAt)) { ++stale; continue; }
        assert(frame.generation == sessionGeneration);
        assert(now - frame.capturedAt < kMicrophoneMaxAgeMs);
        sentCapturedAt.push_back(frame.capturedAt);
        pacer.sent(now, frame.length);
      }
    }
  }
  for (uint32_t now = 2020; queue.size(); now += 20) {
    assert(pacer.due(now));
    AudioFrame frame;
    assert(queue.receive(frame));
    if (microphoneFrameExpired(now, frame.capturedAt)) {
      ++stale;
    } else {
      assert(now - frame.capturedAt < kMicrophoneMaxAgeMs);
      sentCapturedAt.push_back(frame.capturedAt);
      pacer.sent(now, frame.length);
    }
  }
  assert(peakDepth == 1 && replacements > 0 && stale == 0);
  assert(captured == sentCapturedAt.size() + replacements + stale + queue.size());
  assert(queue.size() == 0);
  for (size_t i = 1; i < sentCapturedAt.size(); ++i) {
    assert(sentCapturedAt[i] > sentCapturedAt[i - 1]);
  }
  assert(sentCapturedAt[9] == 180);
  // The exact recovery-time frame is already in the mailbox and goes next.
  assert(sentCapturedAt[10] == 1000);
}

static void testAtomicLatestMailboxAndInputValidation() {
  FixedQueue queue;
  for (uint32_t now = 0; now < 100; now += 20) assert(queue.enqueue(microphoneFrame(now)));
  assert(queue.size() == 1 && queue.spaces() == 0);
  AudioFrame frame;
  assert(queue.receive(frame) && frame.capturedAt == 80);
  assert(!queue.size());

  AudioFrame invalid = microphoneFrame(0);
  invalid.length = 0;  // Continuous streaming has no end marker.
  assert(!queue.enqueue(invalid));
  invalid.length = kFrameBytes - 1;
  assert(!queue.enqueue(invalid));
  invalid.length = kFrameBytes + 2;
  assert(!queue.enqueue(invalid));

  size_t sends = 0;
  AudioFrame valid = microphoneFrame(0);
  assert(!overwriteMicrophone(valid,
      [&](const AudioFrame&) { ++sends; return false; }));
  assert(sends == 1);
  sends = 0;
  bool sendCalled = false;
  assert(overwriteMicrophone(valid,
      [&](const AudioFrame&) { ++sends; sendCalled = true; return true; }));
  assert(sends == 1);
  assert(sendCalled);  // The send callback owns the real queue result.
}

static void testAgeBoundAndRollover() {
  assert(!microphoneFrameExpired(39, 0));
  assert(microphoneFrameExpired(40, 0));
  assert(!microphoneFrameExpired(249, 0, 250));
  assert(microphoneFrameExpired(250, 0, 250));

  const uint32_t base = UINT32_MAX - 40;
  assert(!microphoneFrameExpired(base + 39, base));
  assert(microphoneFrameExpired(base + 40, base));

  // If capture stops during a long transport pause, age filtering still removes
  // the queued latest frame rather than transmitting old audio on recovery.
  FixedQueue queue;
  for (uint32_t now = 0; now < 100; now += 20) assert(queue.enqueue(microphoneFrame(now)));
  AudioFrame frame;
  size_t stale = 0;
  while (queue.receive(frame)) {
    assert(microphoneFrameExpired(1000, frame.capturedAt));
    ++stale;
  }
  assert(stale == 1);
}

static void testReplyPauseAndAutomaticResume() {
  ReplyGate gate;
  bool recordingIntent = true;
  assert(!gate.paused(0, false, false));
  gate.onAudio(11100);
  for (uint32_t now = 11100; now <= 21100; now += 20) {
    assert(gate.paused(now, true, true));
    assert(recordingIntent);
  }
  // 24 queued frames plus one in-flight is normal on the no-PSRAM board.
  const size_t speakerPending = 25;
  assert(gate.paused(22000, false, speakerPending != 0));
  assert(gate.paused(22119, false, false));
  assert(!gate.paused(22120, false, false));
  assert(recordingIntent);  // Capture resumes from existing user intent.

  // A new audio reply after an old playback tail gets its own bounded pause.
  gate.onAudio(23000);
  assert(gate.paused(23119, false, false));
  assert(!gate.paused(23120, false, false));

  gate.onAudio(34000);
  recordingIntent = false;  // Stop tap while paused remains stopped on resume.
  gate.reset();
  assert(!gate.paused(35000, false, false) && !recordingIntent);
}

static void testPauseDuringCaptureAndStopDuringReply() {
  ReplyGate gate;
  FixedQueue queue;
  const bool intent = true;
  const bool allowedBeforeRead = intent && !gate.paused(0, false, false);
  assert(allowedBeforeRead);
  gate.onAudio(10);  // Bot starts while I2S read is in flight.
  const bool allowedAfterRead = intent && !gate.paused(20, true, true);
  if (allowedBeforeRead && allowedAfterRead) assert(queue.enqueue(microphoneFrame(20)));
  assert(queue.size() == 0 && intent);

  // Continuous sessions do not put a zero-length turn marker in the queue.
  AudioFrame end{};
  assert(!queue.enqueue(end));
  assert(queue.size() == 0);
  assert(gate.paused(30, false, true));
  assert(!gate.paused(150, false, false));
}

static void testPacingAndRollover() {
  MicrophonePacer pacer;
  assert(pacer.due(0));
  pacer.sent(0, 640);
  assert(!pacer.due(19) && pacer.due(20));
  assert(pacer.due(1000));
  const uint32_t sendStartedAt = 1000;
  const uint32_t sendCompletedAt = 1080;
  pacer.sent(sendStartedAt, 640);  // TLS took 80 ms; its duration is not added.
  assert(!pacer.due(1019) && pacer.due(1020));
  assert(pacer.due(sendCompletedAt));
  pacer.sent(1100, 2);
  assert(!pacer.due(1100) && pacer.due(1101));
  pacer.sent(1200, 34);
  assert(!pacer.due(1201) && pacer.due(1202));
  pacer.reset();
  assert(pacer.due(0));

  const uint32_t base = UINT32_MAX - 5;
  pacer.sent(base, 640);
  assert(!pacer.due(base + 19) && pacer.due(base + 20));
  ReplyGate gate;
  gate.onAudio(base);
  assert(gate.paused(base + 119, false, false));
  assert(!gate.paused(base + 120, false, false));
}

int main() {
  testStalledNetworkKeepsLatestAudio();
  testAtomicLatestMailboxAndInputValidation();
  testAgeBoundAndRollover();
  testReplyPauseAndAutomaticResume();
  testPauseDuringCaptureAndStopDuringReply();
  testPacingAndRollover();
  std::puts("Microphone flow: latest-frame mailbox, bounded age, reply pause/resume and send-start pacing passed");
}
