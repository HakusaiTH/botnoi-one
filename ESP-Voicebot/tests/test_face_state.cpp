#include "../face_state.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using voicebot_face::FaceAnimator;
using voicebot_face::FaceFrame;
using voicebot_face::Mood;

static void the_integer_sine_covers_a_full_cycle() {
  assert(voicebot_face::sine(0) == 0);
  assert(voicebot_face::sine(32) == 127);
  assert(voicebot_face::sine(64) == 0);
  assert(voicebot_face::sine(96) == -127);
  for (int phase = 0; phase < 256; ++phase) {
    const int16_t value = voicebot_face::sine(static_cast<uint8_t>(phase));
    assert(value >= -127 && value <= 127);
    // The 128-step phase wraps, so the upper bit must not change the result.
    assert(value == voicebot_face::sine(static_cast<uint8_t>(phase ^ 0x80)));
  }
  assert(voicebot_face::phaseOf(0, 1000) == 0);
  assert(voicebot_face::phaseOf(500, 1000) == 64);
  assert(voicebot_face::phaseOf(1000, 1000) == 0);
  assert(voicebot_face::phaseOf(10, 0) == 0);
  assert(voicebot_face::phaseOf(43200000, 86400000) == 64);
  assert(voicebot_face::phaseOf(UINT32_MAX - 1, UINT32_MAX) == 127);
}

static void boot_opens_the_lids_once_and_settles_into_a_smile() {
  FaceAnimator animator;
  animator.reset(5000);
  const FaceFrame start = animator.frame(5000, Mood::Boot, 0, 0);
  assert(start.leftEyeOpen == 0 && start.rightEyeOpen == 0);
  const FaceFrame middle = animator.frame(5350, Mood::Boot, 0, 0);
  assert(middle.leftEyeOpen > 100 && middle.leftEyeOpen < 200);
  const FaceFrame done = animator.frame(6200, Mood::Boot, 0, 0);
  assert(done.leftEyeOpen == 255 && done.rightEyeOpen == 255);
  assert(done.mouthOpen == 0 && done.mouthCurve > 0);
}

static void speaking_drives_the_mouth_from_the_speaker_envelope() {
  FaceAnimator animator;
  animator.reset(0);
  const FaceFrame quiet = animator.frame(100, Mood::Speaking, 0, 0);
  const FaceFrame mid = animator.frame(140, Mood::Speaking, 128, 0);
  const FaceFrame loud = animator.frame(180, Mood::Speaking, 255, 0);
  // A closed mouth during a reply still shows lips, never a blank face.
  assert(quiet.mouthOpen > 0 && quiet.mouthOpen < 64);
  assert(mid.mouthOpen > quiet.mouthOpen);
  assert(loud.mouthOpen == 255);
  assert(loud.mouthCurve > 0);
  // The microphone envelope must not reach the mouth while the bot talks.
  const FaceFrame noisy = animator.frame(220, Mood::Speaking, 0, 255);
  assert(noisy.mouthOpen == quiet.mouthOpen);
}

static void listening_reacts_to_the_user_voice_without_opening_the_mouth() {
  FaceAnimator animator;
  animator.reset(0);
  const FaceFrame quiet = animator.frame(100, Mood::Listening, 0, 0);
  const FaceFrame loud = animator.frame(140, Mood::Listening, 0, 255);
  assert(quiet.mouthOpen == 0 && loud.mouthOpen == 0);
  assert(loud.gazeY < quiet.gazeY);      // Attention lifts the gaze.
  assert(loud.mouthCurve > quiet.mouthCurve);
  assert(loud.leftEyeOpen == 255 && loud.rightEyeOpen == 255);
}

static void a_blink_closes_both_eyes_and_reopens_them() {
  FaceAnimator::Config config = FaceAnimator::defaults();
  config.blinkGapSpreadMs = 0;  // Periodic blinks make the schedule exact.
  config.winkEveryNthBlink = 0;
  FaceAnimator animator(config);
  animator.reset(0);
  // Entering a mood restarts the schedule, so measure from the first frame.
  animator.frame(0, Mood::Idle, 0, 0);
  bool closed = false;
  uint8_t minimum = 255;
  for (uint32_t now = 0; now <= 4000; now += 5) {
    const FaceFrame frame = animator.frame(now, Mood::Idle, 0, 0);
    assert(frame.leftEyeOpen == frame.rightEyeOpen);  // No wink in this config.
    if (frame.leftEyeOpen < minimum) minimum = frame.leftEyeOpen;
    if (frame.leftEyeOpen == 0) closed = true;
  }
  assert(closed && minimum == 0);
  const FaceFrame after = animator.frame(4100, Mood::Idle, 0, 0);
  assert(after.leftEyeOpen > 200 && after.rightEyeOpen > 200);
}

static void a_wink_closes_only_the_right_eye() {
  FaceAnimator::Config config = FaceAnimator::defaults();
  config.blinkGapSpreadMs = 0;
  config.winkEveryNthBlink = 1;  // Every blink is a wink.
  FaceAnimator animator(config);
  animator.reset(0);
  animator.frame(0, Mood::Idle, 0, 0);
  bool sawWink = false;
  for (uint32_t now = 0; now <= 3400; now += 5) {
    const FaceFrame frame = animator.frame(now, Mood::Idle, 0, 0);
    if (frame.rightEyeOpen == 0) {
      assert(frame.leftEyeOpen > 200);
      sawWink = true;
    }
  }
  assert(sawWink);
  // Winking is an idle flourish; a call in progress must not look distracted.
  FaceAnimator listening(config);
  listening.reset(0);
  listening.frame(0, Mood::Listening, 0, 0);
  for (uint32_t now = 0; now <= 3400; now += 5) {
    const FaceFrame frame = listening.frame(now, Mood::Listening, 0, 0);
    assert(frame.leftEyeOpen == frame.rightEyeOpen);
  }
}

static void connecting_and_error_never_blink_and_stay_bounded() {
  FaceAnimator::Config config = FaceAnimator::defaults();
  config.blinkGapSpreadMs = 0;
  FaceAnimator animator(config);
  animator.reset(0);
  for (uint32_t now = 0; now <= 12000; now += 7) {
    const FaceFrame frame = animator.frame(now, Mood::Connecting, 0, 0);
    assert(frame.leftEyeOpen == 215 && frame.rightEyeOpen == 215);
    assert(frame.gazeX >= -12 && frame.gazeX <= 12);
  }
  const FaceFrame error = animator.frame(13000, Mood::Error, 200, 200);
  assert(error.mouthCurve < 0 && error.leftEyeOpen == error.rightEyeOpen);
  assert(error.leftEyeOpen > 0);  // A frown, not a face that looks powered off.
}

static void every_mood_keeps_the_gaze_inside_the_renderer_limits() {
  const Mood moods[] = {Mood::Boot,      Mood::Connecting, Mood::Idle,  Mood::Listening,
                        Mood::Thinking,  Mood::Speaking,   Mood::Error};
  for (size_t i = 0; i < sizeof(moods) / sizeof(moods[0]); ++i) {
    FaceAnimator animator;
    animator.reset(0);
    for (uint32_t now = 0; now <= 20000; now += 11) {
      for (int level = 0; level <= 255; level += 255) {
        const FaceFrame frame =
            animator.frame(now, moods[i], static_cast<uint8_t>(level), static_cast<uint8_t>(level));
        // Layout::gazeLimitX / gazeLimitY size the eye windows.
        assert(frame.gazeX >= -14 && frame.gazeX <= 14);
        assert(frame.gazeY >= -8 && frame.gazeY <= 8);
      }
    }
  }
}

static void the_blink_schedule_is_reproducible_but_seedable() {
  FaceAnimator first;
  FaceAnimator second;
  first.reset(0);
  second.reset(0);
  for (uint32_t now = 0; now <= 30000; now += 13) {
    const FaceFrame a = first.frame(now, Mood::Idle, 0, 0);
    const FaceFrame b = second.frame(now, Mood::Idle, 0, 0);
    assert(a.leftEyeOpen == b.leftEyeOpen && a.rightEyeOpen == b.rightEyeOpen);
  }
  FaceAnimator seeded;
  seeded.seed(0xDEADBEEF);
  seeded.reset(0);
  FaceAnimator unseeded;
  unseeded.reset(0);
  bool differed = false;
  for (uint32_t now = 0; now <= 30000; now += 13) {
    const FaceFrame a = seeded.frame(now, Mood::Idle, 0, 0);
    const FaceFrame b = unseeded.frame(now, Mood::Idle, 0, 0);
    if (a.leftEyeOpen != b.leftEyeOpen) differed = true;
  }
  assert(differed);
  // Seeding with zero would zero the xorshift state and stop every blink.
  FaceAnimator zeroSeed;
  zeroSeed.seed(0);
  zeroSeed.reset(0);
  bool blinked = false;
  for (uint32_t now = 0; now <= 30000; now += 5) {
    if (zeroSeed.frame(now, Mood::Idle, 0, 0).leftEyeOpen == 0) blinked = true;
  }
  assert(blinked);
}

static void the_animation_survives_a_millis_rollover() {
  FaceAnimator::Config config = FaceAnimator::defaults();
  config.blinkGapSpreadMs = 0;
  FaceAnimator animator(config);
  const uint32_t before = 0xFFFFF000u;
  animator.reset(before);
  animator.frame(before, Mood::Idle, 0, 0);
  bool blinked = false;
  for (uint32_t step = 0; step <= 12000; step += 5) {
    const uint32_t now = before + step;  // Wraps through zero part way through.
    const FaceFrame frame = animator.frame(now, Mood::Idle, 0, 0);
    if (frame.leftEyeOpen == 0) blinked = true;
    assert(frame.gazeX >= -14 && frame.gazeX <= 14);
  }
  assert(blinked);
}

int main() {
  the_integer_sine_covers_a_full_cycle();
  boot_opens_the_lids_once_and_settles_into_a_smile();
  speaking_drives_the_mouth_from_the_speaker_envelope();
  listening_reacts_to_the_user_voice_without_opening_the_mouth();
  a_blink_closes_both_eyes_and_reopens_them();
  a_wink_closes_only_the_right_eye();
  connecting_and_error_never_blink_and_stay_bounded();
  every_mood_keeps_the_gaze_inside_the_renderer_limits();
  the_blink_schedule_is_reproducible_but_seedable();
  the_animation_survives_a_millis_rollover();
  printf("test_face_state: all cases passed\n");
  return 0;
}
