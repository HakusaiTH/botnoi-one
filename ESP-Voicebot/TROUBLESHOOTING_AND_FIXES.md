# ESP-Voicebot troubleshooting and fixes

Updated 2026-09-17. This replaces the earlier implementation notes with behavior verified in the current source. The repository-root lowercase `troubleshooting_and_fixes.md` is a saved Chrome error page and was left untouched. The historical wire examples in `VOICEBOT_API_INSPECTION.md` are a protocol reference, not evidence of testing this revision on hardware.

## Issues fixed

| Symptom / risk | Cause in the previous firmware | Current behavior |
| --- | --- | --- |
| RAM exhaustion on a long greeting | Full 256 KiB WebSocket allocations plus 100/200 audio queue slots, with large internal-RAM fallbacks | Binary data streams through a 640 B scratch buffer. Queues have fixed PSRAM/internal budgets. No allocation grows with audio-message length. |
| Truncated replies | Nonblocking speaker queue sends ignored a full queue | Receive capacity is checked before reading binary PCM. A full speaker queue applies TCP backpressure. Unexpected enqueue failure aborts explicitly. |
| TLS corruption or intermittent disconnects | Capture, playback and loop tasks accessed one socket concurrently | Only the Arduino loop task uses the socket; audio workers exchange frames and atomic state. |
| Missing tail of fragmented audio / noise | Final continuation was omitted; text continuation could be sent to the speaker | The transport tracks message type, includes every binary continuation and reassembles bounded text separately. Split PCM samples retain one carry byte. |
| Session stalls after finishing speech | Capture sent silence directly while microphone frames were still queued | An ordered end marker follows captured PCM; loop sends 25 silence frames at 20 ms intervals after it. No blocking 500 ms padding loop. |
| Invalid TLS certificate | Bundled PEM was incomplete and start used insecure mode | Real GTS Root R4 from Google's official repository; verified TLS and valid NTP time required. Authentication query strings are not logged. |
| Resource retention after lost TCP | Socket cleanup had been removed as a presumed handshake fix | Synchronous connection setup and EOF cleanup are handled separately. Reconnect clears session, recording, partial PCM and queued playback. |
| Unsafe partial startup | Queue, I2S or task failure could still allow normal loop execution | Networking remains disabled after startup failure, and initialized audio resources are released. Task creation is checked. |
| CPU/network starvation | Large frame reads, high-priority tasks, and blocking silence sends | Incremental receive work, bounded microphone work per loop, priority-2 audio tasks with blocking I2S/queue operations and explicit capture yield. |
| Unused RAM/CPU work | Local VAD was calculated continuously but never controlled transmission | Unused detector instance and processing removed. The Botnoi server still provides VAD. Vendored VAD source remains available for future use. |

## Checks on the board

1. Confirm the exact module, flash and PSRAM mode. The default pin map targets ESP32-S3 N16R8. A PSRAM-disabled build uses the small internal queue. GPIO46 is a strapping pin; check the README before changing button pull resistors.
2. Capture boot logs at 115200 baud. Queue allocation, both I2S devices and both tasks must succeed. `[SESSION] Ready` appears only after verified TLS and the server's valid `opened` event.
3. Play the complete long greeting, record a sentence, tap to finish, and verify the whole reply. Repeat with long replies and multiple turns. The speaker queue should drain without fault 4; recorded speech must not produce fault 1 under normal Wi-Fi conditions.
4. Exercise barge-in and Wi-Fi loss/recovery. Previous-session audio must not play after reconnection. Once the new session is ready, recording requires another tap.
5. Run for at least 30 minutes while recording `[RAM]` and `[STACK]` lines. Compare **internal free heap and largest block after equivalent idle states**; the historical minimum can only decrease. Repeat connects/disconnects. Do not infer runtime safety from the linker RAM percentage alone.
6. Check stack minima remain comfortably above zero during the busiest operations. Persistent low headroom requires adjustment and retesting on that board. Investigate any panic, watchdog reset, heap error or declining idle heap before treating the firmware as hardware validated.

## Log interpretation

- `[NTP] Waiting ...`: NTP has not supplied a valid clock. Wi-Fi/time retries continue; the device will not skip certificate validation.
- `[RAM] ... connection deferred`: available internal RAM or contiguous allocation is below the initial TLS guard. Review added features or board configuration; increasing PSRAM queue size will not fix internal heap fragmentation.
- Fault **1**: microphone backlog exceeded the bounded queue. The current turn is aborted instead of corrupting speech. Check Wi-Fi/server responsiveness and blocking work added to loop.
- Fault **2** / **3**: microphone / speaker I2S failure. Check hardware and inspect the Arduino core's I2S diagnostics.
- Fault **4**: unexpected speaker enqueue failure despite the capacity contract. The session resets rather than continuing with missing samples.
- `Invalid JSON ... memory limit`: malformed, overly nested or overly complex control message; wire text and its JSON arena each have an 8 KiB bound. Increase a limit only after examining the actual message and RAM budget.
- `Unsupported session audio format`: the server did not advertise unpaused audio/L16, 16 kHz, one channel as documented. Do not play another format as PCM16.

## Validation scope

The repository includes reproducible Arduino builds for PSRAM enabled/disabled and sanitizer-backed host tests for framing, PCM, silence timing and protocol lifecycle. The current changes have not been flashed to a connected ESP32 in this session. Hardware audio quality, runtime TLS peaks, power stability and long-session behavior remain to be measured on the actual device.

### Results recorded on 2026-09-17

Toolchain: Arduino CLI 1.5.1, Espressif Arduino core 3.3.11, ArduinoJson 7.4.3.
Board: `esp32:esp32:esp32s3`, 16 MB flash, `app3M_fat9M_16MB`, USB CDC disabled.
Both updated builds passed with no compiler warnings and used dummy credentials.

| Build | Flash bytes | Static internal RAM bytes | RAM after globals |
| --- | ---: | ---: | ---: |
| Original, OPI PSRAM | 1,125,243 | 48,372 | 279,308 |
| Updated, OPI PSRAM | 1,121,527 | 64,276 | 263,404 |
| Updated, PSRAM disabled | 1,116,305 | 63,824 | 263,856 |

The static RAM increase holds the fixed WebSocket text buffer and JSON arena.
It replaces allocation spikes at runtime. Without PSRAM, audio queue storage
falls from 192,600 to 23,328 bytes (about 88% less), and a complete WebSocket
payload no longer needs a separate allocation. Runtime heap is additional to
the static table; these are compiler results, not measured free heap on a board.

All three host suites passed with AddressSanitizer and UndefinedBehaviorSanitizer,
including the 437,912-byte PCM fixture, larger WebSocket frames, fragmented text
and binary messages, capacity limits, malformed messages, timer rollover and
2,000 successive client JSON events. A live TLS 1.2 handshake also verified the
hostname using only the bundled GTS Root R4 trust anchor; no authenticated
voicebot session or on-device playback was exercised.

Local build logs and source-hash manifest: `build/compile-wn9z9o6v/summary.json`,
`opi.log`, and `none.log` (ignored build artifacts). The compiled firmware source
hashes match this revision, except for deliberately substituted test credentials.
