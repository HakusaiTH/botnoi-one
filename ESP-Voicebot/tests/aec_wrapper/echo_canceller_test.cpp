#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <unordered_map>
#include "../../echo_canceller.h"

namespace {
constexpr size_t KiB = 1024;
struct Fake {
  bool psramInitialized = true;
  bool createFails = false;
  bool missingInnerHandle = false;
  size_t internalFree = 256 * KiB;
  size_t internalLargest = 192 * KiB;
  size_t psramFree = 2 * 1024 * KiB;
  size_t psramLargest = 1024 * KiB;
  size_t nativeInternalBytes = 32 * KiB;
  size_t nativePsramBytes = 100 * KiB;
  size_t nativeLargestLimit = 0;
  size_t savedLargest = 0;
  unsigned allocationCalls = 0;
  unsigned failAllocationCall = 0;
  unsigned createCalls = 0;
  unsigned destroyCalls = 0;
  unsigned incompleteOuterFrees = 0;
  void* incompleteOuter = nullptr;
  unsigned processCalls = 0;
  int frameSamples = 512;
  int64_t time = 100;
  int64_t processMicros = 37;
  std::unordered_map<void*, size_t> allocations;
} fake;

void resetFake() {
  assert(fake.allocations.empty());
  assert(!fake.incompleteOuter);
  assert(fake.createCalls == fake.destroyCalls + fake.incompleteOuterFrees || fake.createFails);
  fake = Fake{};
}
}  // namespace

bool esp_psram_is_initialized() { return fake.psramInitialized; }
int64_t esp_timer_get_time() { return fake.time; }
size_t heap_caps_get_free_size(uint32_t caps) {
  return caps & MALLOC_CAP_SPIRAM ? fake.psramFree : fake.internalFree;
}
size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return caps & MALLOC_CAP_SPIRAM ? std::min(fake.psramFree, fake.psramLargest) :
      std::min(fake.internalFree, fake.internalLargest);
}
void* heap_caps_aligned_calloc(size_t alignment, size_t count, size_t size, uint32_t caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  assert(alignment == 16);
  if (++fake.allocationCalls == fake.failAllocationCall) return nullptr;
  const size_t bytes = count * size;
  assert(bytes <= fake.internalFree);
  void* result = nullptr;
  assert(posix_memalign(&result, alignment, bytes) == 0);
  std::memset(result, 0, bytes);
  fake.allocations[result] = bytes;
  fake.internalFree -= bytes;
  return result;
}
void heap_caps_free(void* pointer) {
  if (!pointer) return;
  auto entry = fake.allocations.find(pointer);
  assert(entry != fake.allocations.end());
  fake.internalFree += entry->second;
  fake.allocations.erase(entry);
  if (pointer == fake.incompleteOuter) {
    ++fake.incompleteOuterFrees;
    fake.incompleteOuter = nullptr;
  }
  std::free(pointer);
}
aec_handle_t* aec_create_from_config(aec_config_t* config) {
  ++fake.createCalls;
  assert(config->mic_num == 1 && config->ref_num == 1 && config->out_num == 1);
  assert(config->filter_length == 4 && config->sample_rate == 16000);
  assert(config->caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  assert(config->mode == AEC_MODE_FD_LOW_COST && config->nlp_level == AEC_NLP_LEVEL_NORMAL);
  if (fake.createFails) return nullptr;
  if (fake.missingInnerHandle) {
    // Match the pinned constructor's malloc-owned outer handle. No inner
    // allocation survives this simulated failure, and native destroy is unsafe.
    void* storage = std::malloc(sizeof(aec_handle_t));
    assert(storage);
    auto* outer = new (storage) aec_handle_t{nullptr, fake.frameSamples, *config};
    fake.allocations[outer] = sizeof(aec_handle_t);
    fake.internalFree -= sizeof(aec_handle_t);
    fake.incompleteOuter = outer;
    return outer;
  }
  assert(fake.nativeInternalBytes <= fake.internalFree);
  assert(fake.nativePsramBytes <= fake.psramFree);
  fake.internalFree -= fake.nativeInternalBytes;
  fake.psramFree -= fake.nativePsramBytes;
  fake.savedLargest = fake.internalLargest;
  if (fake.nativeLargestLimit) fake.internalLargest = fake.nativeLargestLimit;
  return new aec_handle_t{&fake, fake.frameSamples, *config};
}
int aec_get_chunksize(const aec_handle_t* handle) { return handle->frame_size; }
void aec_process(const aec_handle_t*, int16_t* microphone, int16_t* reference, int16_t* output) {
  ++fake.processCalls;
  assert(reinterpret_cast<uintptr_t>(microphone) % 16 == 0);
  assert(reinterpret_cast<uintptr_t>(reference) % 16 == 0);
  assert(reinterpret_cast<uintptr_t>(output) % 16 == 0);
  for (int i = 0; i < fake.frameSamples; ++i) output[i] = microphone[i] - reference[i];
  fake.time += fake.processMicros;
}
void aec_destroy(aec_handle_t* handle) {
  assert(handle && handle->aec_handle); // Pinned native destroy dereferences this.
  ++fake.destroyCalls;
  fake.internalFree += fake.nativeInternalBytes;
  fake.psramFree += fake.nativePsramBytes;
  fake.internalLargest = fake.savedLargest;
  delete handle;
}

using voicebot_audio::EchoCanceller;
using voicebot_audio::EchoCancellerStatus;

void testAdmissionSkipsUnsafeConstructor() {
  for (unsigned failure = 0; failure < 5; ++failure) {
    resetFake();
    if (failure == 0) fake.psramInitialized = false;
    if (failure == 1) fake.psramFree = EchoCanceller::kMinPsramFreeBytes - 1;
    if (failure == 2) fake.psramLargest = EchoCanceller::kMinPsramLargestBlockBytes - 1;
    if (failure == 3) fake.internalFree = EchoCanceller::kMinInternalFreeBytes - 1;
    if (failure == 4) fake.internalLargest = EchoCanceller::kMinInternalLargestBlockBytes - 1;
    EchoCanceller aec;
    const auto expected = failure == 0 ? EchoCancellerStatus::MissingPsram :
        EchoCancellerStatus::InsufficientMemory;
    assert(aec.begin() == expected);
    assert(aec.status() == expected && aec.frameSamples() == 0);
    assert(fake.createCalls == 0 && fake.allocationCalls == 0);
  }

  // The initial heap can pass, but the three buffers take it below the native
  // preflight threshold. No native constructor call may occur in that case.
  resetFake();
  fake.internalFree = EchoCanceller::kMinInternalFreeBytes + 1024;
  const auto initialFree = fake.internalFree;
  EchoCanceller aec;
  assert(aec.begin() == EchoCancellerStatus::InsufficientMemory);
  assert(fake.createCalls == 0 && fake.allocations.empty());
  assert(fake.internalFree == initialFree);
}

void testBufferFailureCleansUp() {
  for (unsigned failAt = 1; failAt <= 3; ++failAt) {
    resetFake();
    fake.failAllocationCall = failAt;
    const auto initialFree = fake.internalFree;
    EchoCanceller aec;
    assert(aec.begin() == EchoCancellerStatus::InsufficientMemory);
    assert(fake.createCalls == 0 && fake.allocations.empty());
    assert(fake.internalFree == initialFree);
  }
}

void testNativeFailureAndUnexpectedFormat() {
  for (unsigned failure = 0; failure < 3; ++failure) {
    resetFake();
    fake.createFails = failure == 0;
    fake.missingInnerHandle = failure == 1;
    if (failure == 2) fake.frameSamples = 1024;
    const auto initialFree = fake.internalFree;
    EchoCanceller aec;
    assert(aec.begin() == EchoCancellerStatus::InitFailed);
    assert(aec.frameSamples() == 0 && fake.allocations.empty());
    assert(fake.internalFree == initialFree);
    assert(fake.destroyCalls == (failure == 2 ? 1U : 0U));
    assert(fake.incompleteOuterFrees == (failure == 1 ? 1U : 0U));
  }
}

void testPostCreationReserve() {
  for (unsigned failure = 0; failure < 2; ++failure) {
    resetFake();
    if (failure == 0) fake.nativeInternalBytes = 200 * KiB;
    if (failure == 1) fake.nativeLargestLimit = EchoCanceller::kInternalLargestReserveBytes - 1;
    const auto initialFree = fake.internalFree;
    EchoCanceller aec;
    assert(aec.begin() == EchoCancellerStatus::InsufficientMemory);
    assert(fake.createCalls == 1 && fake.destroyCalls == 1);
    assert(fake.allocations.empty() && fake.internalFree == initialFree);
    assert(aec.stats().internalBytesUsed == fake.nativeInternalBytes + 3 * 1024);
  }
}

void testFrameProcessingAndLifetime() {
  resetFake();
  const auto initialFree = fake.internalFree;
  {
    EchoCanceller aec;
    assert(aec.status() == EchoCancellerStatus::NotInitialized);
    assert(aec.begin() == EchoCancellerStatus::Ready && aec.frameSamples() == 512);
    assert(aec.begin() == EchoCancellerStatus::Ready && fake.createCalls == 1);
    assert(fake.allocationCalls == 3);
    assert(aec.stats().internalBytesUsed == fake.nativeInternalBytes + 3 * 1024);
    assert(aec.stats().psramBytesUsed == fake.nativePsramBytes);

    // All caller arrays deliberately have 2-byte, but not 16-byte, alignment.
    alignas(16) int16_t microphone[513];
    alignas(16) int16_t reference[513];
    alignas(16) int16_t output[513];
    for (unsigned i = 0; i < 513; ++i) {
      microphone[i] = static_cast<int16_t>(i);
      reference[i] = 100;
      output[i] = -123;
    }
    assert(!aec.process(nullptr, reference + 1, output + 1, 512));
    assert(!aec.process(microphone + 1, nullptr, output + 1, 512));
    assert(!aec.process(microphone + 1, reference + 1, nullptr, 512));
    assert(!aec.process(microphone + 1, reference + 1, output + 1, 511));
    assert(!aec.process(microphone + 1, reference + 1, output + 1, 513));
    assert(fake.processCalls == 0 && output[1] == -123);
    assert(aec.process(microphone + 1, reference + 1, output + 1, 512));
    for (unsigned i = 1; i < 513; ++i) assert(output[i] == microphone[i] - 100);
    assert(output[0] == -123);
    assert(aec.stats().maxProcessMicros == 37);

    // Output may alias microphone data; both input copies precede processing.
    fake.processMicros = 72;
    assert(aec.process(microphone + 1, reference + 1, microphone + 1, 512));
    for (unsigned i = 1; i < 513; ++i) assert(microphone[i] == static_cast<int16_t>(i) - 100);
    assert(aec.stats().maxProcessMicros == 72 && aec.stats().processedFrames == 2);
    assert(fake.allocationCalls == 3 && fake.createCalls == 1);

    aec.end();
    assert(aec.frameSamples() == 0 && aec.status() == EchoCancellerStatus::NotInitialized);
    assert(aec.stats().processedFrames == 2);
    assert(!aec.process(microphone + 1, reference + 1, output + 1, 512));
    assert(fake.allocations.empty() && fake.internalFree == initialFree);
    aec.end();
    assert(fake.destroyCalls == 1);
    assert(aec.begin() == EchoCancellerStatus::Ready);
    assert(aec.stats().processedFrames == 0 && aec.stats().maxProcessMicros == 0);
  }
  assert(fake.createCalls == 2 && fake.destroyCalls == 2);
  assert(fake.allocations.empty() && fake.internalFree == initialFree);
}

int main() {
  testAdmissionSkipsUnsafeConstructor();
  testBufferFailureCleansUp();
  testNativeFailureAndUnexpectedFormat();
  testPostCreationReserve();
  testFrameProcessingAndLifetime();
  std::cout << "AEC wrapper admission, lifetime, alignment and frame tests passed\n";
}
