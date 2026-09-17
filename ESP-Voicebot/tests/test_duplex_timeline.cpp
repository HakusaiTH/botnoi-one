#include "../duplex_timeline.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace voicebot_audio;

struct Rig {
  DuplexTimeline timeline;
  int32_t tx[kDuplexDmaBlocks][kDuplexSamples * 2]{};
  int32_t rx[kDuplexDmaBlocks][kDuplexSamples * 2]{};
  uint32_t clock = 0;
  uint32_t index = 0;
  Rig() { timeline.reset(); }
  void step(bool transmit = true, bool capture = true) {
    const size_t slot = index % kDuplexDmaBlocks;
    clock += 10;
    // TX DMA EOF may precede RX by its FIFO lead. Matching must use sequence,
    // never whichever callback happened most recently.
    if (transmit) timeline.sent(reinterpret_cast<uintptr_t>(tx[slot]), tx[slot], clock - 1);
    if (capture) timeline.received(reinterpret_cast<uintptr_t>(rx[slot]), rx[slot], clock);
    ++index;
  }
};

static void exact_reference_and_underrun() {
  Rig rig;
  int16_t pcm[kDuplexSamples];
  for (size_t i = 0; i < kDuplexSamples; ++i) {
    pcm[i] = static_cast<int16_t>(i * 317 - 25000);
    for (size_t slot = 0; slot < kDuplexDmaBlocks; ++slot) {
      rig.rx[slot][2 * i] = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(pcm[i])) << 16);
      rig.rx[slot][2 * i + 1] = 0x12340000; // The microphone uses only LEFT.
    }
  }
  assert(rig.timeline.submit(pcm, 159, 1));
  assert(rig.timeline.pendingBlocks() == 1);
  assert(!rig.timeline.drainedValid() && !rig.timeline.startedGeneration());
  AlignedAudioBlock block;
  rig.step();  // Initial silence finishes; first PCM enters DMA descriptor 0.
  assert(rig.timeline.receive(block) && block.sequence == 0 && !block.discontinuity);
  for (size_t i = 0; i < kDuplexSamples; ++i) {
    assert(block.reference[i] == 0);
    assert(block.mic[i] == pcm[i]);
    const int16_t expected = i < 159 ? pcm[i] : 0;
    assert(static_cast<int16_t>(rig.tx[0][2 * i] >> 16) == expected);
    assert(rig.tx[0][2 * i] == rig.tx[0][2 * i + 1]);
  }
  assert(rig.timeline.pendingBlocks() == 1 && !rig.timeline.startedGeneration());
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 1);
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 2);
  rig.step();
  assert(rig.timeline.receive(block) && block.sequence == 3 && !block.discontinuity);
  for (size_t i = 0; i < kDuplexSamples; ++i) assert(block.reference[i] == (i < 159 ? pcm[i] : 0));
  assert(rig.timeline.pendingBlocks() == 0);
  assert(rig.timeline.startedGeneration() == 1);
  assert(rig.timeline.drainedValid() && rig.timeline.drainedAt() == 39);
  for (int32_t word : rig.tx[0]) assert(word == 0); // Underrun never repeats old PCM.
  assert(rig.timeline.diagnostics().stagingUnderruns == 2);
  rig.step(); assert(rig.timeline.receive(block));
  for (int16_t sample : block.reference) assert(sample == 0);
  assert(rig.timeline.submit(pcm, 160, 1)); // A temporary underrun preserves evidence.
  assert(rig.timeline.startedGeneration() == 1 && !rig.timeline.drainedValid());
  assert(!rig.timeline.clearPlaybackProgress());
  for (int i = 0; i < 4; ++i) rig.step();
  assert(rig.timeline.pendingBlocks() == 0);
  assert(rig.timeline.clearPlaybackProgress()); // Protocol completion, same generation next time.
  assert(!rig.timeline.startedGeneration() && !rig.timeline.drainedValid());
  assert(rig.timeline.submit(pcm, 160, 1));
  assert(!rig.timeline.startedGeneration());
}

static void cancel_preserves_dma_tail() {
  Rig rig;
  int16_t oldPcm[kDuplexSamples], newPcm[kDuplexSamples];
  for (size_t i = 0; i < kDuplexSamples; ++i) { oldPcm[i] = -321; newPcm[i] = 876; }
  for (size_t i = 0; i < kDuplexStagingBlocks; ++i) assert(rig.timeline.submit(oldPcm, 160, 1));
  assert(!rig.timeline.submit(oldPcm, 160, 1));
  assert(!rig.timeline.submit(nullptr, 160, 1));
  assert(!rig.timeline.submit(oldPcm, 161, 1));
  assert(!rig.timeline.submit(oldPcm, 160, 0));
  rig.step(); rig.step(); // Two old blocks in DMA, two still staged.
  rig.timeline.invalidate(2, rig.clock);
  assert(rig.timeline.pendingBlocks() == 2);
  assert(rig.timeline.diagnostics().staleStagingDrops == 2);
  assert(!rig.timeline.submit(oldPcm, 160, 1));
  assert(rig.timeline.submit(newPcm, 160, 2));
  assert(rig.timeline.pendingBlocks() == 3);
  rig.step(); // New generation enters descriptor 2.
  rig.step(); // Actual old tail on descriptor 0.
  assert(rig.timeline.startedGeneration() == 1 && rig.timeline.pendingBlocks() == 2);
  rig.step();
  assert(rig.timeline.pendingBlocks() == 1);
  rig.step();
  assert(rig.timeline.startedGeneration() == 2 && rig.timeline.pendingBlocks() == 0);
  AlignedAudioBlock block;
  for (uint32_t sequence = 0; sequence < 6; ++sequence) {
    assert(rig.timeline.receive(block) && block.sequence == sequence && !block.discontinuity);
    const int16_t expected = sequence == 3 || sequence == 4 ? -321 : sequence == 5 ? 876 : 0;
    for (int16_t sample : block.reference) assert(sample == expected);
  }
  rig.timeline.invalidate(3, 0); // Rollover timestamp zero is a valid drain.
  assert(rig.timeline.drainedValid() && rig.timeline.drainedAt() == 0);
}

static void out_of_order_callbacks_and_missing_peer() {
  Rig rig;
  AlignedAudioBlock block;
  // RX callbacks may run before TX, with task receives between each interrupt.
  rig.timeline.received(reinterpret_cast<uintptr_t>(rig.rx[0]), rig.rx[0], 10);
  assert(!rig.timeline.receive(block));
  rig.timeline.sent(reinterpret_cast<uintptr_t>(rig.tx[0]), rig.tx[0], 10);
  assert(rig.timeline.receive(block) && block.sequence == 0 && !block.discontinuity);
  rig.index = 1; rig.clock = 10;
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 1);
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 2);
  // RX EOF for block 3 is lost/coalesced. Later block identities recover the
  // sequence; stale TX reference is dropped instead of paired with wrong mic.
  rig.step(true, false);
  assert(!rig.timeline.receive(block));
  rig.step();
  assert(rig.timeline.receive(block) && block.sequence == 4 && block.discontinuity);
  assert(rig.timeline.diagnostics().alignmentDrops == 1);
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 5 && !block.discontinuity);
}

static void coalesced_full_ring_and_overflow_resync() {
  Rig rig;
  AlignedAudioBlock block;
  for (int i = 0; i < 3; ++i) { rig.step(); assert(rig.timeline.receive(block)); }
  rig.step(false, false); rig.step(false, false);
  rig.step(); // Same descriptor after a complete ring lap, not +1 block.
  assert(rig.timeline.receive(block) && block.sequence == 5 && block.discontinuity);
  rig.step(); assert(rig.timeline.receive(block) && block.sequence == 6 && !block.discontinuity);
  for (int i = 0; i < 12; ++i) rig.step();
  assert(rig.timeline.diagnostics().rxOverflows == 4);
  assert(rig.timeline.diagnostics().referenceOverflows == 4);
  assert(rig.timeline.receive(block) && block.sequence == 11 && block.discontinuity);
  for (uint32_t sequence = 12; sequence < 19; ++sequence) {
    assert(rig.timeline.receive(block) && block.sequence == sequence && !block.discontinuity);
  }
  assert(!rig.timeline.receive(block));
}

static void sequence_and_clock_wrap() {
  Rig rig;
  rig.clock = UINT32_MAX - 19;
  rig.timeline.reset(1, UINT32_MAX - 1, rig.clock);
  AlignedAudioBlock block;
  for (uint32_t i = 0; i < 5; ++i) {
    rig.step();
    assert(rig.timeline.receive(block));
    assert(block.sequence == UINT32_MAX - 1 + i);
    assert(!block.discontinuity);
    assert(block.capturedAtMillis == rig.clock);
  }
}

static void generation_publication_order() {
  Rig rig;
  int16_t pcm[kDuplexSamples]{};
  rig.timeline.invalidate(3, 10); // A newer physical action reaches us first.
  assert(rig.timeline.submit(pcm, 160, 3));
  rig.timeline.invalidate(2, 11); // Delayed earlier loop action must be ignored.
  rig.timeline.invalidate(3, 12); // Equal generation is also a no-op.
  rig.timeline.invalidate(0, 13); // Zero is never a valid playback generation.
  assert(rig.timeline.pendingBlocks() == 1);
  assert(rig.timeline.submit(pcm, 160, 3));
  assert(rig.timeline.diagnostics().staleStagingDrops == 0);
  rig.timeline.reset(UINT32_MAX);
  assert(rig.timeline.submit(pcm, 160, UINT32_MAX));
  rig.timeline.invalidate(1, 20); // The skipped-zero wrap is still newer.
  assert(rig.timeline.pendingBlocks() == 0);
  assert(rig.timeline.submit(pcm, 160, 1));
  rig.timeline.invalidate(UINT32_MAX, 21);
  assert(rig.timeline.pendingBlocks() == 1);
  assert(rig.timeline.submit(pcm, 160, 1));
}

static void startup_time_is_not_a_sample_origin() {
  for (int reverse = 0; reverse < 2; ++reverse) {
    Rig rig;
    AlignedAudioBlock block;
    for (size_t i = 0; i < kDuplexSamples; ++i) {
      rig.tx[0][2 * i] = rig.rx[0][2 * i] = 100 * 65536;
      rig.tx[1][2 * i] = rig.rx[1][2 * i] = 200 * 65536;
    }
    // A setup pause before enabling the master puts first EOFs on opposite
    // sides of a millisecond rounding boundary. Both still describe block 0.
    if (reverse) {
      rig.timeline.received(reinterpret_cast<uintptr_t>(rig.rx[0]), rig.rx[0], 24);
      assert(!rig.timeline.receive(block));
      rig.timeline.sent(reinterpret_cast<uintptr_t>(rig.tx[0]), rig.tx[0], 25);
    } else {
      rig.timeline.sent(reinterpret_cast<uintptr_t>(rig.tx[0]), rig.tx[0], 24);
      assert(!rig.timeline.receive(block));
      rig.timeline.received(reinterpret_cast<uintptr_t>(rig.rx[0]), rig.rx[0], 25);
    }
    assert(rig.timeline.receive(block) && block.sequence == 0 && block.discontinuity);
    assert(block.mic[0] == 100 && block.reference[0] == 100);
    rig.timeline.sent(reinterpret_cast<uintptr_t>(rig.tx[1]), rig.tx[1], reverse ? 35 : 34);
    rig.timeline.received(reinterpret_cast<uintptr_t>(rig.rx[1]), rig.rx[1], reverse ? 34 : 35);
    assert(rig.timeline.receive(block) && block.sequence == 1 && !block.discontinuity);
    assert(block.mic[0] == 200 && block.reference[0] == 200);
  }
}

int main() {
  static_assert(sizeof(DuplexTimeline) < 8 * 1024, "Duplex copied audio must fit a fixed internal budget");
  exact_reference_and_underrun();
  cancel_preserves_dma_tail();
  out_of_order_callbacks_and_missing_peer();
  coalesced_full_ring_and_overflow_resync();
  sequence_and_clock_wrap();
  generation_publication_order();
  startup_time_is_not_a_sample_origin();
  std::printf("Duplex timeline (%zu bytes): exact DMA reference, silence, cancellation tails, stale generations, reordered/coalesced EOF, overflow recovery and wrap passed\n", sizeof(DuplexTimeline));
}
