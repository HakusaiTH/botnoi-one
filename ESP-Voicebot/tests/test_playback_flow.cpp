#include "../playback_flow.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using voicebot_audio::PlaybackAction;
using voicebot_audio::PlaybackFlow;

static void normal_end_hint_and_control_backpressure() {
  PlaybackFlow flow;
  assert(!flow.blocksAudio() && !flow.micPaused());
  assert(!flow.acknowledge(PlaybackAction::Started));
  assert(flow.onAudio(1000, 7));
  assert(flow.micPaused());
  assert(flow.nextAction(1000, 0, 1, false, 0, true) == PlaybackAction::None);
  assert(flow.nextAction(1001, 7, 1, false, 0, true) == PlaybackAction::Started);
  // TCP cannot send yet. Merely proposing a message must not change its state.
  assert(flow.nextAction(1050, 7, 1, false, 0, true) == PlaybackAction::Started);
  assert(!flow.acknowledge(PlaybackAction::Completed));
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.nextAction(1051, 7, 1, false, 0, true) == PlaybackAction::None);
  flow.onEndHint();
  assert(flow.nextAction(1219, 7, 0, true, 1100, false) == PlaybackAction::None);
  assert(flow.nextAction(1220, 7, 1, true, 1100, false) == PlaybackAction::None);
  assert(flow.nextAction(1220, 7, 0, false, 1100, false) == PlaybackAction::None);
  assert(flow.nextAction(1220, 7, 0, true, 1100, true) == PlaybackAction::None);
  assert(flow.nextAction(1220, 7, 0, true, 1100, false) == PlaybackAction::Completed);
  assert(flow.micPaused());
  assert(flow.nextAction(1600, 7, 0, true, 1100, false) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
  assert(!flow.micPaused() && !flow.blocksAudio());
  assert(flow.nextAction(1700, 7, 0, true, 1100, false) == PlaybackAction::None);
  assert(!flow.acknowledge(PlaybackAction::Completed));
}

static void greeting_order_and_zero_timestamp() {
  PlaybackFlow flow;
  flow.onEndHint();  // Greeting metadata precedes its audio.
  assert(flow.onAudio(0, 1));
  assert(flow.nextAction(0, 1, 0, true, 0, false) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.nextAction(120, 1, 0, true, 0, false) == PlaybackAction::None);
  assert(flow.nextAction(599, 1, 0, true, 0, false) == PlaybackAction::None);
  assert(flow.nextAction(600, 1, 0, true, 0, false) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
  assert(!flow.micPaused());
}

static void new_audio_invalidates_an_older_end_hint() {
  PlaybackFlow flow;
  assert(flow.onAudio(100, 2));
  assert(flow.nextAction(100, 2, 1, false, 0, true) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  flow.onEndHint();
  assert(flow.onAudio(200, 2));
  assert(flow.nextAction(320, 2, 0, true, 200, false) == PlaybackAction::None);
  assert(flow.nextAction(799, 2, 0, true, 200, false) == PlaybackAction::None);
  assert(flow.nextAction(800, 2, 0, true, 200, false) == PlaybackAction::Completed);
  // A later byte arriving before the proposed control was sent revokes it.
  assert(flow.onAudio(801, 2));
  assert(!flow.acknowledge(PlaybackAction::Completed));
  flow.onEndHint();
  assert(flow.nextAction(921, 2, 0, true, 801, false) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
}

static void cancellation_after_started_and_immediate_next_reply() {
  PlaybackFlow flow;
  assert(flow.onAudio(10, 8));
  assert(flow.nextAction(11, 8, 1, false, 0, true) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.cancel(8));
  assert(flow.blocksAudio() && flow.micPaused());
  assert(!flow.cancel(9));
  assert(!flow.onAudio(12, 9));
  assert(flow.nextAction(139, 8, 1, true, 20, true) == PlaybackAction::None);
  assert(flow.nextAction(139, 8, 0, true, 20, true) == PlaybackAction::None);
  // Binary reception may still be paused mid-message. It does not gate cancel.
  assert(flow.nextAction(140, 8, 0, true, 20, true) == PlaybackAction::Completed);
  assert(flow.nextAction(900, 8, 0, true, 20, true) == PlaybackAction::Completed);
  assert(flow.blocksAudio() && flow.micPaused());
  assert(flow.acknowledge(PlaybackAction::Completed));
  assert(!flow.blocksAudio() && !flow.micPaused());
  // There is no discard timer: the very next reply is accepted immediately.
  assert(flow.onAudio(901, 9));
  assert(flow.nextAction(901, 8, 1, false, 0, true) == PlaybackAction::None);
  assert(flow.nextAction(902, 9, 1, false, 0, true) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  flow.onEndHint();
  assert(flow.nextAction(1022, 9, 0, true, 902, false) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
}

static void cancellation_before_notification_and_late_i2s_start() {
  PlaybackFlow flow;
  assert(flow.onAudio(10, 20));
  assert(flow.cancel(20));
  assert(!flow.cancel(21));  // Do not replace the cancelled generation.
  assert(flow.nextAction(11, 0, 1, false, 0, true) == PlaybackAction::None);
  // An I2S call already in flight accepts samples after the barge-in arrived.
  assert(flow.nextAction(12, 20, 1, false, 0, true) == PlaybackAction::None);
  assert(flow.nextAction(139, 20, 0, true, 20, true) == PlaybackAction::None);
  assert(flow.nextAction(140, 20, 0, true, 20, true) == PlaybackAction::Started);
  assert(flow.nextAction(150, 20, 0, true, 20, true) == PlaybackAction::Started);
  assert(!flow.acknowledge(PlaybackAction::Completed));
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.blocksAudio() && flow.micPaused());
  assert(flow.nextAction(150, 20, 0, true, 20, true) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
  assert(!flow.blocksAudio() && !flow.micPaused());
}

static void cancellation_of_offered_but_unsent_start() {
  PlaybackFlow flow;
  assert(flow.onAudio(0, 4));
  assert(flow.nextAction(1, 4, 1, false, 0, true) == PlaybackAction::Started);
  assert(flow.cancel(4));
  assert(!flow.acknowledge(PlaybackAction::Started));
  assert(flow.nextAction(2, 4, 1, false, 0, true) == PlaybackAction::None);
  assert(flow.nextAction(122, 4, 0, true, 2, false) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.nextAction(122, 4, 0, true, 2, false) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
}

static void unplayed_cancellation_has_no_control_messages() {
  PlaybackFlow flow;
  assert(flow.onAudio(0, 30));
  assert(flow.cancel(30));
  assert(flow.nextAction(0, 0, 0, false, 0, true) == PlaybackAction::None);
  assert(flow.blocksAudio() && flow.micPaused());
  assert(flow.nextAction(119, 0, 0, true, 0, true) == PlaybackAction::None);
  assert(flow.blocksAudio() && flow.micPaused());
  assert(flow.nextAction(120, 0, 0, true, 0, true) == PlaybackAction::None);
  assert(!flow.blocksAudio() && !flow.micPaused());
  assert(!flow.acknowledge(PlaybackAction::Started));

  // A barge-in can precede all TTS; its guard still has a valid zero timestamp.
  assert(flow.cancel(31));
  assert(flow.nextAction(119, 0, 0, true, 0, false) == PlaybackAction::None);
  assert(flow.blocksAudio());
  assert(flow.nextAction(120, 0, 0, true, 0, false) == PlaybackAction::None);
  assert(!flow.blocksAudio());
}

static void rollover_reset_and_generation_validation() {
  PlaybackFlow flow;
  assert(!flow.onAudio(0, 0));
  const uint32_t firstAt = UINT32_MAX - 50;
  const uint32_t drainedAt = UINT32_MAX - 10;
  assert(flow.onAudio(firstAt, 50));
  assert(!flow.onAudio(firstAt + 1, 51));
  assert(flow.nextAction(firstAt, 50, 1, false, 0, true) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  flow.onEndHint();
  assert(flow.nextAction(drainedAt + 119, 50, 0, true, drainedAt, false) == PlaybackAction::None);
  assert(flow.nextAction(drainedAt + 120, 50, 0, true, drainedAt, false) == PlaybackAction::Completed);
  flow.reset();
  assert(!flow.acknowledge(PlaybackAction::Completed));
  assert(!flow.micPaused() && !flow.blocksAudio());

  assert(flow.onAudio(firstAt, 51));
  assert(flow.cancel(51));
  assert(flow.nextAction(drainedAt + 119, 51, 0, true, drainedAt, true) == PlaybackAction::None);
  assert(flow.nextAction(drainedAt + 120, 51, 0, true, drainedAt, true) == PlaybackAction::Started);
  assert(flow.acknowledge(PlaybackAction::Started));
  assert(flow.nextAction(drainedAt + 120, 51, 0, true, drainedAt, true) == PlaybackAction::Completed);
  assert(flow.acknowledge(PlaybackAction::Completed));
}

int main() {
  static_assert(sizeof(PlaybackFlow) <= 32, "Playback state must stay fixed and small");
  normal_end_hint_and_control_backpressure();
  greeting_order_and_zero_timestamp();
  new_audio_invalidates_an_older_end_hint();
  cancellation_after_started_and_immediate_next_reply();
  cancellation_before_notification_and_late_i2s_start();
  cancellation_of_offered_but_unsent_start();
  unplayed_cancellation_has_no_control_messages();
  rollover_reset_and_generation_validation();
  std::puts("Playback flow: control pairing, cancellation, late I2S, drain guards, immediate replies and clock rollover passed");
}
