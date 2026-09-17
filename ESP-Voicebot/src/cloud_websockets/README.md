This directory contains the client-only subset of
[Links2004/arduinoWebSockets 2.7.2](https://github.com/Links2004/arduinoWebSockets/tree/2.7.2),
under LGPL-2.1, with the upstream license and libb64 notices retained. Original
file SHA-256 hashes are recorded in `upstream-sha256.json`. Source line endings
were normalized to LF.

This copy is specialized for the synchronous ESP32 voicebot client.
`voicebot.patch` records all differences from those four upstream client files
and adds `WebSocketsStream.h` and `WebSocketsWritable.h`. It replaces the old `byteplus.patch` and uses
zero-context diffs so the patch artifact has no trailing whitespace. Apply it
to LF-normalized upstream sources with `git apply --unidiff-zero voicebot.patch`.

- Binary payloads stream as `WStype_BIN` events of at most 640 bytes, including
  the final continuation. RAM does not grow with frame or message length. Each
  `loop()` reads one parser boundary and at most 640 payload bytes (one 20 ms
  PCM16 speaker queue slot). A binary
  message may contain up to 16 MiB, regardless of fragmentation.
- `setBinaryReceiveCapacity(callback)` reports available application queue
  space before the next TCP/TLS payload read. Zero capacity pauses reading
  without blocking. `isReceivingBinary()` remains true during unfinished binary
  headers, frames, and fragmented messages so playback can track pending audio.
- `canSendNow() const` checks ESP32 TCP write readiness with a zero-timeout
  `select`, without reading from TLS or changing connection state. Check it
  before removing a microphone packet from its queue or sending a control
  message. False means continue receiving and try on a later loop. Socket
  readiness avoids ordinary TCP congestion waits but cannot guarantee that TLS
  finishes a write immediately; failed/partial writes still close the session.
- The receiver owns an 8,193-byte text buffer, a 125-byte control buffer, and
  small parser state. Receive uses 640 bytes of stack scratch; transmit uses
  654 bytes. No frame-sized allocation or PSRAM fallback occurs.
- Fragmented text is delivered once as `WStype_TEXT`, capped at 8,192 bytes in
  total and validated as UTF-8. Ping/pong/close may interleave with fragments.
  Invalid masking, reserved bits/opcodes, continuation order, control-frame
  lengths, and nonminimal/oversized extended lengths close the connection.
- Incomplete input has a 15-second inactivity deadline. Application backpressure
  suspends that deadline and heartbeat timeouts; elapsed pause time cannot
  immediately expire the next frame. Buffered bytes are drained before EOF,
  then all receive state and network objects are reset on disconnect.
- HTTP response parsing is incremental, with a 1 KiB line cap and 8 KiB total
  cap. TCP connection/write timeouts are 3 seconds; ESP32 TLS handshake timeout
  is 8 **seconds**, matching the ESP32 API's units. HTTP upgrade has a separate
  3-second deadline. Library logging remains disabled to protect credentials.
- Outgoing frames use a fresh ESP32 hardware-random mask and fixed scratch
  storage, including callers with reserved header space. The source PCM is
  unchanged, common 640-byte audio packets use one TLS write, and a partial
  write closes TCP because the damaged frame cannot safely be retried.

`../../voicebot_client.h` owns the connection. All socket operations and receive
callbacks run on the task that calls `loop()`. A capacity callback must reserve
enough space for the following binary event; it must not claim space another
producer can consume first. A slow consumer applies TCP backpressure, so control
frames behind an unfinished binary frame must wait for that audio to drain.
Connection establishment and a stalled outbound write can still block up to
their configured deadlines.

The framing rules follow [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455),
especially sections 5.2, 5.4 and 5.5. The host regression test exercises a 768 KiB
frame, small queue capacity, fragmented binary/text, control interleaving,
UTF-8/length rejection, disconnect reset, and timeout/clock rollover:

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  ESP-Voicebot/tests/test_websocket_stream.cpp -o /tmp/voicebot-websocket-test
/tmp/voicebot-websocket-test
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  ESP-Voicebot/tests/test_websocket_writable.cpp -o /tmp/voicebot-writable-test
/tmp/voicebot-writable-test
```

No installed WebSockets library is required for this sketch. Arduino recursively
compiles this `src` directory. The server and Socket.IO sources are omitted.
