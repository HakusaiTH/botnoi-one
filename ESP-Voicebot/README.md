# ESP-Voicebot: standalone ESP32-S3 voicebot

Streams INMP441 microphone audio directly to the Botnoi Voicebot WebSocket API over verified TLS, and plays PCM replies through MAX98357A. An optional 2.8" ILI9341 panel shows an animated robot face whose mouth moves with the reply audio. No PC bridge is required.

The supported target is **ESP32-S3**. Smart-speaker voice barge-in uses Espressif acoustic echo cancellation (AEC) and **requires working PSRAM**; select **OPI PSRAM** for the original N16R8 module. Boards with PSRAM disabled or unavailable retain a bounded half-duplex fallback, which listens again after the bot finishes. This pin map is not suitable for the original ESP32. A compile-time guard catches the wrong board selection.

## Hardware

The current wiring follows [hardware_pinout.md](hardware_pinout.md) for the GOOUUU ESP32-S3-CAM V1.5. Its TFT output pins follow the [GOOUUU expansion-board reference](https://github.com/profharris/GOOUUU-Tech-ESP32-S3-CAM-Expansion-Board#lcd-28in-240320-spi-tft-display-ili9341); the unused SDO connection is removed for the session button. Audio pins are shared between the I2S driver and display conflict checks through `hardware_pins.h`.

| Device | Signal | ESP32-S3 GPIO |
| --- | --- | ---: |
| INMP441 | BCLK / SCK | 48 |
| INMP441 | WS / LRCLK | 2 |
| INMP441 | DOUT / SD | 1 |
| MAX98357A | BCLK | 38 |
| MAX98357A | LRC | 39 |
| MAX98357A | DIN | 40 |
| External LED, with series resistor | Anode | 13 |
| Session button, other side to GND | Signal | 46 |
| ILI9341 panel | SCK / CLK | 3 |
| ILI9341 panel | MOSI / SDI | 45 |
| ILI9341 panel | MISO / SDO | Not connected |
| ILI9341 panel | DC / RS | 47 |
| ILI9341 panel | CS | 14 |
| ILI9341 panel | RESET | 21 |
| ILI9341 panel | LED / BL | 3.3V |

Tie INMP441 L/R to GND for the left slot. Use a common ground and suitable power supply for the amplifier. The LED code expects an ordinary external LED, not an addressable RGB LED.

Leave the panel's **MISO / SDO disconnected from GPIO46**, which now belongs to the session button. The display is write-only and does not need MISO. If the expansion board connects SDO to GPIO46, isolate that connection before using the button. Touch is unsupported with this audio wiring. Every display pin is configurable in `config.local.h`, and `VOICEBOT_DISPLAY_ENABLED 0` builds audio-only firmware.

**GPIO45 is a strapping pin** (VDD_SPI voltage select) sampled at reset; it is an ordinary output afterwards, but do not add an external pull resistor to it. If your panel's RESET is tied to the board's reset line, set `VOICEBOT_DISPLAY_RESET_PIN -1`.

**The backlight is connected to 3.3V** (`VOICEBOT_DISPLAY_BACKLIGHT_PIN -1`). The face comes up already lit. If driving backlight via GPIO, ensure the module does not exceed the pin's rated continuous current. [Espressif ESP32-S3 pin documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html).

GPIO46 is the **external** session button (`VOICEBOT_BUTTON_PIN 46`), configured with the internal pull-up and pressed by connecting to GND; the expansion board's built-in KEY/BOOT button uses GPIO0. Open expansion jumpers **P7/P8** for microphone operation, and do not use the SD peripheral that shares amplifier pins. See the [shared-peripheral checklist](hardware_pinout.md#5-expansion-board-peripherals-sharing-these-pins), including the unused touch connections.

## Build and run

1. Install **esp32 by Espressif Systems 3.3.11** and **ArduinoJson 7.4.3** in Arduino IDE. The core already includes **ESP-SR 2.4.6** and its native AEC library; no extra speech models or model partition are required. WebSockets and the audio helpers are included in this sketch.
2. Copy `config.example.h` to **`config.local.h`** and enter your Wi-Fi SSID/password, Botnoi API key and agent ID. Local credentials are ignored by Git. The default endpoint is `voicebot-stg.botnoigroup.com`.
3. Select **ESP32-S3 Dev Module**, flash size matching your board (**16 MB** for the original hardware), **16M Flash (3MB APP/9.9MB FATFS)** partition scheme, and **OPI PSRAM** for the original N16R8 board. Select **Disabled** for a board without PSRAM; do not select OPI on incompatible hardware. USB CDC On Boot depends on the serial interface you use; the reproducible build uses Disabled.
4. Upload and open the serial monitor at **115200 baud**. For voice barge-in, verify `[AEC] AEC ready` and `[MIC] Echo-cancelled full duplex` before `[SYSTEM] Ready`. A half-duplex fallback log means voice barge-in is unavailable; check the PSRAM boot diagnostics and board setting. The firmware does not open a conversational session at boot.
5. **Tap once to start the call; speak naturally for as many turns as needed; tap again to hang up.** The same WebSocket and Voicebot session stay open across every turn. The Botnoi service detects utterance boundaries from the continuous 20 ms PCM stream. Hangup stops microphone upload immediately, sends protocol `close` with reason `end`, waits briefly for `closed`, then closes the transport. This follows the official [Realtime Preview Call contract](https://voicebot-stg.botnoigroup.com/docs/preview-call).

AEC is enabled by default (`VOICEBOT_AEC_ENABLED 1`). It receives the microphone and the speaker's actual post-gain PCM, including underrun silence and the remaining DMA tail after cancellation. The microphone keeps streaming cleaned audio while the bot talks, so the server can detect speech and send `barge_in` without ending the session. Microphone capture and speaker playback share I2S clocks through the GPIO matrix; **the existing wiring stays the same**. The fixed startup/FIFO offset and real acoustic performance still require verification on the finished speaker enclosure.

The AEC instance stays allocated and processes continuously across turns. A lost capture/reference block resets partial packetization and suppresses output for four AEC frames while valid history resumes; it does not repeatedly recreate the DSP. AEC's 512-sample output is repacked into complete 320-sample network packets, preserving the remainder.

Without usable PSRAM or sufficient memory reserve, initialization explicitly falls back to half duplex and sends silence during bot replies. Set `VOICEBOT_AEC_ENABLED 0` to disable AEC deliberately. Only in that mode does `VOICEBOT_FULL_DUPLEX 1` enable raw full duplex, which requires external echo control or acoustic isolation.

## Animated face

The face is a white screen with two black eyes and a pink mouth, driven by what the firmware already knows about the call:

| Expression | When |
| --- | --- |
| Lids opening once | First 700 ms after the display starts |
| Eyes scanning side to side | Wi-Fi, TLS or the Voicebot session is not usable yet |
| Slow gaze drift, blinks, an occasional wink | No call requested |
| Wide eyes lifting with your voice | Call open, microphone streaming |
| Eyes looking up, small flat mouth | A final transcript arrived and no reply audio has started |
| Mouth opening and closing with the reply | Reply audio is playing |
| Squinted eyes and a frown | An audio/pipeline fault after the display starts |

The mouth follows **the actual completed speaker DMA audio**, including silence and the tail of cancelled playback. Both envelopes use the capture timeline, so a delayed task does not speed up the animation by processing several queued blocks at once. The listening reaction uses raw pre-AEC capture, so the face responds to the room even when the cleaned upload stream is near silence.

Rendering runs in its own priority-1 task, below the priority-2 audio tasks, on a 33 ms schedule. It rasterizes one row at a time into a small buffer and pushes only changed eye or mouth rectangles; quantized-identical mouth shapes do not redraw. Rendering yields even when a frame misses its deadline. The face task never touches the socket, audio queues or heap. Invalid pins, a failed SPI start or insufficient memory disable the display while the voicebot continues.

Display bring-up happens **after audio/AEC and Wi-Fi initialization**. Its SPI state and 4 KiB task stack require heap, so the firmware checks the audio/TLS reserve before allocating them and again before allowing the render task to run. A startup audio failure is reported over serial before a display is started. With the documented backlight connected to 3.3 V, the panel stays lit even when initialization is skipped; GPIO-controlled backlights instead stay off until a complete face is drawn. Every 30 seconds `[FACE]` reports the current mood, both envelope levels and the render task's minimum unused stack.

The driver initializes at **1 MHz** and writes pixels at **10 MHz** by default. The ILI9341 specifies a minimum 100 ns serial write-clock period, equivalent to 10 MHz; 40 MHz exceeds that published timing. [ILI9341 datasheet, section 18.3.4](https://www.displayfuture.com/Display/datasheet/controller/ILI9341.pdf#page=238).

The face uses **320×240 landscape** (`VOICEBOT_DISPLAY_ROTATION 1`); use `3` for the opposite landscape direction. A portrait override (`0` or `2`) now produces a compile-time message instead of leaving the face disabled. The driver reasserts the selected rotation at at most 1 MHz before every drawing window, including the initial full-screen clear, so a missed startup rotation command does not remain the only orientation write.

If upgrading, update the display/button overrides in `config.local.h` as shown in [hardware_pinout.md](hardware_pinout.md#6-updating-an-existing-local-configuration), including **`VOICEBOT_BUTTON_PIN 46`**, **`VOICEBOT_DISPLAY_ROTATION 1`** and **`VOICEBOT_DISPLAY_SPI_HZ 10000000`**. Existing local definitions take precedence over updated defaults. Microphone SCK and status LED come from `hardware_pins.h` and are now GPIO48 and GPIO13. Expect `[PINS] INMP441 SCK=48 WS=2 SD=1; BUTTON=46; STATUS_LED=13.` and `[FACE] ILI9341 320x240 at 10MHz; ...; landscape rotation=1.`; a conflict log includes the exact GPIO.

This is a write-only SPI connection: a successful `[FACE] ILI9341 ...` log confirms initialization was sent, but cannot detect an unplugged panel. A photo of snowy pixels alone cannot distinguish timing, wiring, reset, supply or controller problems. The extended register setup follows [Adafruit's ILI9341 driver](https://github.com/adafruit/Adafruit_ILI9341/blob/master/Adafruit_ILI9341.cpp). The firmware also performs a software reset even when a reset GPIO is configured. These changes need confirmation on the physical panel.

For a blank or noisy screen, run `python3 ESP-Voicebot/scripts/display_check.py --frequency 1000000` from the repository root. It builds an isolated 1 MHz pattern sketch without audio, Wi-Fi or credentials and prints its path; upload it using the [display-test instructions](tests/display_target/README.md). Check that test before returning to the integrated voicebot.

Application keepalives run every 20 seconds. A genuine network/upstream failure retries while Start remains active, with delays increasing from 1 to 30 seconds and small jitter. Recovery is explicitly logged as a **new server session**; the API does not document restoring conversation context after transport loss. Stop cancels recovery. Server completion, authorization rejection and invalid protocol messages end the call. A 60-second lack of incoming progress detects a stalled connection; deliberate speaker backpressure and ongoing inbound audio do not trigger that deadline.

`tls_roots.h` contains the genuine GTS Root R4 certificate. The current endpoint's chain was checked against that root on 2026-09-17. If you change the host or its CA changes, update the trust anchor; certificate validation is never bypassed. [Google Trust Services repository](https://pki.goog/repository/).

## RAM design

Audio memory is fixed at startup. Incoming binary messages are consumed in chunks of at most **640 bytes**; their full payload is never allocated. When the speaker queue fills, reception pauses until playback frees space, letting TCP apply backpressure. The receive path accepts binary messages up to 16 MiB without allocating by message size.

| Allocation | With PSRAM | Without PSRAM |
| --- | ---: | ---: |
| Recent microphone packets, 4 slots | 2,608 B internal | 2,608 B internal |
| Speaker queue, 800 or 24 slots | 521,600 B PSRAM | 15,648 B internal |
| Capture/AEC + playback task stacks | 12,288 B internal | 12,288 B internal |
| Duplex TX/RX DMA, 3 × 160 frames each | 7,680 B internal | 7,680 B internal |
| Native AEC aligned input/reference/output | 3,072 B internal | Not allocated |
| WebSocket text storage | 8,193 B static | 8,193 B static |
| JSON arena | 12,288 B static | 12,288 B static |
| Panel row and byte-swap buffers | 896 B static | 896 B static |
| Face render task stack | 4,096 B internal | 4,096 B internal |

Queue slots include metadata and alignment. At full 20 ms chunks, speaker capacity is about 16 seconds with PSRAM or 480 ms without it; short network reads can reduce that duration. If the large PSRAM allocation fails, the firmware tries **only the small internal queue**, never a half-megabyte internal allocation. The duplex history/staging and capture packetizer use additional fixed storage; native DSP workspace, Wi-Fi, TLS and driver/control objects are also additional to this table. `[AEC]` reports the measured native allocation delta at startup. Task stacks and DMA still need internal memory even when PSRAM is present. [Espressif external-RAM guidance](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/external-ram.html).

All socket reads/writes run in `loop()`. Audio tasks never call TLS. A fixed recent-audio queue accommodates DSP output bursts and evicts its oldest packet under congestion; the consumer also rejects microphone packets older than 60 ms before upload. Network congestion does not skip DSP/reference processing or reset the session. Initialization checks every queue, I2S device and task before enabling networking. A requested connection is deferred unless at least 64 KiB internal heap and a 32 KiB contiguous internal block remain; this is a guard, not a guarantee about TLS's peak demand.

Before calling the native AEC constructor, the firmware requires at least 512 KiB free PSRAM with a 256 KiB contiguous block, plus 128 KiB free internal memory with a 64 KiB block. The pinned native library has unchecked inner allocations and some PSRAM-to-internal fallbacks, so testing only its return value is insufficient. Initialization runs before Wi-Fi and other allocation tasks. The frame-processing path allocates nothing.

Every 30 seconds `[RAM]` reports internal free/minimum/largest-block bytes, PSRAM free bytes and outstanding software/staged speaker audio. `[STACK]` reports the smallest unused stack space observed for capture, playback and loop. `[AEC]` reports processed frames, maximum DSP time, reference/capture drops and synchronization recovery. One native AEC frame represents 32 ms; sustained processing time near or above that requires investigation on the board. Arduino's static-memory build report does **not** include runtime queues, task stacks, DMA, DSP workspace or TLS.

## Validation

With Arduino CLI installed:

```sh
python3 ESP-Voicebot/scripts/compile.py --install-deps
python3 ESP-Voicebot/scripts/test_host.py --arduinojson /path/to/ArduinoJson/src
```

Run commands from the repository root. `compile.py` pins dependencies, stages **dummy credentials**, builds both OPI-PSRAM and PSRAM-disabled variants, and saves logs, firmware and `summary.json` under ignored `ESP-Voicebot/build/`. Those dummy-credential binaries are compile artifacts; build your configured sketch in Arduino IDE for actual use. To reuse a CLI toolchain, pass `--arduino-cli` and `--config-file`.

An existing pinned installation can be reused without another core download:

```sh
python3 ESP-Voicebot/scripts/compile.py --arduino-cli /path/to/arduino-cli --config-file /path/to/arduino-cli.yaml
python3 ESP-Voicebot/scripts/compile.py --arduino-cli /path/to/arduino-cli --config-file /path/to/arduino-cli.yaml --display off --target opi
python3 ESP-Voicebot/scripts/test_host.py --arduinojson /path/to/ArduinoJson/src
```

`--display default|on|off` selects display coverage independently of `--aec`. The host runner requires ArduinoJson for the client suite and runs all suites; an unchanged client still needs validation alongside new integration code.

Host suites exercise the actual framing, capture/speaker packetizers, duplex callback timeline, playback/session helpers, real socket congestion, the mouth envelope, the face animation and rasterizer, and client logic with fake transport and real ArduinoJson, under AddressSanitizer and UndefinedBehaviorSanitizer. Wrapper doubles test AEC admission/cleanup policy; they do not establish echo-cancellation quality. The panel suite runs the real ILI9341 glue against recording Arduino/SPI doubles, checking the command stream, the pixel byte order and that only changed regions reach the bus; it cannot show that a physical panel lights up or that the face looks right on it. See [TROUBLESHOOTING_AND_FIXES.md](TROUBLESHOOTING_AND_FIXES.md) for measured results and hardware checks.

`scripts/test_aec_target.py` compiles a standalone fixture against the production AEC wrapper and actual ESP-SR binary. Use `--build-only` to prepare it for a board, or `--qemu /path/to/qemu-system-xtensa` with pinned **Espressif QEMU `esp_develop_9.2.2_20260417`** to execute synthetic PCM tests. `--psram none` verifies the no-PSRAM admission path. It uses no credentials or network. Emulator timing is not an ESP32-S3 performance benchmark, and QEMU does not test I2S alignment or acoustic barge-in. [Espressif QEMU instructions](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md).

On-device audio, the panel itself, Wi-Fi/TLS peak memory and a long-running soak test still require a connected board. Panel rotation, colour order and the real cost of SPI writes alongside the audio tasks are unverified until then; if the face appears upside down, change `VOICEBOT_DISPLAY_ROTATION` to 3. Backpressure preserves bounded RAM; a server may still end a session on a service or network error. A barge-in event behind already queued TCP audio cannot be processed until those preceding bytes are read. On receipt, queued playback is cancelled and subsequent audio waits in TCP until the in-flight frame/DMA guard and required playback notifications finish. No timed window of new TTS is discarded. Normal completion waits until the final software frame has entered I2S plus a conservative 120 ms DMA/acoustic drain guard; a post-audio bot-response event provides an end hint, with a 600 ms receive-idle fallback because the protocol has no explicit TTS audio-end marker. Any further audio invalidates the previous hint. In half-duplex fallback, listening resumes after completion is sent; AEC mode keeps listening throughout playback.
