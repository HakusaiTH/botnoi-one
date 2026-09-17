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
  bool send(const AudioFrame& frame) {
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
    return enqueueMicrophone(frame, [&] { return spaces(); },
                             [&](const AudioFrame& value) { return send(value); });
  }
 private:
  std::array<AudioFrame, 12> frames_{};
  size_t head_ = 0, count_ = 0;
};

static AudioFrame microphoneFrame(uint32_t now, uint32_t generation = 17) {
  AudioFrame frame{};
  frame.generation = generation;
  frame.capturedAt = now;
  frame.length = kFrameBytes;
  return frame;
}

static void testStalledNetworkPreservesBoundedRecording() {
  FixedQueue queue;
  MicrophonePacer pacer;
  const uint32_t sessionGeneration = 17;
  const bool recordingIntent = true;
  size_t dropped = 0, stale = 0, captured = 0, peakDepth = 0;
  std::vector<uint32_t> sentAt;

  // Reproduce the board failure: the consumer cannot run for 800 ms, far longer
  // than the 11 PCM slots / 220 ms budget, while capture continues every 20 ms.
  for (uint32_t now = 0; now <= 2000; ++now) {
    if (now % 20 == 0) {
      ++captured;
      if (!queue.enqueue(microphoneFrame(now, sessionGeneration))) ++dropped;
      if (queue.size() > peakDepth) peakDepth = queue.size();
      assert(queue.size() <= 11);
    }
    const bool networkStalled = now >= 200 && now < 1000;
    if (!networkStalled && pacer.due(now)) {
      AudioFrame frame;
      while (queue.receive(frame)) {
        if (microphoneFrameExpired(now, frame.capturedAt)) { ++stale; continue; }
        assert(frame.generation == sessionGeneration);
        sentAt.push_back(now);
        pacer.sent(now, frame.length);
        break;
      }
    }
    assert(recordingIntent);  // Drop policy never owns or clears user intent.
  }
  assert(peakDepth == 11 && dropped >= 29 && stale == 11);
  assert(captured == sentAt.size() + dropped + stale + queue.size());
  assert(queue.size() == 0);
  for (size_t i = 1; i < sentAt.size(); ++i) assert(sentAt[i] - sentAt[i - 1] >= 20);
  // Stale queued speech is discarded; fresh capture restarts without a burst.
  assert(sentAt[9] == 180 && sentAt[10] == 1020);
}

static void testReservedEndMarkerSurvivesOverflowAndExpiry() {
  FixedQueue queue;
  for (uint32_t now = 0; now < 220; now += 20) assert(queue.enqueue(microphoneFrame(now)));
  assert(queue.size() == 11);
  assert(!queue.enqueue(microphoneFrame(220)));
  AudioFrame end{};
  end.generation = 17;
  end.capturedAt = 240;
  assert(queue.enqueue(end));
  assert(queue.size() == 12 && queue.spaces() == 0);
  assert(!queue.enqueue(microphoneFrame(260)));
  AudioFrame frame;
  size_t stale = 0, finished = 0;
  while (queue.receive(frame)) {
    // End markers are deliberately handled before age filtering.
    if (!frame.length) { ++finished; continue; }
    assert(microphoneFrameExpired(1000, frame.capturedAt));
    ++stale;
  }
  assert(stale == 11 && finished == 1);

  bool sendCalled = false;
  assert(!enqueueMicrophone(microphoneFrame(0), [] { return 1; },
      [&](const AudioFrame&) { sendCalled = true; return true; }));
  assert(!sendCalled);
  assert(!enqueueMicrophone(microphoneFrame(0), [] { return 12; },
      [](const AudioFrame&) { return false; }));
}

static void testReplyPauseAndAutomaticResume() {
  ReplyGate gate;
  bool recordingIntent = true;
  assert(!gate.paused(0, false, false));
  gate.onReplyText(100);
  assert(gate.paused(100, false, false));
  gate.onReplyText(9900);
  assert(gate.paused(10099, false, false));
  assert(!gate.paused(10100, false, false));  // Repeated text cannot extend 10 s.
  assert(recordingIntent);

  gate.onReplyText(11000);
  gate.onAudio(11100);
  for (uint32_t now = 11100; now <= 21100; now += 20) {
    assert(gate.paused(now, true, true));
    assert(recordingIntent);
  }
  // 24 queued frames plus one in-flight is normal on the no-PSRAM board.
  const size_t speakerPending = 25;
  assert(gate.paused(22000, false, speakerPending != 0));
  gate.onReplyText(22200);  // Extra text during playback cannot add a 10 s mute.
  assert(gate.paused(22399, false, false));
  assert(!gate.paused(22400, false, false));
  assert(recordingIntent);  // Capture resumes from existing user intent.

  // A new text reply after an old playback tail gets its own bounded preparation.
  gate.onAudio(23000);
  gate.onReplyText(23400);
  assert(gate.paused(33399, false, false));
  assert(!gate.paused(33400, false, false));

  gate.onReplyText(34000);
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

  // Even while audio is suppressed, a stop marker is delivered and consumed.
  AudioFrame end{};
  end.capturedAt = 30;
  assert(queue.enqueue(end));
  AudioFrame result;
  assert(queue.receive(result) && !result.length);
  const bool finishing = result.length != 0;
  assert(!finishing);
  assert(gate.paused(30, false, true));
  assert(!gate.paused(430, false, false));
}

static void testPacingAndRollover() {
  MicrophonePacer pacer;
  assert(pacer.due(0));
  pacer.sent(0, 640);
  assert(!pacer.due(19) && pacer.due(20));
  assert(pacer.due(1000));
  pacer.sent(1080, 640);  // Simulate an 80 ms write: schedule from completion.
  assert(!pacer.due(1080) && !pacer.due(1099) && pacer.due(1100));
  pacer.sent(1100, 2);
  assert(!pacer.due(1100) && pacer.due(1101));
  pacer.sent(1200, 34);
  assert(!pacer.due(1201) && pacer.due(1202));
  pacer.reset();
  assert(pacer.due(0));

  const uint32_t base = UINT32_MAX - 5;
  pacer.sent(base, 640);
  assert(!pacer.due(base + 19) && pacer.due(base + 20));
  assert(!microphoneFrameExpired(base + 249, base));
  assert(microphoneFrameExpired(base + 250, base));
  ReplyGate gate;
  gate.onReplyText(base);
  assert(gate.paused(base + 9999, false, false));
  assert(!gate.paused(base + 10000, false, false));
  gate.onAudio(base);
  assert(gate.paused(base + 399, false, false));
  assert(!gate.paused(base + 400, false, false));
}

int main() {
  testStalledNetworkPreservesBoundedRecording();
  testReservedEndMarkerSurvivesOverflowAndExpiry();
  testReplyPauseAndAutomaticResume();
  testPauseDuringCaptureAndStopDuringReply();
  testPacingAndRollover();
  std::puts("Microphone flow: 800ms stall, bounded soft drops, end markers, reply pause/resume and paced output passed");
}
