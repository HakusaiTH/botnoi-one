#include "../audio_pipeline.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using namespace voicebot_audio;

static void checkStreaming(size_t chunkSize, size_t slots) {
  PcmAssembler assembler;
  std::vector<uint8_t> input(437912), output;
  for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<uint8_t>((i * 17) ^ (i >> 8));
  size_t offset = 0;
  while (offset < input.size()) {
    const size_t length = std::min({chunkSize, input.size() - offset, assembler.capacity(slots)});
    size_t emitted = 0;
    assert(length);
    assert(assembler.append(input.data() + offset, length, 23, [&](const AudioFrame& frame) {
      assert(frame.generation == 23);
      assert(frame.length && frame.length <= kFrameBytes && !(frame.length & 1));
      assert(++emitted <= slots);
      output.insert(output.end(), frame.pcm, frame.pcm + frame.length);
      return true;
    }));
    offset += length;
  }
  assert(output == input);
  assert(assembler.capacity(0) == 0);
}

int main() {
  for (size_t chunk : {size_t(1), size_t(3), size_t(503), size_t(639), size_t(640), size_t(1024)}) {
    for (size_t slots : {size_t(1), size_t(2), size_t(24), size_t(800)}) checkStreaming(chunk, slots);
  }
  PcmAssembler assembler;
  uint8_t bytes[] = {0x11, 0x22, 0x33};
  assert(assembler.append(bytes, 1, 1, [](const AudioFrame&) { assert(false); return true; }));
  assert(assembler.capacity(1) == 639);
  assembler.reset();
  assert(assembler.capacity(1) == 640);
  assert(assembler.append(bytes + 1, 2, 2, [](const AudioFrame& frame) {
    assert(frame.generation == 2 && frame.length == 2);
    assert(frame.pcm[0] == 0x22 && frame.pcm[1] == 0x33);
    return true;
  }));
  assert(!assembler.append(bytes, 2, 3, [](const AudioFrame&) { return false; }));
  SilenceTail tail;
  assert(!tail.active() && !tail.due(0));
  uint32_t now = UINT32_MAX - 40;
  tail.start(now);
  for (size_t i = 0; i < 25; ++i) {
    assert(tail.active() && tail.due(now));
    tail.sent(now);
    assert(!tail.due(now));
    assert(!tail.due(now + 19));
    now += 20;
  }
  assert(!tail.active() && !tail.due(now));
  tail.start(10);
  assert(tail.due(1000));
  tail.sent(1000);
  assert(!tail.due(1000) && !tail.due(1019) && tail.due(1020));
  tail.reset();
  assert(!tail.active());
  std::puts("Audio pipeline: long PCM, odd boundaries, queue capacity, cancel and paced silence passed");
}
