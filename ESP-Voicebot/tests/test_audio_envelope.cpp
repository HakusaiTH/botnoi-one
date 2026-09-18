#include "../audio_envelope.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using voicebot_face::Envelope;

static Envelope::Config config() { return {100, 2000, 20, 100, 140}; }
static bool near(uint8_t a, uint8_t b) { return int(a)-int(b)<=1 && int(b)-int(a)<=1; }
static const int16_t loud[] = {-4100, 4100};
static void settle(Envelope& envelope, uint32_t origin = 0) {
  for (uint32_t now=0; now<=200; now+=10) envelope.push(origin+now, loud, 2);
  assert(envelope.level()==255);
}

static void silence_and_missing_input_do_not_hold_the_target() {
  Envelope envelope(config());
  const int16_t quiet[] = {-90, 90};
  for (uint32_t now=0; now<400; now+=10) assert(envelope.push(now, quiet, 2)==0);
  settle(envelope, 400);
  for (uint32_t now=610; now<=1600; now+=10) envelope.push(now, nullptr, 2);
  assert(envelope.level()==0);
  assert(envelope.push(1610, loud, 0)==0);
}

static void attack_and_release_are_independent_of_poll_cadence() {
  Envelope fine(config()), medium(config()), coarse(config());
  assert(fine.push(0,loud,2)==0);
  medium.push(0,loud,2); coarse.push(0,loud,2);
  for (uint32_t now=1; now<=140; ++now) {
    fine.push(now,loud,2);
    if (now%10==0) medium.push(now,loud,2);
    if (now%20==0) {
      coarse.push(now,loud,2);
      assert(near(fine.level(),medium.level()) && near(fine.level(),coarse.level()));
    }
    // One exponential time constant covers roughly 63% of the distance.
    if (now==20) assert(fine.level()>=157 && fine.level()<=163);
  }
  assert(fine.level()==255 && coarse.level()==255);
  for (uint32_t now=141; now<=1000; ++now) {
    fine.decay(now);
    if (now%10==0) medium.decay(now);
    if (now%20==0) {
      coarse.decay(now);
      assert(near(fine.level(),medium.level()) && near(fine.level(),coarse.level()));
    }
  }
  assert(fine.level()==0 && coarse.level()==0);
}

static void hold_expiry_does_not_decay_the_time_before_its_deadline() {
  Envelope fine(config()), coarse(config()); settle(fine); settle(coarse);
  assert(fine.decay(339)==255);
  assert(fine.decay(340)==255); // Hold expires now, with no release time yet.
  for (uint32_t now=341; now<=380; ++now) fine.decay(now);
  coarse.decay(380); // Single poll spans the hold plus 40ms release.
  assert(near(fine.level(),coarse.level()));
  assert(coarse.level()>=169 && coarse.level()<=173);
}

static void new_audio_cannot_retroactively_fill_a_gap() {
  Envelope envelope(config()); settle(envelope);
  assert(envelope.push(5000,loud,2)==0);
  const uint8_t restarting=envelope.push(5010,loud,2);
  assert(restarting>90 && restarting<110);
}

static void rollover_and_reset_preserve_timing() {
  Envelope ordinary(config()), wrapped(config());
  const uint32_t base=UINT32_MAX-40;
  for (uint32_t elapsed=0; elapsed<=200; elapsed+=10)
    assert(ordinary.push(elapsed,loud,2)==wrapped.push(base+elapsed,loud,2));
  for (uint32_t elapsed=210; elapsed<=1200; elapsed+=10)
    assert(ordinary.decay(elapsed)==wrapped.decay(base+elapsed));
  wrapped.reset(); assert(wrapped.level()==0);
  assert(wrapped.push(6000,loud,2)==0);
}

static void magnitude_and_accumulation_do_not_overflow() {
  Envelope envelope({0,32768,0,0,140});
  const std::vector<int16_t> extreme(131072,INT16_MIN); // Sum is exactly 2^32.
  assert(envelope.push(0,extreme.data(),extreme.size())==255);
  const int16_t mixed[]={INT16_MIN,INT16_MAX,-1,1};
  assert(envelope.push(10,mixed,4)==127);
  const int16_t zero=0;
  assert(envelope.push(20,&zero,1)==0);
}

static void ordinary_tts_does_not_saturate_the_mouth() {
  Envelope envelope;
  const std::vector<int16_t> speech(1120,2600);
  for (uint32_t now=0; now<=700; now+=70) envelope.push(now,speech.data(),speech.size());
  assert(envelope.level()>100 && envelope.level()<150);
}

int main() {
  silence_and_missing_input_do_not_hold_the_target();
  attack_and_release_are_independent_of_poll_cadence();
  hold_expiry_does_not_decay_the_time_before_its_deadline();
  new_audio_cannot_retroactively_fill_a_gap();
  rollover_and_reset_preserve_timing();
  magnitude_and_accumulation_do_not_overflow();
  ordinary_tts_does_not_saturate_the_mouth();
  puts("test_audio_envelope: all cases passed");
}
