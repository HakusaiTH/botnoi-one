# ESP-Voicebot: standalone ESP32-S3 voicebot

Streams INMP441 microphone audio directly to the Botnoi Voicebot WebSocket API over verified TLS, and plays PCM replies through MAX98357A. No PC bridge is required.

The supported target is **ESP32-S3**. An S3 with **8 MB OPI PSRAM** is preferred; the firmware also includes a small-buffer mode for S3 boards with PSRAM disabled or unavailable. This pin map is not suitable for the original ESP32. A compile-time guard catches the wrong board selection.

## Hardware

| Device | Signal | ESP32-S3 GPIO |
| --- | --- | ---: |
| INMP441 | BCLK / SCK | 3 |
| INMP441 | WS / LRCLK | 2 |
| INMP441 | DOUT / SD | 1 |
| MAX98357A | BCLK | 38 |
| MAX98357A | LRC | 39 |
| MAX98357A | DIN | 40 |
| External LED, with series resistor | Anode | 48 |
| Talk button, other side to GND | Signal | 46 |

Tie INMP441 L/R to GND for the left slot. Use a common ground and suitable power supply for the amplifier. The LED code expects an ordinary external LED, not an addressable RGB LED.

GPIO46 is a boot strapping pin. Preserve the existing wiring if it works; for a new design, GPIO4 is a convenient talk-button alternative: set `VOICEBOT_BUTTON_PIN 4` in `config.local.h`. Do not add a pull-up on GPIO46 without checking the board's boot/download requirements. [Espressif boot-mode documentation](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/advanced-topics/boot-mode-selection.html).

## Build and run

1. Install **esp32 by Espressif Systems 3.3.11** and **ArduinoJson 7.4.3** in Arduino IDE. WebSockets and the audio helpers are included in this sketch.
2. Copy `config.example.h` to **`config.local.h`** and enter your Wi-Fi SSID/password, Botnoi API key and agent ID. Local credentials are ignored by Git. The default endpoint is `voicebot-stg.botnoigroup.com`.
3. Select **ESP32-S3 Dev Module**, flash size matching your board (**16 MB** for the original hardware), **16M Flash (3MB APP/9.9MB FATFS)** partition scheme, and **OPI PSRAM** for the original N16R8 board. Select **Disabled** for a board without PSRAM; do not select OPI on incompatible hardware. USB CDC On Boot depends on the serial interface you use; the reproducible build uses Disabled.
4. Upload and open the serial monitor at **115200 baud**. Wi-Fi and NTP must be available before TLS can establish a session. Watch for `[SESSION] Ready`.
5. **Tap once to record; tap again to finish.** This is a toggle, not hold-to-talk. Finishing sends the queued microphone audio followed by 500 ms of paced silence so the server can detect the end of speech. Wait for the previous turn to finish sending before starting another.

`tls_roots.h` contains the genuine GTS Root R4 certificate. The current endpoint's chain was checked against that root on 2026-09-17. If you change the host or its CA changes, update the trust anchor; certificate validation is never bypassed. [Google Trust Services repository](https://pki.goog/repository/).

## RAM design

Audio memory is fixed at startup. Incoming binary messages are consumed in chunks of at most **640 bytes**; their full payload is never allocated. When the speaker queue fills, reception pauses until playback frees space, letting TCP apply backpressure. The receive path accepts binary messages up to 16 MiB without allocating by message size.

| Allocation | With PSRAM | Without PSRAM |
| --- | ---: | ---: |
| Microphone queue, 12 slots | 7,776 B internal | 7,776 B internal |
| Speaker queue, 800 or 24 slots | 518,400 B PSRAM | 15,552 B internal |
| Capture + playback task stacks | 8,192 B internal | 8,192 B internal |
| WebSocket text storage | 8,193 B static | 8,193 B static |
| JSON arena | 8,192 B static | 8,192 B static |

Queue slots include metadata and alignment. At full 20 ms chunks, speaker capacity is about 16 seconds with PSRAM or 480 ms without it; short network reads can reduce that duration. If the large PSRAM allocation fails, the firmware tries **only the small internal queue**, never a half-megabyte internal allocation. DMA, Wi-Fi, TLS, queue control blocks and other runtime allocations are additional to this table. Task stacks and DMA still need internal memory even when PSRAM is present. [Espressif external-RAM guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html).

All socket reads/writes run in `loop()`. Audio tasks never call TLS. Microphone overload aborts the session with a diagnostic instead of silently dropping speech. Initialization checks every queue, I2S device and task before enabling networking. The first connection is deferred unless at least 64 KiB internal heap and a 32 KiB contiguous internal block remain; this is a guard, not a guarantee about TLS's peak demand.

Every 30 seconds `[RAM]` reports internal free/minimum/largest-block bytes, PSRAM free bytes and outstanding speaker frames. `[STACK]` reports the smallest unused stack space observed for capture, playback and loop. Arduino's static-memory build report does **not** include runtime queues, task stacks, DMA or TLS.

## Validation

With Arduino CLI installed:

```sh
python3 ESP-Voicebot/scripts/compile.py --install-deps
python3 ESP-Voicebot/scripts/test_host.py --arduinojson /path/to/ArduinoJson/src
```

Run commands from the repository root. `compile.py` pins dependencies, stages **dummy credentials**, builds both OPI-PSRAM and PSRAM-disabled variants, and saves logs, firmware and `summary.json` under ignored `ESP-Voicebot/build/`. Those dummy-credential binaries are compile artifacts; build your configured sketch in Arduino IDE for actual use. To reuse a CLI toolchain, pass `--arduino-cli` and `--config-file`.

Host tests exercise the actual framing/PCM helpers and client logic with fake transport and real ArduinoJson, under AddressSanitizer and UndefinedBehaviorSanitizer. They cover large messages, fragmentation/control interleaving, queue capacity, sample alignment, JSON allocation limits/reuse, reconnect lifecycle, malformed input and silence pacing. See [TROUBLESHOOTING_AND_FIXES.md](TROUBLESHOOTING_AND_FIXES.md) for fixes and hardware checks.

On-device audio, Wi-Fi/TLS peak memory and a long-running soak test still require a connected board. Backpressure preserves bounded RAM; a server may still time out a slow receiver. A barge-in event behind already queued TCP audio cannot be processed until those preceding bytes are read. On receipt, queued playback is cancelled; a short amount already in I2S DMA may remain audible. Playback completion uses a 400 ms empty-queue grace period because the documented protocol has no explicit audio-end marker.
