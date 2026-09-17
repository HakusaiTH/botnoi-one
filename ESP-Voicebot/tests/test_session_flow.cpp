#include "../session_flow.h"
#include <cassert>
#include <cstdio>
#include <stdint.h>

using voicebot_session::SessionIntent;
using voicebot_session::ReconnectBackoff;

int main() {
  SessionIntent intent;
  uint32_t token = 0;
  assert(!intent.requested() && !intent.consume(token));
  intent.publish(true);
  assert(intent.consume(token) && intent.current(token));
  const uint32_t firstCall = token;
  // An arbitrary number of conversation turns never creates a new request.
  for (int turn = 0; turn < 10000; ++turn) {
    assert(!intent.consume(token) && intent.current(firstCall));
  }
  // Stop/Start while connect or a TLS write blocks must replace the old call,
  // even though the final desired state is still Start.
  intent.publish(false);
  assert(!intent.current(firstCall));
  intent.publish(true);
  assert(!intent.end(firstCall));
  assert(intent.consume(token) && intent.requested(token) && token != firstCall);
  const uint32_t secondCall = token;
  // Physical Stop wins over any stale opening/recovery callback.
  intent.publish(false);
  assert(!intent.current(secondCall));
  assert(!intent.end(secondCall));
  assert(intent.consume(token) && !intent.requested(token));
  // No finite button queue can overflow under a stalled socket.
  for (int press = 0; press < 10000; ++press) intent.publish((press & 1) == 0);
  assert(intent.consume(token) && !intent.requested(token));
  intent.publish(true);
  assert(intent.consume(token));
  assert(intent.end(token));
  assert(intent.consume(token) && !intent.requested());

  ReconnectBackoff retry;
  retry.reset(0);
  assert(retry.due(0));
  assert(retry.failed(0, 0) == 1000);
  assert(!retry.due(999) && retry.due(1000));
  retry.opened(1000);
  retry.maintain(59999);
  assert(retry.failed(60000, 250) == 2250); // short call retains backoff
  assert(!retry.due(62249) && retry.due(62250));
  retry.opened(62250);
  retry.maintain(122250);
  assert(retry.failed(122250, 0) == 1000); // stable call resets it
  for (int failure = 0; failure < 100; ++failure) {
    assert(retry.failed(200000, 250) <= 30000);
  }
  assert(!retry.due(229999) && retry.due(230000));
  // Both retry deadlines and stable-connection accounting cross millis wrap.
  retry.reset(UINT32_MAX - 499);
  assert(retry.failed(UINT32_MAX - 499, 0) == 1000);
  assert(!retry.due(499) && retry.due(500));
  retry.opened(UINT32_MAX - 1000);
  retry.maintain(58999);
  assert(retry.failed(60000, 0) == 1000);
  puts("Session intent and reconnect tests passed.");
}
