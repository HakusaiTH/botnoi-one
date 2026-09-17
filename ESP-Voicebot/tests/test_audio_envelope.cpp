#include "../audio_envelope.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using voicebot_face::Envelope;

static std::vector<int16_t> tone(size_t samples, int16_t amplitude) {
  std::vector<int16_t> pcm(samples);
  for (size_t i = 0; i < samples; ++i) pcm[i] = (i & 1) ? amplitude : static_cast<int16_t>(-amplitude);
  return pcm;
}

static Envelope::Config fastConfig() {
  Envelope::Config config = {100, 2000, 20, 100, 60};
  return config;
}

static void silence_and_the_noise_floor_keep_the_mouth_shut() {
  Envelope envelope(fastConfig());
  const std::vector<int16_t> quiet = tone(160, 90);  // Below the 100 floor.
  uint32_t now = 1000;
  for (int i = 0; i < 40; ++i) {
    assert(envelope.push(now, quiet.data(), quiet.size()) == 0);
    now += 10;
  }
  const std::vector<int16_t> zeros(160, 0);
  assert(envelope.push(now, zeros.data(), zeros.size()) == 0);
  assert(envelope.push(now + 10, nullptr, 0) == 0);
  assert(envelope.push(now + 20, zeros.data(), 0) == 0);
}

static void attack_reaches_the_target_in_one_attack_window() {
  Envelope envelope(fastConfig());
  const std::vector<int16_t> loud = tone(160, 4100);  // Saturates the 2000 span.
  assert(envelope.push(1000, loud.data(), loud.size()) == 0);  // First call only seeds the clock.
  const uint8_t halfway = envelope.push(1010, loud.data(), loud.size());
  assert(halfway > 100 && halfway < 200);  // Half of a 20 ms attack.
  // Repeated shorter steps approach the target monotonically and get there.
  uint8_t last = halfway;
  uint32_t now = 1015;
  for (int i = 0; i < 20; ++i) {
    const uint8_t level = envelope.push(now, loud.data(), loud.size());
    assert(level >= last);
    last = level;
    now += 5;
  }
  assert(last == 255);
  // One step of a whole attack window closes the entire remaining distance.
  Envelope coarse(fastConfig());
  assert(coarse.push(1000, loud.data(), loud.size()) == 0);
  assert(coarse.push(1020, loud.data(), loud.size()) == 255);
}

static void release_runs_out_to_silence_after_the_hold_expires() {
  Envelope envelope(fastConfig());
  const std::vector<int16_t> loud = tone(160, 4100);
  envelope.push(1000, loud.data(), loud.size());
  envelope.push(1030, loud.data(), loud.size());
  assert(envelope.level() == 255);
  // Inside the hold window the level must not start falling yet.
  assert(envelope.decay(1080) == 255);
  uint8_t last = 255;
  uint32_t now = 1100;
  for (int i = 0; i < 200; ++i) {
    const uint8_t level = envelope.decay(now);
    assert(level <= last);
    last = level;
    now += 5;
  }
  assert(last == 0);
  // Rounding up must guarantee progress even when polled faster than 1 ms/step.
  Envelope slow(fastConfig());
  slow.push(0, loud.data(), loud.size());
  slow.push(30, loud.data(), loud.size());
  for (uint32_t tick = 100; tick < 400; ++tick) slow.decay(tick);
  assert(slow.level() == 0);
}

static void the_level_survives_a_millis_rollover() {
  Envelope envelope(fastConfig());
  const std::vector<int16_t> loud = tone(160, 4100);
  const uint32_t before = 0xFFFFFFF0u;
  envelope.push(before, loud.data(), loud.size());
  assert(envelope.push(before + 30, loud.data(), loud.size()) == 255);
  // Wrapping past zero is an ordinary 20 ms step, not a 49-day release.
  assert(envelope.decay(20) == 255);
  assert(envelope.decay(120) < 255);
}

static void the_most_negative_sample_has_no_magnitude_overflow() {
  Envelope envelope(fastConfig());
  const std::vector<int16_t> extreme(160, INT16_MIN);
  envelope.push(1000, extreme.data(), extreme.size());
  assert(envelope.push(1020, extreme.data(), extreme.size()) == 255);
}

static void speech_level_amplitude_lands_mid_scale() {
  // The shipped speaker configuration must not clip ordinary TTS to a
  // permanently wide-open mouth, nor leave it nearly shut.
  Envelope envelope(Envelope::speakerDefaults());
  const std::vector<int16_t> speech = tone(1120, 2600);
  envelope.push(0, speech.data(), speech.size());
  const uint8_t level = envelope.push(40, speech.data(), speech.size());
  assert(level > 60 && level < 220);
}

int main() {
  silence_and_the_noise_floor_keep_the_mouth_shut();
  attack_reaches_the_target_in_one_attack_window();
  release_runs_out_to_silence_after_the_hold_expires();
  the_level_survives_a_millis_rollover();
  the_most_negative_sample_has_no_magnitude_overflow();
  speech_level_amplitude_lands_mid_scale();
  printf("test_audio_envelope: all cases passed\n");
  return 0;
}
