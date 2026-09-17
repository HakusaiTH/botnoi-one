#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <esp_aec.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_timer.h>

namespace voicebot_audio {

enum class EchoCancellerStatus : uint8_t {
  NotInitialized,
  Ready,
  MissingPsram,
  InsufficientMemory,
  InitFailed,
};

struct EchoHeapSnapshot {
  size_t internalFreeBytes = 0;
  size_t internalLargestBlockBytes = 0;
  size_t psramFreeBytes = 0;
  size_t psramLargestBlockBytes = 0;
};

struct EchoCancellerStats {
  EchoHeapSnapshot before;
  EchoHeapSnapshot after;
  size_t internalBytesUsed = 0;
  size_t psramBytesUsed = 0;
  uint32_t maxProcessMicros = 0;
  uint32_t processedFrames = 0;
};

// Create before Wi-Fi and before starting tasks which allocate memory. The
// pinned ESP-SR FD constructor has unchecked internal allocations and requires
// PSRAM even when config.caps requests another heap; a NULL check alone cannot
// protect it. These admission limits leave ample room for its fixed workspace.
// All lifecycle operations and process() require one serial audio owner. Once
// initialized, stats() can also be read by the logging task.
class EchoCanceller {
 public:
  static constexpr size_t kSampleRate = 16000;
  static constexpr size_t kFrameSamples = 512;
  static constexpr size_t kMinPsramFreeBytes = 512 * 1024;
  static constexpr size_t kMinPsramLargestBlockBytes = 256 * 1024;
  static constexpr size_t kMinInternalFreeBytes = 128 * 1024;
  static constexpr size_t kMinInternalLargestBlockBytes = 64 * 1024;
  static constexpr size_t kInternalReserveBytes = 64 * 1024;
  static constexpr size_t kInternalLargestReserveBytes = 32 * 1024;

  EchoCanceller() = default;
  ~EchoCanceller() { end(); }
  EchoCanceller(const EchoCanceller&) = delete;
  EchoCanceller& operator=(const EchoCanceller&) = delete;
  EchoCanceller(EchoCanceller&&) = delete;
  EchoCanceller& operator=(EchoCanceller&&) = delete;

  EchoCancellerStatus begin() {
    // Starting another conversation must retain the learned acoustic path.
    if (status_ == EchoCancellerStatus::Ready) return status_;
    releaseResources();
    stats_ = EchoCancellerStats{};
    maxProcessMicros_.store(0, std::memory_order_relaxed);
    processedFrames_.store(0, std::memory_order_relaxed);
    stats_.before = heapSnapshot();
    stats_.after = stats_.before;

    if (!esp_psram_is_initialized()) return fail(EchoCancellerStatus::MissingPsram);
    if (!hasInitializationRoom(stats_.before)) {
      return fail(EchoCancellerStatus::InsufficientMemory);
    }

    // Caller buffers need not be aligned. Keep all three native buffers fixed
    // for the instance lifetime, including during reference discontinuities.
    microphone_ = allocateFrame();
    reference_ = allocateFrame();
    output_ = allocateFrame();
    if (!microphone_ || !reference_ || !output_) {
      recordAllocationStats();
      return fail(EchoCancellerStatus::InsufficientMemory);
    }
    // Recheck immediately before the unsafe native constructor, accounting for
    // our buffers. There must be no concurrent allocation tasks during begin().
    if (!hasInitializationRoom(heapSnapshot())) {
      recordAllocationStats();
      return fail(EchoCancellerStatus::InsufficientMemory);
    }

    aec_config_t config{};
    config.mic_num = config.ref_num = config.out_num = 1;
    config.filter_length = 4;
    config.sample_rate = static_cast<int>(kSampleRate);
    config.caps = kPsramCaps;
    config.mode = AEC_MODE_FD_LOW_COST;
    config.nlp_level = AEC_NLP_LEVEL_NORMAL;
    handle_ = aec_create_from_config(&config);
    recordAllocationStats();
    if (!handle_ || !handle_->aec_handle) return fail(EchoCancellerStatus::InitFailed);

    const int frameSamples = aec_get_chunksize(handle_);
    // An unexpected library format must never overrun our bounded buffers.
    if (frameSamples != static_cast<int>(kFrameSamples)) {
      return fail(EchoCancellerStatus::InitFailed);
    }
    if (stats_.after.internalFreeBytes < kInternalReserveBytes ||
        stats_.after.internalLargestBlockBytes < kInternalLargestReserveBytes) {
      return fail(EchoCancellerStatus::InsufficientMemory);
    }
    frameSamples_ = static_cast<size_t>(frameSamples);
    status_ = EchoCancellerStatus::Ready;
    return status_;
  }

  void end() {
    releaseResources();
    status_ = EchoCancellerStatus::NotInitialized;
    // Retain the last measurements for diagnostics after shutdown.
  }

  size_t frameSamples() const { return frameSamples_; }
  EchoCancellerStatus status() const { return status_; }
  static const char* modeName() { return "ESP-SR FD_LOW_COST / NORMAL"; }

  const char* statusMessage() const { return statusMessage(status_); }
  static const char* statusMessage(EchoCancellerStatus status) {
    switch (status) {
      case EchoCancellerStatus::NotInitialized: return "AEC not initialized";
      case EchoCancellerStatus::Ready: return "AEC ready";
      case EchoCancellerStatus::MissingPsram: return "AEC unavailable: PSRAM not initialized";
      case EchoCancellerStatus::InsufficientMemory: return "AEC unavailable: insufficient memory reserve";
      case EchoCancellerStatus::InitFailed: return "AEC unavailable: native initialization failed";
    }
    return "AEC unavailable";
  }

  EchoCancellerStats stats() const {
    EchoCancellerStats result = stats_;
    result.maxProcessMicros = maxProcessMicros_.load(std::memory_order_relaxed);
    result.processedFrames = processedFrames_.load(std::memory_order_relaxed);
    return result;
  }

  bool process(const int16_t* microphone, const int16_t* reference,
               int16_t* output, size_t count) {
    if (status_ != EchoCancellerStatus::Ready || !microphone || !reference ||
        !output || count != frameSamples_) return false;
    const int64_t startedAt = esp_timer_get_time();
    const size_t bytes = count * sizeof(int16_t);
    memcpy(microphone_, microphone, bytes);
    memcpy(reference_, reference, bytes);
    aec_process(handle_, microphone_, reference_, output_);
    memcpy(output, output_, bytes);
    const int64_t elapsed = esp_timer_get_time() - startedAt;
    const uint32_t duration = elapsed <= 0 ? 0 :
        (static_cast<uint64_t>(elapsed) > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(elapsed));
    if (duration > maxProcessMicros_.load(std::memory_order_relaxed)) {
      maxProcessMicros_.store(duration, std::memory_order_relaxed);
    }
    processedFrames_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

 private:
  static constexpr uint32_t kInternalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  static constexpr uint32_t kPsramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

  static EchoHeapSnapshot heapSnapshot() {
    EchoHeapSnapshot result;
    result.internalFreeBytes = heap_caps_get_free_size(kInternalCaps);
    result.internalLargestBlockBytes = heap_caps_get_largest_free_block(kInternalCaps);
    result.psramFreeBytes = heap_caps_get_free_size(kPsramCaps);
    result.psramLargestBlockBytes = heap_caps_get_largest_free_block(kPsramCaps);
    return result;
  }

  static bool hasInitializationRoom(const EchoHeapSnapshot& heap) {
    return heap.psramFreeBytes >= kMinPsramFreeBytes &&
        heap.psramLargestBlockBytes >= kMinPsramLargestBlockBytes &&
        heap.internalFreeBytes >= kMinInternalFreeBytes &&
        heap.internalLargestBlockBytes >= kMinInternalLargestBlockBytes;
  }

  static int16_t* allocateFrame() {
    return static_cast<int16_t*>(heap_caps_aligned_calloc(
        16, kFrameSamples, sizeof(int16_t), kInternalCaps));
  }

  void recordAllocationStats() {
    stats_.after = heapSnapshot();
    stats_.internalBytesUsed = stats_.before.internalFreeBytes >= stats_.after.internalFreeBytes ?
        stats_.before.internalFreeBytes - stats_.after.internalFreeBytes : 0;
    stats_.psramBytesUsed = stats_.before.psramFreeBytes >= stats_.after.psramFreeBytes ?
        stats_.before.psramFreeBytes - stats_.after.psramFreeBytes : 0;
  }

  EchoCancellerStatus fail(EchoCancellerStatus status) {
    releaseResources();
    status_ = status;
    return status_;
  }

  void releaseResources() {
    if (handle_) {
      if (handle_->aec_handle) aec_destroy(handle_);
      // The pinned constructor mallocs this outer handle. Native destroy
      // dereferences the inner state without checking it, even after a failed
      // initialization; only release the outer allocation when it is absent.
      else heap_caps_free(handle_);
    }
    handle_ = nullptr;
    heap_caps_free(microphone_);
    heap_caps_free(reference_);
    heap_caps_free(output_);
    microphone_ = reference_ = output_ = nullptr;
    frameSamples_ = 0;
  }

  aec_handle_t* handle_ = nullptr;
  int16_t* microphone_ = nullptr;
  int16_t* reference_ = nullptr;
  int16_t* output_ = nullptr;
  size_t frameSamples_ = 0;
  EchoCancellerStatus status_ = EchoCancellerStatus::NotInitialized;
  EchoCancellerStats stats_;
  std::atomic<uint32_t> maxProcessMicros_{0};
  std::atomic<uint32_t> processedFrames_{0};
};

}  // namespace voicebot_audio
