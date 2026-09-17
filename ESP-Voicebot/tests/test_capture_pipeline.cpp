#include "../capture_pipeline.h"
#include "../microphone_flow.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>

using namespace voicebot_audio;

static int16_t sample(const AudioFrame& frame, size_t i) {
  return static_cast<int16_t>(uint16_t(frame.pcm[2*i]) | (uint16_t(frame.pcm[2*i+1]) << 8));
}

struct TimingResult {
  size_t produced = 0, sent = 0, expired = 0, evicted = 0, peakDepth = 0;
};

static TimingResult simulateCaptureTiming(uint32_t processMs, uint32_t ageLimitMs,
                                         uint32_t socketStallMs = 0, uint32_t origin = 0) {
  struct TimedPacket { uint32_t readyAt; AudioFrame frame; };
  std::vector<TimedPacket> packets;
  CapturePipeline pipeline;
  assert(pipeline.configure(512, true));
  int16_t mic[160]{}, reference[160]{};
  uint32_t completedAt = 0;
  // Real production packetization and capture timestamps; the DSP double adds
  // only timing, since the separate target fixture checks actual echo removal.
  for (uint32_t dmaTime = 10; dmaTime <= 10000; dmaTime += 10) {
    assert(pipeline.push(mic, reference, 160, origin + dmaTime, 1, true, false,
        [&](const int16_t* input, const int16_t*, int16_t* output, size_t count) {
          assert(count == 512);
          memcpy(output, input, count * sizeof(int16_t));
          completedAt = dmaTime + processMs;
          return true;
        }, [&](const AudioFrame& frame) {
          packets.push_back({completedAt, frame});
          return true;
        }));
  }
  TimingResult result;
  result.produced = packets.size();
  std::array<AudioFrame, kMicrophoneQueueFrames> queue{};
  size_t head = 0, count = 0, nextPacket = 0;
  MicrophonePacer pacer;
  uint32_t loopAvailableAt = 0, lastSentCapture = 0;
  bool sentAny = false;
  auto send = [&](const AudioFrame& frame) {
    if (count == queue.size()) return false;
    queue[(head + count) % queue.size()] = frame;
    ++count;
    if (count > result.peakDepth) result.peakDepth = count;
    return true;
  };
  auto dropOldest = [&]() {
    if (!count) return false;
    head = (head + 1) % queue.size();
    --count;
    ++result.evicted;
    return true;
  };
  for (uint32_t now = 0; now <= 10100; ++now) {
    // Audio production continues while loop() is blocked inside a TLS write.
    while (nextPacket < packets.size() && packets[nextPacket].readyAt <= now) {
      assert(enqueueRecentMicrophone(packets[nextPacket++].frame, send, dropOldest));
    }
    assert(count <= kMicrophoneQueueFrames);
    if (now < loopAvailableAt || (now >= 2000 && now < 2000 + socketStallMs) ||
        !count || !pacer.due(origin + now)) continue;
    const AudioFrame frame = queue[head];
    head = (head + 1) % queue.size();
    --count;
    if (microphoneFrameExpired(origin + now, frame.capturedAt, ageLimitMs)) {
      ++result.expired;
      continue;
    }
    assert(static_cast<uint32_t>(origin + now - frame.capturedAt) < ageLimitMs);
    if (sentAny) assert(static_cast<int32_t>(frame.capturedAt - lastSentCapture) > 0);
    lastSentCapture = frame.capturedAt;
    sentAny = true;
    ++result.sent;
    pacer.sent(origin + now, frame.length);
    loopAvailableAt = now + 5; // A healthy write costs 5 ms, not an extra 20 ms.
  }
  assert(count == 0 && nextPacket == packets.size());
  assert(result.produced == result.sent + result.expired + result.evicted);
  return result;
}

static void testDspBatchingDoesNotExpireHealthySpeech() {
  // With 32ms AEC batching, a 40ms age cap drops one packet in four even with
  // healthy 10ms DSP /5ms socket work. No actual queue congestion is involved.
  const auto previousLimit = simulateCaptureTiming(10, 40);
  assert(previousLimit.produced == 499 && previousLimit.sent == 374);
  assert(previousLimit.expired == 125 && previousLimit.evicted == 0);
  for (uint32_t processMs : {6U, 10U, 12U, 20U, 25U}) {
    const auto healthy = simulateCaptureTiming(processMs, kMicrophoneMaxAgeMs);
    assert(healthy.produced == 499 && healthy.sent == healthy.produced);
    assert(healthy.expired == 0 && healthy.evicted == 0);
    assert(healthy.peakDepth <= kMicrophoneQueueFrames);
  }
  const auto rollover = simulateCaptureTiming(12, kMicrophoneMaxAgeMs, 0, UINT32_MAX - 100);
  assert(rollover.sent == rollover.produced && rollover.expired == 0);
  // The larger age budget remains bounded and never replays stale pre-stall
  // frames when an otherwise healthy capture stream encounters socket stalls.
  for (uint32_t stallMs : {80U, 800U, 3000U}) {
    const auto stalled = simulateCaptureTiming(10, kMicrophoneMaxAgeMs, stallMs);
    assert(stalled.sent > 0 && stalled.expired + stalled.evicted > 0);
    assert(stalled.peakDepth <= kMicrophoneQueueFrames);
  }
}

int main() {
  testDspBatchingDoesNotExpireHealthySpeech();
  CapturePipeline pipeline;
  assert(!pipeline.configure(0, true));
  assert(!pipeline.configure(513, true));
  assert(pipeline.configure(512, true));
  std::vector<AudioFrame> packets;
  size_t processed = 0;
  auto process = [&](const int16_t* mic, const int16_t* reference, int16_t* output, size_t count) {
    assert(count == 512);
    for (size_t i = 0; i < count; ++i) {
      assert(mic[i] == int16_t(processed + i));
      assert(reference[i] == -mic[i]);
    }
    memcpy(output, mic, count * sizeof(int16_t));
    processed += count;
    return true;
  };
  auto emit = [&](const AudioFrame& frame) { packets.push_back(frame); return true; };
  int16_t mic[160], reference[160];
  // 16 DMA blocks -> five actual DSP calls -> eight complete 20ms packets.
  // DSP double used here verifies plumbing; real DSP has a target fixture suite.
  for (int block = 0; block < 16; ++block) {
    for (int i = 0; i < 160; ++i) { mic[i] = block * 160 + i; reference[i] = -mic[i]; }
    assert(pipeline.push(mic, reference, 160, (block + 1) * 10, 1, true, false, process, emit));
  }
  assert(processed == 2560 && packets.size() == 8);
  for (size_t packet = 0; packet < packets.size(); ++packet) {
    assert(packets[packet].capturedAt == (packet + 1) * 20);
    assert(packets[packet].generation == 1 && packets[packet].length == 640);
    for (size_t i = 0; i < 320; ++i) assert(sample(packets[packet], i) == int16_t(packet * 320 + i));
  }

  // A new session/recording boundary cannot append an old partial packet.
  MicrophonePacketizer packetizer;
  int16_t pcm[512];
  for (auto& value : pcm) value = 7;
  packets.clear();
  assert(packetizer.append(pcm, 512, 1, 32, emit));
  for (auto& value : pcm) value = 9;
  assert(packetizer.append(pcm, 512, 2, 64, emit));
  assert(packets.size() == 2);
  assert(packets[1].generation == 2);
  for (size_t i = 0; i < 320; ++i) assert(sample(packets[1], i) == 9);
  packetizer.reset();
  packets.clear();
  assert(packetizer.append(pcm, 512, 3, 0, emit));
  assert(packets[0].capturedAt == uint32_t(0 - 12));

  auto copy = [](const int16_t* input, const int16_t*, int16_t* output, size_t count) {
    memcpy(output, input, count * sizeof(int16_t)); return true;
  };
  // A missing reference block triggers bounded output suppression without
  // stopping DSP history or mixing pre-gap samples into new network packets.
  pipeline.configure(512, true);
  packets.clear();
  int16_t refs[512]{};
  for (int frame = 0; frame < 5; ++frame) {
    assert(pipeline.push(pcm, refs, 512, (frame + 1) * 32, 4, true, frame == 0, copy, emit));
  }
  assert(packets.size() == 8);
  for (size_t i = 0; i < 2048; ++i) assert(sample(packets[i / 320], i % 320) == 0);
  for (size_t i = 2048; i < 2560; ++i) assert(sample(packets[i / 320], i % 320) == 9);

  // Keep processing reference/mic while the call is stopped. Nothing uploads.
  size_t calls = 0;
  packets.clear();
  pipeline.configure(512, true);
  for (int frame = 0; frame < 5; ++frame) {
    assert(pipeline.push(pcm, refs, 512, frame * 32, 5, false, false,
        [&](const int16_t* m, const int16_t* r, int16_t* o, size_t n) { ++calls; return copy(m, r, o, n); }, emit));
  }
  assert(calls == 5 && packets.empty());
  assert(!pipeline.push(pcm, refs, 512, 200, 5, true, false,
      [](const int16_t*, const int16_t*, int16_t*, size_t) { return false; }, emit));
  puts("Capture pipeline: DSP timing, congestion, aligned frames, continuous history, packetization, stop boundaries and resync passed.");
}
