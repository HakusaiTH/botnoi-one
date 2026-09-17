#pragma once
// Offline target test: uses the production wrapper and real ESP-SR binary.
// No I2S, network, credentials, or board peripherals are used.
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "echo_canceller.h"

using voicebot_audio::EchoCanceller;
using voicebot_audio::EchoCancellerStatus;
constexpr size_t kRate = 16000;
constexpr size_t kCount = 8 * kRate;
constexpr uint32_t kInternal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kExternal = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

// A bounded deterministic broadband signal with a speech-like amplitude
// envelope. Separate PRNG streams make the near/far signals independent.
struct Signal {
  uint32_t state;
  float low = 0;
  float dc = 0;
  explicit Signal(uint32_t seed) : state(seed) {}
  float next(size_t index, float amplitude) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float white = static_cast<int32_t>(state >> 16) - 32768;
    low += 0.55f * (white - low);
    dc += 0.025f * (low - dc);
    const float envelope = ((index / 1600) % 7 == 0) ? 0.2f : 1.0f;
    return (low - dc) * amplitude * envelope / 32768.0f;
  }
};

int16_t clip(float sample) {
  return static_cast<int16_t>(sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample);
}

struct Signals {
  int16_t* far = nullptr;
  int16_t* near = nullptr;
  int16_t* mic = nullptr;
  int16_t* output = nullptr;
  bool allocate() {
    far = static_cast<int16_t*>(heap_caps_calloc(kCount, 2, kExternal));
    near = static_cast<int16_t*>(heap_caps_calloc(kCount, 2, kExternal));
    mic = static_cast<int16_t*>(heap_caps_calloc(kCount, 2, kExternal));
    output = static_cast<int16_t*>(heap_caps_calloc(kCount, 2, kExternal));
    return far && near && mic && output;
  }
  ~Signals() {
    heap_caps_free(far); heap_caps_free(near);
    heap_caps_free(mic); heap_caps_free(output);
  }
};

enum class Scenario { Silence, FarOnly, NearOnly, DoubleTalk, PathChange, Clipping };

void generate(Signals& s, Scenario scenario) {
  Signal far(0x7a29e843), near(0x8c56b139);
  for (size_t i = 0; i < kCount; ++i) {
    s.far[i] = clip(far.next(i, scenario == Scenario::Clipping ? 58000 : 16000));
    s.near[i] = clip(near.next(i, 16000));
    if (scenario == Scenario::Silence || scenario == Scenario::NearOnly) s.far[i] = 0;
    if (scenario != Scenario::NearOnly && scenario != Scenario::DoubleTalk) s.near[i] = 0;
    if (scenario == Scenario::DoubleTalk && i < 3 * kRate) s.near[i] = 0;
  }
  for (size_t i = 0; i < kCount; ++i) {
    const bool changed = scenario == Scenario::PathChange && i >= 4 * kRate;
    const size_t delay = changed ? 640 : 96;
    float echo = i >= delay ? s.far[i - delay] * (changed ? 0.75f : 0.65f) : 0;
    if (i >= delay + 127) echo += s.far[i - delay - 127] * 0.22f;
    if (i >= delay + 431) echo -= s.far[i - delay - 431] * 0.12f;
    if (scenario == Scenario::Clipping) echo *= 4;
    s.mic[i] = clip(s.near[i] + echo);
    s.output[i] = 0;
  }
}

struct Metrics {
  int lag = 0;
  double correlation = 0;
  double gain = 0;
  double snrDb = 0;
  double improvementDb = 0;
  double erleDb = 0;
  int outputPeak = 0;
};

double scoreLag(const Signals& s, size_t begin, int lag, size_t stride) {
  double xx = 0, yy = 0, xy = 0;
  for (size_t i = begin; i < kCount; i += stride) {
    const double x = s.near[i - lag], y = s.output[i];
    xx += x * x; yy += y * y; xy += x * y;
  }
  return xx > 0 && yy > 0 ? xy / sqrt(xx * yy) : 0;
}

Metrics measure(const Signals& s, Scenario scenario) {
  Metrics m;
  // Score only steady state; doubletalk follows three seconds of adaptation,
  // and the path-change case has two seconds to adapt to the new echo delay.
  const size_t begin = 6 * kRate;
  if (scenario == Scenario::NearOnly || scenario == Scenario::DoubleTalk) {
    double best = -1;
    for (int lag = 0; lag <= 2048; lag += 8) {
      const double score = scoreLag(s, begin, lag, 8);
      if (score > best) { best = score; m.lag = lag; }
    }
    const int coarse = m.lag;
    for (int lag = coarse > 7 ? coarse - 7 : 0; lag <= coarse + 7; ++lag) {
      const double score = scoreLag(s, begin, lag, 4);
      if (score > best) { best = score; m.lag = lag; }
    }
  }
  double xx = 0, yy = 0, xy = 0, error = 0, baselineError = 0, micEnergy = 0;
  for (size_t i = begin; i < kCount; ++i) {
    const double x = s.near[i - m.lag], y = s.output[i];
    const double input = s.mic[i - m.lag];
    xx += x * x; yy += y * y; xy += x * y;
    error += (y - x) * (y - x);
    baselineError += (input - x) * (input - x);
    micEnergy += input * input;
  }
  for (size_t i = 0; i < kCount; ++i) {
    int peak = s.output[i] < 0 ? -static_cast<int>(s.output[i]) : s.output[i];
    if (peak > m.outputPeak) m.outputPeak = peak;
  }
  m.correlation = xx > 0 && yy > 0 ? xy / sqrt(xx * yy) : 0;
  m.gain = xx > 0 ? xy / xx : 0;
  m.snrDb = 10 * log10((xx + 1) / (error + 1));
  m.improvementDb = 10 * log10((baselineError + 1) / (error + 1));
  m.erleDb = 10 * log10((micEnergy + 1) / (yy + 1));
  return m;
}

bool runScenario(const char* name, Scenario scenario, Signals& signals) {
  printf("AEC_PROGRESS %s generating\n", name);
  generate(signals, scenario);
  const size_t beforeInternal = heap_caps_get_free_size(kInternal);
  const size_t beforeExternal = heap_caps_get_free_size(kExternal);
  EchoCanceller aec;
  printf("AEC_PROGRESS %s initializing\n", name);
  if (aec.begin() != EchoCancellerStatus::Ready) {
    printf("AEC_FAILURE %s: %s\n", name, aec.statusMessage());
    return false;
  }
  const size_t activeInternal = heap_caps_get_free_size(kInternal);
  const size_t activeExternal = heap_caps_get_free_size(kExternal);
  printf("AEC_MEMORY {\"case\":\"%s\",\"internal_bytes\":%u,\"psram_bytes\":%u}\n",
      name, unsigned(aec.stats().internalBytesUsed), unsigned(aec.stats().psramBytesUsed));
  if (aec.frameSamples() != 512 || aec.begin() != EchoCancellerStatus::Ready ||
      activeInternal != heap_caps_get_free_size(kInternal) ||
      activeExternal != heap_caps_get_free_size(kExternal)) {
    printf("AEC_FAILURE %s: frame format/repeated begin allocation\n", name);
    return false;
  }
  bool processed = true;
  for (size_t i = 0; i < kCount; i += aec.frameSamples()) {
    if ((i / 512) % 32 == 0) printf("AEC_PROGRESS %s frame=%u/250\n", name, unsigned(i / 512));
    processed &= aec.process(signals.mic + i, signals.far + i, signals.output + i, aec.frameSamples());
    if ((i / 512) % 32 == 0) delay(1);  // Allow the idle task to feed the watchdog.
  }
  const auto stats = aec.stats();
  const bool stableHeap = activeInternal == heap_caps_get_free_size(kInternal) &&
      activeExternal == heap_caps_get_free_size(kExternal);
  aec.end();
  const bool released = beforeInternal == heap_caps_get_free_size(kInternal) &&
      beforeExternal == heap_caps_get_free_size(kExternal);
  const Metrics m = measure(signals, scenario);
  bool quality = true;
  if (scenario == Scenario::Silence) quality = m.outputPeak <= 2;
  if (scenario == Scenario::FarOnly) quality = m.erleDb >= 8;
  if (scenario == Scenario::PathChange) quality = m.erleDb >= 6;
  if (scenario == Scenario::NearOnly) {
    quality = m.correlation >= 0.7 && m.gain >= 0.5 && m.gain <= 1.5 && m.snrDb >= 6;
  }
  if (scenario == Scenario::DoubleTalk) {
    quality = m.correlation >= 0.55 && m.gain >= 0.35 && m.gain <= 1.5 && m.improvementDb >= 3;
  }
  // PCM16 bounds are enforced by the actual library interface; clipping stress
  // additionally must produce a nonconstant result and complete without panic.
  if (scenario == Scenario::Clipping) quality = m.outputPeak > 0 && m.outputPeak <= 32768;
  const bool pass = processed && stableHeap && released && quality &&
      isfinite(m.erleDb) && isfinite(m.correlation) && isfinite(m.snrDb);
  printf("AEC_RESULT {\"case\":\"%s\",\"pass\":%s,\"processed\":%s,"
      "\"heap_stable\":%s,\"heap_released\":%s,\"erle_db\":%.3f,\"near_correlation\":%.5f,"
      "\"near_gain\":%.5f,\"near_snr_db\":%.3f,\"error_improvement_db\":%.3f,"
      "\"lag_samples\":%d,\"peak\":%d,\"internal_bytes\":%u,\"psram_bytes\":%u,"
      "\"frames\":%u,\"emulated_max_process_us\":%u}\n",
      name, pass ? "true" : "false", processed ? "true" : "false",
      stableHeap ? "true" : "false", released ? "true" : "false", m.erleDb,
      m.correlation, m.gain, m.snrDb, m.improvementDb, m.lag, m.outputPeak,
      unsigned(stats.internalBytesUsed), unsigned(stats.psramBytesUsed),
      unsigned(stats.processedFrames), unsigned(stats.maxProcessMicros));
  return pass;
}

void setup() {
  delay(100);
  printf("AEC_TARGET {\"rate\":16000,\"frame\":512,\"psram\":%s,\"mode\":\"%s\"}\n",
      esp_psram_is_initialized() ? "true" : "false", EchoCanceller::modeName());
#ifdef AEC_TEST_NO_PSRAM
  EchoCanceller aec;
  const size_t before = heap_caps_get_free_size(kInternal);
  const bool pass = aec.begin() == EchoCancellerStatus::MissingPsram &&
      heap_caps_get_free_size(kInternal) == before;
  printf("AEC_RESULT {\"case\":\"missing_psram\",\"pass\":%s}\n", pass ? "true" : "false");
  printf("AEC_DONE %s\n", pass ? "PASS" : "FAIL");
#else
  Signals signals;
  if (!signals.allocate()) {
    puts("AEC_FAILURE fixture allocation");
    puts("AEC_DONE FAIL");
    return;
  }
  bool pass = true;
  pass &= runScenario("silence", Scenario::Silence, signals);
  pass &= runScenario("far_only", Scenario::FarOnly, signals);
  pass &= runScenario("near_only", Scenario::NearOnly, signals);
  pass &= runScenario("doubletalk", Scenario::DoubleTalk, signals);
  pass &= runScenario("path_change", Scenario::PathChange, signals);
  pass &= runScenario("clipping", Scenario::Clipping, signals);
  printf("AEC_DONE %s\n", pass ? "PASS" : "FAIL");
#endif
}

void loop() { delay(1000); }
