#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
// Xtensa cannot reliably place literal pools for section-attributed inline
// COMDAT methods. Inline these bounded helpers into the adapter's two explicit
// out-of-class IRAM callbacks instead of giving each method its own section.
#define VOICEBOT_AUDIO_IRAM __attribute__((always_inline))
#else
#define VOICEBOT_AUDIO_IRAM
#endif

namespace voicebot_audio {

constexpr size_t kDuplexSamples = 160;  // 10 ms, independent of the AEC frame size.
constexpr size_t kDuplexDmaBlocks = 3;
constexpr size_t kDuplexStagingBlocks = 4;
constexpr size_t kDuplexHistoryBlocks = 8;

struct AlignedAudioBlock {
  int16_t mic[kDuplexSamples];
  int16_t reference[kDuplexSamples];
  uint32_t sequence;
  uint32_t capturedAtMillis;
  bool discontinuity;
};

struct DuplexDiagnostics {
  uint32_t rxOverflows = 0;
  uint32_t referenceOverflows = 0;
  uint32_t stagingUnderruns = 0;
  uint32_t staleStagingDrops = 0;
  uint32_t alignmentDrops = 0;
  uint32_t dmaDiscontinuities = 0;
};

// Every operation is bounded and allocation-free. The hardware adapter must
// serialize these operations against its two ISR callbacks. Keeping the actual
// callback logic here also lets host tests exercise the real PCM/accounting path.
class DuplexTimeline {
 public:
  void reset(uint32_t generation = 1, uint32_t firstSequence = 0, uint32_t now = 0) {
    stageHead_ = stageCount_ = rxHead_ = rxCount_ = refHead_ = refCount_ = 0;
    generation_ = generation;
    pending_ = startedGeneration_ = drainedAt_ = 0;
    drainedValid_ = delivered_ = discontinuity_ = false;
    expectedSequence_ = firstSequence;
    txCursor_.reset(firstSequence, now);
    rxCursor_.reset(firstSequence, now);
    diagnostics_ = DuplexDiagnostics{};
    for (size_t i = 0; i < kDuplexDmaBlocks; ++i) {
      dma_[i].address = 0;
      dma_[i].generation = 0;
      dma_[i].valid = false;
    }
  }

  bool VOICEBOT_AUDIO_IRAM submit(const int16_t* pcm, size_t samples, uint32_t generation) {
    if (!pcm || !samples || samples > kDuplexSamples || !generation ||
        generation != generation_ || stageCount_ == kDuplexStagingBlocks) return false;
    drainedValid_ = false;
    // Publish accounting before making the staged block available to the ISR.
    ++pending_;
    Stage& slot = stage_[(stageHead_ + stageCount_) % kDuplexStagingBlocks];
    slot.generation = generation;
    for (size_t i = 0; i < kDuplexSamples; ++i) slot.pcm[i] = i < samples ? pcm[i] : 0;
    ++stageCount_;
    return true;
  }

  void VOICEBOT_AUDIO_IRAM invalidate(uint32_t generation, uint32_t now) {
    // Physical Stop and the protocol loop can publish successive generations
    // on different cores. A delayed older publication must not cancel newer
    // PCM or permanently make all future submits look stale.
    if (!generation || static_cast<int32_t>(generation - generation_) <= 0) return;
    generation_ = generation;
    // Only staged PCM can be cancelled immediately. DMA PCM may already be in
    // the peripheral FIFO: retain its reference and pending count until EOF.
    while (stageCount_) {
      stageHead_ = (stageHead_ + 1) % kDuplexStagingBlocks;
      --stageCount_;
      ++diagnostics_.staleStagingDrops;
      release(now);
    }
    if (!pending_) { drainedAt_ = now; drainedValid_ = true; }
  }

  // Called before overwriting the exact DMA buffer that just completed. Stereo
  // words contain postgain PCM16 in their high bits. Zero-padding, underruns and
  // cancelled DMA tails therefore have precisely the same software reference.
  void VOICEBOT_AUDIO_IRAM sent(uintptr_t address, int32_t* stereo, uint32_t now) {
    bool gap = false;
    const uint32_t sequence = txCursor_.next(address, now, gap);
    if (gap) { ++diagnostics_.dmaDiscontinuities; discontinuity_ = true; }
    SampleBlock& reference = pushReference();
    reference.sequence = sequence;
    reference.capturedAt = now;
    for (size_t i = 0; i < kDuplexSamples; ++i) reference.pcm[i] = static_cast<int16_t>(stereo[2 * i] >> 16);

    DmaBlock* metadata = nullptr;
    for (size_t i = 0; i < kDuplexDmaBlocks; ++i) {
      if (dma_[i].address == address) { metadata = &dma_[i]; break; }
    }
    if (!metadata) {
      for (size_t i = 0; i < kDuplexDmaBlocks; ++i) {
        if (!dma_[i].address) { metadata = &dma_[i]; metadata->address = address; break; }
      }
    }
    if (metadata && metadata->valid) {
      startedGeneration_ = metadata->generation;
      metadata->valid = false;
      release(now);
    }
    // Both driver auto-clear flags are disabled: completely overwrite the
    // completed buffer every time, including when there is no staged PCM.
    for (size_t i = 0; i < kDuplexSamples * 2; ++i) stereo[i] = 0;
    while (stageCount_ && stage_[stageHead_].generation != generation_) {
      stageHead_ = (stageHead_ + 1) % kDuplexStagingBlocks;
      --stageCount_;
      ++diagnostics_.staleStagingDrops;
      release(now);
    }
    if (metadata && stageCount_) {
      const Stage& slot = stage_[stageHead_];
      metadata->generation = slot.generation;
      metadata->valid = true;  // Move ownership; pending is unchanged.
      for (size_t i = 0; i < kDuplexSamples; ++i) {
        const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(slot.pcm[i])) << 16);
        stereo[2 * i] = sample;
        stereo[2 * i + 1] = sample;
      }
      stageHead_ = (stageHead_ + 1) % kDuplexStagingBlocks;
      --stageCount_;
    } else {
      // Silence while idle is expected. Count underruns only when a previously
      // accepted TTS block remains in DMA and the staging producer fell behind.
      if (pending_) ++diagnostics_.stagingUnderruns;
    }
  }

  void VOICEBOT_AUDIO_IRAM received(uintptr_t address, const int32_t* stereo, uint32_t now) {
    bool gap = false;
    const uint32_t sequence = rxCursor_.next(address, now, gap);
    if (gap) { ++diagnostics_.dmaDiscontinuities; discontinuity_ = true; }
    if (rxCount_ == kDuplexHistoryBlocks) {
      rxHead_ = (rxHead_ + 1) % kDuplexHistoryBlocks;
      --rxCount_;
      ++diagnostics_.rxOverflows;
      discontinuity_ = true;
    }
    SampleBlock& slot = rx_[(rxHead_ + rxCount_) % kDuplexHistoryBlocks];
    slot.sequence = sequence;
    slot.capturedAt = now;
    for (size_t i = 0; i < kDuplexSamples; ++i) slot.pcm[i] = static_cast<int16_t>(stereo[2 * i] >> 16);
    ++rxCount_;
  }

  bool VOICEBOT_AUDIO_IRAM receive(AlignedAudioBlock& out) {
    // At most sixteen stale blocks can be removed per call. A missing peer
    // never causes an unbounded wait, and later matching data recovers itself.
    while (rxCount_ && refCount_) {
      const SampleBlock& mic = rx_[rxHead_];
      const SampleBlock& reference = ref_[refHead_];
      const int32_t difference = static_cast<int32_t>(mic.sequence - reference.sequence);
      if (difference) {
        if (difference < 0) { rxHead_ = (rxHead_ + 1) % kDuplexHistoryBlocks; --rxCount_; }
        else { refHead_ = (refHead_ + 1) % kDuplexHistoryBlocks; --refCount_; }
        ++diagnostics_.alignmentDrops;
        discontinuity_ = true;
        continue;
      }
      out.sequence = mic.sequence;
      out.capturedAtMillis = mic.capturedAt;
      out.discontinuity = discontinuity_ || (delivered_ && mic.sequence != expectedSequence_);
      for (size_t i = 0; i < kDuplexSamples; ++i) {
        out.mic[i] = mic.pcm[i];
        out.reference[i] = reference.pcm[i];
      }
      rxHead_ = (rxHead_ + 1) % kDuplexHistoryBlocks;
      refHead_ = (refHead_ + 1) % kDuplexHistoryBlocks;
      --rxCount_; --refCount_;
      expectedSequence_ = out.sequence + 1;
      delivered_ = true;
      discontinuity_ = false;
      return true;
    }
    return false;
  }

  uint32_t pendingBlocks() const { return pending_; }
  uint32_t startedGeneration() const { return startedGeneration_; }
  uint32_t drainedAt() const { return drainedAt_; }
  bool drainedValid() const { return drainedValid_; }
  DuplexDiagnostics diagnostics() const { return diagnostics_; }
  // Only the protocol owner knows whether an idle DMA interval ends a burst.
  // Preserve actual-start evidence across temporary producer underruns.
  bool clearPlaybackProgress() {
    if (pending_) return false;
    startedGeneration_ = 0;
    drainedValid_ = false;
    return true;
  }

 private:
  struct Stage { int16_t pcm[kDuplexSamples]; uint32_t generation; };
  struct SampleBlock { int16_t pcm[kDuplexSamples]; uint32_t sequence, capturedAt; };
  struct DmaBlock { uintptr_t address; uint32_t generation; bool valid; };

  // Descriptor identities detect skipped/coalesced EOF callbacks. Millisecond
  // spacing disambiguates complete descriptor-ring laps. A missed callback is
  // always a discontinuity; estimated indices must not silently train the AEC.
  struct Cursor {
    uintptr_t addresses[kDuplexDmaBlocks] = {};
    uint32_t sequence = 0, lastAt = 0;
    size_t lastSlot = 0;
    bool active = false;
    void reset(uint32_t first, uint32_t now) {
      sequence = first; lastAt = now; lastSlot = 0; active = false;
      for (size_t i = 0; i < kDuplexDmaBlocks; ++i) addresses[i] = 0;
    }
    uint32_t VOICEBOT_AUDIO_IRAM next(uintptr_t address, uint32_t now, bool& gap) {
      const uint32_t elapsed = now - lastAt;
      if (!active) {
        // RX is armed before TX starts, so both first DMA blocks share the
        // same sample origin. Time before enabling TX is not audio time, and
        // FIFO/ISR phase can straddle a rounding boundary. Never derive the
        // first block's sequence from that wall time. Flag a delayed startup
        // for DSP recovery without permanently offsetting microphone/reference.
        if (elapsed >= 15U) gap = true;
        addresses[0] = address;
        active = true;
        lastAt = now;
        return sequence;
      }
      uint32_t advance = (elapsed + 5U) / 10U;
      if (!advance) advance = 1;
      size_t slot = kDuplexDmaBlocks;
      for (size_t i = 0; i < kDuplexDmaBlocks; ++i) if (addresses[i] == address) { slot = i; break; }
      if (slot == kDuplexDmaBlocks) {
        slot = (lastSlot + advance) % kDuplexDmaBlocks;
        if (addresses[slot]) {
          for (size_t i = 0; i < kDuplexDmaBlocks; ++i) if (!addresses[i]) { slot = i; break; }
          gap = true;
        }
        addresses[slot] = address;
      }
      uint32_t distance = (slot + kDuplexDmaBlocks - lastSlot) % kDuplexDmaBlocks;
      if (!distance) distance = kDuplexDmaBlocks;
      if (advance > distance) distance += ((advance - distance + 1U) / kDuplexDmaBlocks) * kDuplexDmaBlocks;
      if (distance != 1) gap = true;
      sequence += distance;
      lastSlot = slot;
      lastAt = now;
      return sequence;
    }
  } txCursor_, rxCursor_;

  SampleBlock& VOICEBOT_AUDIO_IRAM pushReference() {
    if (refCount_ == kDuplexHistoryBlocks) {
      refHead_ = (refHead_ + 1) % kDuplexHistoryBlocks;
      --refCount_;
      ++diagnostics_.referenceOverflows;
      discontinuity_ = true;
    }
    SampleBlock& slot = ref_[(refHead_ + refCount_) % kDuplexHistoryBlocks];
    ++refCount_;
    return slot;
  }
  void VOICEBOT_AUDIO_IRAM release(uint32_t now) {
    drainedAt_ = now;
    drainedValid_ = true;
    if (pending_) --pending_;
  }

  Stage stage_[kDuplexStagingBlocks];
  SampleBlock rx_[kDuplexHistoryBlocks], ref_[kDuplexHistoryBlocks];
  DmaBlock dma_[kDuplexDmaBlocks];
  size_t stageHead_ = 0, stageCount_ = 0, rxHead_ = 0, rxCount_ = 0, refHead_ = 0, refCount_ = 0;
  uint32_t generation_ = 1, pending_ = 0, startedGeneration_ = 0, drainedAt_ = 0, expectedSequence_ = 0;
  bool drainedValid_ = false, delivered_ = false, discontinuity_ = false;
  DuplexDiagnostics diagnostics_;
};

}  // namespace voicebot_audio
