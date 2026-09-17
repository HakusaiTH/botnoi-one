#include "../speaker_packetizer.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace voicebot_audio;

static AudioFrame frameFrom(const int16_t* samples, size_t count, uint32_t generation) {
  assert(count <= kFrameBytes / 2);
  AudioFrame frame{};
  frame.generation = generation;
  frame.length = static_cast<uint16_t>(count * 2);
  for (size_t i = 0; i < count; ++i) {
    const uint16_t value = static_cast<uint16_t>(samples[i]);
    frame.pcm[2 * i] = static_cast<uint8_t>(value);
    frame.pcm[2 * i + 1] = static_cast<uint8_t>(value >> 8);
  }
  return frame;
}

static std::vector<int16_t> sourcePcm(size_t count) {
  std::vector<int16_t> pcm;
  pcm.reserve(count);
  uint32_t random = 12345;
  for (size_t i = 0; i < count; ++i) {
    random = random * 1664525U + 1013904223U;
    pcm.push_back(static_cast<int16_t>(static_cast<int32_t>(random & 65535U) - 32768));
  }
  return pcm;
}

static void appendFrame(SpeakerPacketizer& packetizer, const AudioFrame& frame,
                        std::vector<int16_t>& played) {
  size_t offset = 0;
  while (offset < frame.length / 2U) {
    if (packetizer.full()) {
      // A congested driver leaves the same packet and sample offset pending.
      int16_t pending[160];
      std::memcpy(pending, packetizer.data(), sizeof(pending));
      for (int retry = 0; retry < 3; ++retry) {
        assert(packetizer.append(frame, offset) == 0);
        assert(packetizer.full());
        assert(std::memcmp(pending, packetizer.data(), sizeof(pending)) == 0);
      }
      played.insert(played.end(), packetizer.data(), packetizer.data() + packetizer.size());
      packetizer.reset();
    }
    const size_t previous = packetizer.size();
    const size_t consumed = packetizer.append(frame, offset);
    assert(consumed > 0 && consumed <= 160 - previous);
    assert(packetizer.size() == previous + consumed);
    offset += consumed;
  }
}

static void assertExactPlayback(const std::vector<int16_t>& source,
                                const std::vector<size_t>& frameSizes) {
  SpeakerPacketizer packetizer;
  std::vector<int16_t> played;
  size_t position = 0;
  size_t frameIndex = 0;
  while (position < source.size()) {
    const size_t count = std::min(frameSizes[frameIndex++ % frameSizes.size()], source.size() - position);
    const auto frame = frameFrom(source.data() + position, count, 42);
    appendFrame(packetizer, frame, played);
    position += count;
  }
  // Full packets contain exactly 160 samples. A final partial packet remains
  // unpadded until the player's idle timer expires and the driver accepts it.
  assert(played.size() % 160 == 0);
  assert(packetizer.size() == (source.size() % 160 ? source.size() % 160 : 160));
  played.insert(played.end(), packetizer.data(), packetizer.data() + packetizer.size());
  assert(played.size() == source.size());
  for (size_t i = 0; i < source.size(); ++i) {
    assert(played[i] == static_cast<int32_t>(source[i]) * 70 / 100);
  }
}

static void testArbitraryNetworkChunkBoundaries() {
  // A long stream with every chunk size from one through 320 samples, repeated
  // four times, plus a final tail. No network boundary may add silence.
  const auto longSource = sourcePcm(4 * 320 * 321 / 2 + 73);
  std::vector<size_t> varyingSizes;
  for (size_t count = 1; count <= 320; ++count) varyingSizes.push_back(count);
  assertExactPlayback(longSource, varyingSizes);

  const auto shortSource = sourcePcm(20 * 160 + 19);
  for (size_t count = 1; count <= 320; ++count) assertExactPlayback(shortSource, {count});
  assertExactPlayback(sourcePcm(3200), {1, 319, 27, 133, 320});
}

static void testGenerationAndCancellation() {
  const auto source = sourcePcm(320);
  SpeakerPacketizer packetizer(100);
  auto oldFrame = frameFrom(source.data(), 80, UINT32_MAX);
  auto newFrame = frameFrom(source.data() + 80, 100, 0);
  assert(packetizer.append(oldFrame, 0) == 80);
  assert(packetizer.generation() == UINT32_MAX);
  int16_t pending[80];
  std::memcpy(pending, packetizer.data(), sizeof(pending));
  assert(packetizer.append(newFrame, 0) == 0);
  assert(packetizer.size() == 80 && packetizer.generation() == UINT32_MAX);
  assert(std::memcmp(pending, packetizer.data(), sizeof(pending)) == 0);

  packetizer.reset(); // Stop/barge-in discards the old partial packet.
  assert(packetizer.size() == 0 && !packetizer.full());
  assert(packetizer.append(newFrame, 0) == 100);
  assert(packetizer.generation() == 0);
  assert(std::memcmp(source.data() + 80, packetizer.data(), 100 * sizeof(int16_t)) == 0);
  assert(packetizer.append(oldFrame, 0) == 0);

  packetizer.reset();
  const auto fullFrame = frameFrom(source.data(), 320, 7);
  assert(packetizer.append(fullFrame, 0) == 160);
  assert(packetizer.append(fullFrame, 160) == 0);
  packetizer.reset(); // Successful submit: continue the same frame's remainder.
  assert(packetizer.append(fullFrame, 160) == 160);
  assert(packetizer.generation() == 7);
  assert(std::memcmp(source.data() + 160, packetizer.data(), 160 * sizeof(int16_t)) == 0);
}

static void testGainAndMalformedInput() {
  const int16_t extremes[]{INT16_MIN, -32767, -1, 0, 1, INT16_MAX};
  const auto frame = frameFrom(extremes, 6, 9);
  for (int gain : {0, 1, 70, 100}) {
    SpeakerPacketizer packetizer(gain);
    assert(packetizer.append(frame, 0) == 6 && packetizer.size() == 6);
    for (size_t i = 0; i < 6; ++i) {
      assert(packetizer.data()[i] == static_cast<int32_t>(extremes[i]) * gain / 100);
    }
    assert(!packetizer.setGainPercent(50));
    assert(packetizer.gainPercent() == gain);
    packetizer.reset();
    assert(packetizer.setGainPercent(50));
    assert(!packetizer.setGainPercent(-1) && !packetizer.setGainPercent(101));
    assert(packetizer.gainPercent() == 50);
  }
  assert(SpeakerPacketizer(-100).gainPercent() == 0);
  assert(SpeakerPacketizer(1000).gainPercent() == 100);

  SpeakerPacketizer packetizer;
  assert(packetizer.append(frame, 6) == 0);
  assert(packetizer.append(frame, std::numeric_limits<size_t>::max()) == 0);
  AudioFrame invalid = frame;
  invalid.length = 0;
  assert(packetizer.append(invalid, 0) == 0);
  invalid.length = 3;
  assert(packetizer.append(invalid, 0) == 0);
  invalid.length = kFrameBytes + 2;
  assert(packetizer.append(invalid, 0) == 0);
  assert(packetizer.size() == 0);
  assert(packetizer.append(frame, 5) == 1);
  assert(packetizer.size() == 1 && !packetizer.full());
  assert(packetizer.data()[0] == static_cast<int32_t>(INT16_MAX) * 70 / 100);
}

int main() {
  testArbitraryNetworkChunkBoundaries();
  testGenerationAndCancellation();
  testGainAndMalformedInput();
  puts("Speaker packetizer: exact PCM across chunk boundaries, congestion, cancellation, gain and bounds passed.");
}
