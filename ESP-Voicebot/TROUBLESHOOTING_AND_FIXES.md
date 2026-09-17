# ESP-Voicebot troubleshooting and fixes

Updated 2026-09-18. These notes describe the current implementation and distinguish software checks from hardware validation. The historical wire examples in `VOICEBOT_API_INSPECTION.md` are a protocol reference, not evidence of testing this revision on hardware.

## Issues fixed

| Symptom / risk | Cause in the previous firmware | Current behavior |
| --- | --- | --- |
| RAM exhaustion on a long greeting | Full 256 KiB WebSocket allocations plus 100/200 audio queue slots, with large internal-RAM fallbacks | Binary data streams through a 640 B scratch buffer. Queues have fixed PSRAM/internal budgets. No allocation grows with audio-message length. |
| Truncated replies | Nonblocking speaker queue sends ignored a full queue | Receive capacity is checked before reading binary PCM. A full speaker queue applies TCP backpressure. Unexpected enqueue failure aborts explicitly. |
| TLS corruption or intermittent disconnects | Capture, playback and loop tasks accessed one socket concurrently | Only the Arduino loop task uses the socket; audio workers exchange frames and atomic state. |
| Missing tail of fragmented audio / noise | Final continuation was omitted; text continuation could be sent to the speaker | The transport tracks message type, includes every binary continuation and reassembles bounded text separately. Split PCM samples retain one carry byte. |
| Slow, button-gated conversation | The button started/stopped every utterance and added 500 ms of synthetic silence | The button now starts/ends one call. Mic PCM streams across turns and the server owns utterance detection, matching the Preview Call contract. |
| Invalid TLS certificate | Bundled PEM was incomplete and start used insecure mode | Real GTS Root R4 from Google's official repository; verified TLS and valid NTP time required. Authentication query strings are not logged. |
| Repeated `fault=1` reconnects during long replies | Microphone backlog was treated as a fatal session error, despite available RAM | Four fixed microphone slots absorb DSP bursts, evicting the oldest packet on congestion. Packets older than 60 ms are rejected before upload. Congestion does not end the call or interrupt DSP history. |
| Speaker cannot hear an interruption / hears its own reply | Default half duplex muted the microphone; raw full duplex uploaded speaker echo | Native ESP-SR AEC cleans the microphone using the actual post-gain speaker PCM. With working PSRAM, cleaned audio streams during playback so the server can detect barge-in. |
| Echo cancellation drifts or uses audio that never played | Independent I2S clocks and references taken from queued network audio | TX/RX share I2S clocks, preserving existing GPIO wiring. Bounded DMA history pairs microphone blocks with the actual transmitted samples, including underrun silence and cancellation tails. The physical FIFO/acoustic offset still needs board validation. |
| Choppy PCM after adding DSP | AEC outputs 512 samples while network packets contain 320; speaker messages may end at arbitrary sample boundaries | Capture retains every packetization remainder. Playback packs contiguous samples into 160-sample DMA blocks and pads a final partial block only after confirming no queued audio is available. |
| Native AEC crashes on missing or exhausted PSRAM | The pinned library has unchecked inner allocations and requires PSRAM even with internal-memory allocation flags | Initialization checks PSRAM and conservative internal/external heap reserves before calling the constructor. AEC is created before Wi-Fi/allocation tasks. Failure selects explicit half duplex; it never silently enables raw full duplex. |
| Missing beginning of the next reply after barge-in | A two-second discard timer also consumed valid new TTS | Cancellation pauses binary reads until old playback drains and its notifications finish; subsequent audio is preserved in TCP. |
| Long text reply ends the call | An 8 KiB JSON arena could not hold ArduinoJson's growing string buffer plus envelope nodes | A fixed 12 KiB arena accepts tested 4,096/7,000-byte reply strings within the unchanged 8 KiB wire limit. |
| Stop/Start race while TLS is busy | Finite command queue and competing writes to the requested state | Versioned atomic intent preserves the newest press and invalidates old connection callbacks. Stop cancels queued audio immediately; rapid Stop/Start closes the old call before starting another. |
| Resource retention or silent connection stall | A socket can stop making progress without an immediate disconnect callback | Cleanup clears recording, partial PCM and queued playback. Keepalives and a traffic-aware deadline detect stalls. Network/upstream errors retry with bounded backoff while Start remains active, explicitly logging a new server session. |
| Unsafe partial startup | Queue, I2S or task failure could still allow normal loop execution | Networking remains disabled after startup failure, and initialized audio resources are released. Task creation is checked. |
| CPU/network starvation | Large frame reads, high-priority tasks, and blocking/bursty sends | Incremental receive work, one paced microphone packet per interval, bounded DMA callbacks and priority-2 audio tasks with queue waits and explicit capture yield. |
| Unused RAM/CPU work | Local VAD was calculated continuously but never controlled transmission | Unused detector instance and processing removed. The Botnoi server still provides VAD. Vendored VAD source remains available for future use. |

## Checks on the board

1. Confirm the exact module, flash and PSRAM mode. The default pin map targets ESP32-S3 N16R8 and needs **OPI PSRAM** for voice barge-in. `PSRAM free=0` means this capability is unavailable; a disabled/failed PSRAM configuration uses a small internal speaker queue and explicit half-duplex fallback. GPIO46 is a strapping pin; check the README before changing button pull resistors.
2. Capture boot logs at 115200 baud. Queue allocation, duplex I2S and both tasks must succeed. For voice barge-in, require `[AEC] AEC ready` and `[MIC] Echo-cancelled full duplex`. Inspect the reported native heap delta. The socket must remain closed at `[SYSTEM] Ready`; tap once and verify `[SESSION] Ready` appears only after verified TLS and a valid `opened` event.
3. Let the complete greeting play, then speak several turns without touching the button. Verify each response uses the same session id/context and begins promptly after server end-of-utterance detection. Tap once more only to hang up; the log should show the protocol close before transport shutdown.
4. With AEC ready, interrupt long replies at normal speaking volume and at several distances/angles. Repeat with quiet speech, loud playback and simultaneous speech. Barge-in must stop queued playback, keep the session, preserve the interruption's words and preserve the next reply. Confirm the bot's own voice does not trigger false interruptions. Measure mic/reference FIFO alignment and tune the finished enclosure if needed; a digital reference cannot correct amplifier clipping or mechanical vibration by itself.
5. Exercise rapid Stop/Start and Wi-Fi loss/recovery. After Wi-Fi recovers, active Start intent opens a visibly new server session; Stop must prevent every retry. Conversation restoration after a lost transport is not documented by the API. Authorization rejection and server completion must end the call without a reconnect loop.
6. Run for at least 30 minutes while recording `[RAM]`, `[STACK]` and `[AEC]` lines. Compare **internal free heap and largest block after equivalent idle states**; the historical minimum can only decrease. Repeat connects/disconnects. DSP processing must keep up with its 32 ms frame period; investigate repeated capture/reference overflows, alignment loss or rising microphone drops. Do not infer runtime safety from the linker RAM percentage alone.
7. If the ILI9341 panel is wired, verify `[FACE] ILI9341 320x240` at boot and a complete face before the backlight lights. Check the face is upright (otherwise set `VOICEBOT_DISPLAY_ROTATION 3`) and that the whites are white rather than inverted or blue-shifted. Confirm the mouth tracks the reply audio rather than lagging it by seconds, that the eyes blink and react while you talk, and that the frown appears on a latched audio fault. Measure whether the added SPI traffic changes `[AEC] max process`, microphone drops or the `[STACK]` minima; the face task is priority 1 and must lose to audio under load. Confirm the backlight supply, not a GPIO, carries the panel's LED current.
8. Check stack minima remain comfortably above zero during the busiest operations. Persistent low headroom requires adjustment and retesting on that board. Investigate any panic, watchdog reset, heap error or declining idle heap before treating the firmware as hardware validated.

## Log interpretation

- `[NTP] Waiting ...`: NTP has not supplied a valid clock. Wi-Fi/time retries continue; the device will not skip certificate validation.
- `[RAM] ... connection deferred`: available internal RAM or contiguous allocation is below the initial TLS guard. Review added features or board configuration; increasing PSRAM queue size will not fix internal heap fragmentation.
- `[MIC] Upload backlog ...`: the four-packet queue filled while socket/TLS work was busy, or a packet exceeded the 60 ms upload-age limit. The session remains open and continues from current audio. The 60 ms limit accommodates 32 ms DSP output bursts plus processing/pacing; tests showed the previous 40 ms limit discarded healthy audio.
- `[AEC] AEC unavailable ...`: PSRAM or initialization reserve is missing, or native initialization failed. The device remains half duplex; it cannot support voice barge-in in this state.
- `[AEC] ...`: frame count should advance continuously, including while Stop is active. Repeated RX/reference/alignment/DMA gaps indicate that capture or interrupt processing cannot keep up. After a discontinuity, partial capture packets are dropped and four AEC frames (128 ms) of output are silenced while valid history resumes.
- Fault **2**: invalid duplex DMA event or no aligned microphone/reference data for 500 ms. Check I2S startup and driver diagnostics.
- Fault **3**: current-generation speaker staging made no progress for 200 ms. Check TX DMA progress and scheduling.
- Fault **4**: unexpected playback-state or speaker enqueue failure despite the capacity contract.
- Fault **7**: native AEC processing or capture packet handoff failed. Check the preceding AEC diagnostics and memory/stack headroom.
- Faults **2**, **3**, **4** and **7** latch once and end the call. Check the reported hardware/pipeline fault and restart; the firmware does not repeatedly reconnect a broken audio pipeline. The old microphone faults **1**/**5** and button-queue fault **6** are removed.
- `Invalid JSON ... memory limit`: malformed, overly nested or overly complex control message; wire text is limited to 8 KiB and the JSON arena to 12 KiB. Increase a limit only after examining the actual message and RAM budget.
- `No inbound progress ...`: keepalive/transport progress has stalled for 60 seconds. Incoming audio, partial frame progress and deliberate receive backpressure keep a healthy busy connection alive.
- `Unsupported session audio format`: the server did not advertise unpaused audio/L16, 16 kHz, one channel as documented. Do not play another format as PCM16.
- `[FACE] Display unavailable ...`: the configured pins are incomplete or the face layout does not fit the selected rotation. The voicebot continues without the panel; audio is never blocked on the display.
- `[FACE] mood=...`: mood values follow `voicebot_face::Mood` (0 boot, 1 connecting, 2 idle, 3 listening, 4 speaking-wait/thinking, 5 speaking, 6 error). A mood stuck at 6 means a latched audio fault, not a display problem. Both levels are 0..255 envelope outputs; a speaker level that never leaves 0 during a reply points at the playback path rather than the panel.

## Validation scope

The repository includes reproducible Arduino builds for PSRAM enabled/disabled, sanitizer-backed host tests and a standalone actual ESP-SR DSP fixture. The current changes have not been flashed to a connected ESP32 in this session. The ILI9341 face has **not** run on a physical panel: its host suites cover the animation, the rasterized pixels, the command stream and the byte order, but rotation, colour order, backlight wiring, SPI timing margin and the visual result are unmeasured. Hardware audio quality, runtime TLS peaks, power stability, echo behavior and long-session context remain to be measured on the actual device. The supplied log shows application-triggered session reconnections with available internal RAM; it does not contain a boot banner, panic or reset cause establishing an ESP32 reboot.

### Results recorded on 2026-09-18

Toolchain: Arduino CLI 1.5.1, Espressif Arduino core 3.3.11 (bundled ESP-SR 2.4.6), ArduinoJson 7.4.3. Board: `esp32:esp32:esp32s3`, 16 MB flash, `app3M_fat9M_16MB`, USB CDC disabled. All three builds passed without compiler warnings using dummy credentials.

| Build | Flash bytes | Static internal RAM bytes | RAM after globals |
| --- | ---: | ---: | ---: |
| Default AEC, OPI PSRAM | 1,178,719 | 82,180 | 245,500 |
| Default AEC, PSRAM disabled (runtime fallback) | 1,173,509 | 81,728 | 245,952 |
| AEC explicitly disabled, OPI PSRAM | 1,149,663 | 79,092 | 248,588 |

The additional static RAM holds bounded microphone/reference history and packetizers. Runtime native DSP allocations, DMA, queues, task stacks and TLS are additional; these figures are not free-heap measurements. With PSRAM disabled, audio queue storage is 18,256 bytes (four 652-byte microphone packets plus 24 speaker frames).

All **11 host suites** passed AddressSanitizer and UndefinedBehaviorSanitizer. New coverage includes exact post-gain DMA references and cancellation tails, stale generations, staggered first callbacks, reordered/coalesced callbacks, ring overflow and timer rollover; continuous AEC frame history and 512-to-320 sample conversion; and speaker packetization across every chunk size from 1 to 320 samples. The pacing regression sends all 499 eligible packets under healthy modeled DSP timings from 6 to 25 ms with a 5 ms transport write; congestion still rejects stale audio. AEC wrapper doubles exercise admission, partial-init cleanup, alignment and resource lifetime.

Source-hash manifests and logs are in ignored artifacts `build/compile-ddtkov57/` (default matrix) and `build/compile-udm8xbcp/` (AEC disabled). These firmware sources match the current revision, apart from substituted test credentials. The real target missing-PSRAM branch passed on pinned Espressif QEMU 9.2.2 (`esp_develop_9.2.2_20260417`), artifact `build/aec-target-abrdofw6/`; it returned the fallback status without allocating native DSP.

With OPI PSRAM, the actual native AEC constructor reached Ready in QEMU, using 5,668 internal bytes and 121,512 PSRAM bytes including the wrapper's aligned buffers. This is an emulator allocation measurement, not a hardware runtime peak. The quality fixture stalled during the first DSP frame in the native HPS16 FFT instruction path, so **echo suppression, double-talk preservation, processing speed and processing-time heap stability are not verified**. Artifact `build/aec-target-u8mpq0jj/` records the failure and its no-progress timeout. The [standalone fixture](tests/aec_target/README.md) remains available for running on an actual ESP32-S3; the host wrapper tests do not substitute for this measurement.

### Pre-AEC baseline recorded on 2026-09-17 (`dd433ea`)

Toolchain: Arduino CLI 1.5.1, Espressif Arduino core 3.3.11, ArduinoJson 7.4.3.
Board: `esp32:esp32:esp32s3`, 16 MB flash, `app3M_fat9M_16MB`, USB CDC disabled.
Both persistent-session builds passed with no compiler warnings and used dummy credentials. These are historical results, superseded by the AEC build table above.

| Build | Flash bytes | Static internal RAM bytes | RAM after globals |
| --- | ---: | ---: | ---: |
| Original, OPI PSRAM | 1,125,243 | 48,372 | 279,308 |
| Persistent-session build, OPI PSRAM | 1,127,499 | 68,620 | 259,060 |
| Persistent-session build, PSRAM disabled | 1,122,309 | 68,168 | 259,512 |

The static RAM increase holds the fixed WebSocket text buffer and JSON arena.
It replaces allocation spikes at runtime. Without PSRAM, that revision's audio queue
storage is 16,300 bytes (one 652-byte microphone mailbox plus 24 speaker
frames), and a complete WebSocket payload no longer needs a separate allocation. Runtime heap is additional to
the static table; these are compiler results, not measured free heap on a board.

All seven host suites passed with AddressSanitizer and UndefinedBehaviorSanitizer,
including the 437,912-byte PCM fixture, larger WebSocket frames, fragmented text
and binary messages, capacity limits, malformed messages, timer rollover and
2,000 successive client JSON events. Further regressions cover 7,000-byte reply
text, 6,500-byte text plus 700-byte metadata, deferred pong under real socket
congestion, pre-open authorization rejection, receive backpressure over four
minutes, late I2S start/completion pairing and rapid Stop/Start intent. A live
TLS 1.2 handshake earlier in this session also verified the
hostname using only the bundled GTS Root R4 trust anchor; no authenticated
voicebot session or on-device playback was exercised.

Local build logs and source-hash manifest: `build/compile-gnymr_aq/summary.json`,
`opi.log`, and `none.log` (ignored build artifacts). The compiled firmware source
hashes match the pre-AEC baseline, except for deliberately substituted test credentials.
