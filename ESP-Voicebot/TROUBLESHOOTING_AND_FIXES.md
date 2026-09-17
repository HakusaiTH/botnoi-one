# ESP-Voicebot troubleshooting and fixes

Updated 2026-09-17. This replaces the earlier implementation notes with behavior verified in the current source. The historical wire examples in `VOICEBOT_API_INSPECTION.md` are a protocol reference, not evidence of testing this revision on hardware.

## Issues fixed

| Symptom / risk | Cause in the previous firmware | Current behavior |
| --- | --- | --- |
| RAM exhaustion on a long greeting | Full 256 KiB WebSocket allocations plus 100/200 audio queue slots, with large internal-RAM fallbacks | Binary data streams through a 640 B scratch buffer. Queues have fixed PSRAM/internal budgets. No allocation grows with audio-message length. |
| Truncated replies | Nonblocking speaker queue sends ignored a full queue | Receive capacity is checked before reading binary PCM. A full speaker queue applies TCP backpressure. Unexpected enqueue failure aborts explicitly. |
| TLS corruption or intermittent disconnects | Capture, playback and loop tasks accessed one socket concurrently | Only the Arduino loop task uses the socket; audio workers exchange frames and atomic state. |
| Missing tail of fragmented audio / noise | Final continuation was omitted; text continuation could be sent to the speaker | The transport tracks message type, includes every binary continuation and reassembles bounded text separately. Split PCM samples retain one carry byte. |
| Slow, button-gated conversation | The button started/stopped every utterance and added 500 ms of synthetic silence | The button now starts/ends one call. Mic PCM streams across turns and the server owns utterance detection, matching the Preview Call contract. |
| Invalid TLS certificate | Bundled PEM was incomplete and start used insecure mode | Real GTS Root R4 from Google's official repository; verified TLS and valid NTP time required. Authentication query strings are not logged. |
| Resource retention or lost context after TCP failure | Socket cleanup had been removed as a presumed handshake fix | EOF cleanup clears recording, partial PCM and queued playback. Transport loss ends the call; only a new button tap may create a new server session. |
| Unsafe partial startup | Queue, I2S or task failure could still allow normal loop execution | Networking remains disabled after startup failure, and initialized audio resources are released. Task creation is checked. |
| CPU/network starvation | Large frame reads, high-priority tasks, and blocking/bursty sends | Incremental receive work, one paced microphone frame per interval, priority-2 audio tasks with blocking I2S/queue operations and explicit capture yield. |
| Unused RAM/CPU work | Local VAD was calculated continuously but never controlled transmission | Unused detector instance and processing removed. The Botnoi server still provides VAD. Vendored VAD source remains available for future use. |

## Checks on the board

1. Confirm the exact module, flash and PSRAM mode. The default pin map targets ESP32-S3 N16R8. A PSRAM-disabled build uses the small internal queue. GPIO46 is a strapping pin; check the README before changing button pull resistors.
2. Capture boot logs at 115200 baud. Queue allocation, both I2S devices and both tasks must succeed. The socket must remain closed at `[SYSTEM] Ready`; tap once and verify `[SESSION] Ready` appears only after verified TLS and a valid `opened` event.
3. Let the complete greeting play, then speak several turns without touching the button. Verify each response uses the same session id/context and begins promptly after server end-of-utterance detection. Tap once more only to hang up; the log should show the protocol close before transport shutdown.
4. Exercise barge-in (full-duplex/AEC mode only) and Wi-Fi loss/recovery. Barge-in must stop queued playback, send completion when needed and discard late TTS. A lost Voicebot transport must end the active call; after Wi-Fi recovers, a new button tap starts a visibly new server session rather than silently replacing its context.
5. Run for at least 30 minutes while recording `[RAM]` and `[STACK]` lines. Compare **internal free heap and largest block after equivalent idle states**; the historical minimum can only decrease. Repeat connects/disconnects. Do not infer runtime safety from the linker RAM percentage alone.
6. Check stack minima remain comfortably above zero during the busiest operations. Persistent low headroom requires adjustment and retesting on that board. Investigate any panic, watchdog reset, heap error or declining idle heap before treating the firmware as hardware validated.

## Log interpretation

- `[NTP] Waiting ...`: NTP has not supplied a valid clock. Wi-Fi/time retries continue; the device will not skip certificate validation.
- `[RAM] ... connection deferred`: available internal RAM or contiguous allocation is below the initial TLS guard. Review added features or board configuration; increasing PSRAM queue size will not fix internal heap fragmentation.
- `[MIC] Upload backlog ...`: while socket/TLS work was busy, a newer 20 ms microphone frame replaced the single pending frame, or a frame exceeded the 40 ms upload-age limit. The session remains open and continues from current audio; frequent growth means socket/TLS work is still blocking too long.
- Fault **2** / **3**: microphone / speaker I2S failure. Check hardware and inspect the Arduino core's I2S diagnostics.
- Fault **4**: unexpected speaker enqueue failure despite the capacity contract. The session resets rather than continuing with missing samples.
- Fault **5**: the one-slot microphone mailbox rejected an overwrite. This should not occur after successful queue initialization; the call ends rather than continuing with uncertain capture state.
- Fault **6**: more than eight debounced session-button actions accumulated before the loop could service them. The call ends safely instead of losing an ordered hangup/start command.
- `Invalid JSON ... memory limit`: malformed, overly nested or overly complex control message; wire text and its JSON arena each have an 8 KiB bound. Increase a limit only after examining the actual message and RAM budget.
- `Unsupported session audio format`: the server did not advertise unpaused audio/L16, 16 kHz, one channel as documented. Do not play another format as PCM16.

## Validation scope

The repository includes reproducible Arduino builds for PSRAM enabled/disabled and sanitizer-backed host tests for framing, PCM freshness/pacing, graceful close and protocol lifecycle. The current changes have not been flashed to a connected ESP32 in this session. Hardware audio quality, runtime TLS peaks, power stability, echo behavior and long-session context remain to be measured on the actual device.

### Results recorded on 2026-09-17

Toolchain: Arduino CLI 1.5.1, Espressif Arduino core 3.3.11, ArduinoJson 7.4.3.
Board: `esp32:esp32:esp32s3`, 16 MB flash, `app3M_fat9M_16MB`, USB CDC disabled.
Both updated builds passed with no compiler warnings and used dummy credentials.

| Build | Flash bytes | Static internal RAM bytes | RAM after globals |
| --- | ---: | ---: | ---: |
| Original, OPI PSRAM | 1,125,243 | 48,372 | 279,308 |
| Persistent-session build, OPI PSRAM | 1,124,911 | 64,468 | 263,212 |
| Persistent-session build, PSRAM disabled | 1,119,705 | 64,008 | 263,672 |

The static RAM increase holds the fixed WebSocket text buffer and JSON arena.
It replaces allocation spikes at runtime. Without PSRAM, current audio queue
storage is 16,300 bytes (one 652-byte microphone mailbox plus 24 speaker
frames), and a complete WebSocket payload no longer needs a separate allocation. Runtime heap is additional to
the static table; these are compiler results, not measured free heap on a board.

All host suites passed with AddressSanitizer and UndefinedBehaviorSanitizer,
including the 437,912-byte PCM fixture, larger WebSocket frames, fragmented text
and binary messages, capacity limits, malformed messages, timer rollover and
2,000 successive client JSON events. A live TLS 1.2 handshake also verified the
hostname using only the bundled GTS Root R4 trust anchor; no authenticated
voicebot session or on-device playback was exercised.

Local build logs and source-hash manifest: `build/compile-_l1gfwmm/summary.json`,
`opi.log`, and `none.log` (ignored build artifacts). The compiled firmware source
hashes match this revision, except for deliberately substituted test credentials.
