# ESP-Voicebot: Deep Inspection, Technical Issues & Fixes Log

## 1. Overview
This document provides a comprehensive technical breakdown of all issues encountered, root-cause analyses, and implemented resolutions during the development and testing of the **Standalone ESP32-S3 Botnoi Voicebot Client (`ESP-Voicebot`)**.

The objective of `ESP-Voicebot` is to establish a direct, real-time, bi-directional audio conversation between an ESP32-S3 hardware module (INMP441 microphone + MAX98357A I2S speaker) and the **Botnoi Voicebot API** over WebSockets TLS (`wss://voicebot-stg.botnoigroup.com:443/v1/preview_call?api_key=...&agent_id=...`) without requiring any PC host or Python intermediary bridge.

---

## 2. Hardware & Architecture Specifications

- **Microcontroller**: ESP32-S3-WROOM-1 (16MB QSPI/OPI Flash, 8MB PSRAM).
- **I2S Microphone (INMP441)**:
  - BCLK: `GPIO 3`
  - WS (LRCL): `GPIO 2`
  - SD (Data Out): `GPIO 1`
  - Configuration: `I2S_MODE_STD`, `16000Hz`, `I2S_DATA_BIT_WIDTH_32BIT` (24-bit left-aligned in 32-bit slot), `I2S_SLOT_MODE_MONO`, `I2S_STD_SLOT_LEFT`.
- **I2S DAC / Amplifier (MAX98357A)**:
  - BCLK: `GPIO 38`
  - LRC (WS): `GPIO 39`
  - DIN (Data In): `GPIO 40`
  - Configuration: `I2S_MODE_STD`, `16000Hz`, `I2S_DATA_BIT_WIDTH_16BIT`, `I2S_SLOT_MODE_STEREO`, `I2S_STD_SLOT_BOTH`.
- **Control & Display**:
  - Talk Button: `GPIO 46` (Internal `INPUT_PULLUP`, toggle recording mode).
  - Status LED: `GPIO 48` (HIGH when recording).

---

## 3. Issues Encountered & Applied Resolutions

### Issue 1: System Freeze / Deadlock at `Connecting to wss://...`
#### Symptom
When the board booted, Wi-Fi connected, and time was synchronized via NTP, the Serial output stopped at:
```text
[Voicebot] Connecting to Botnoi Voicebot WebSocket...
[Voicebot] Connecting to wss://voicebot-stg.botnoigroup.com:443/v1/preview_call?api_key=...&agent_id=...
```
The board froze completely and did not respond to button presses or process incoming WebSocket packets.

#### Root Cause Analysis
FreeRTOS task priority starvation. 
`captureTask` and `playbackTask` were originally created using `xTaskCreatePinnedToCore` with **Priority 5** pinned to **Core 1**. 
`captureTask` executed a continuous loop calling `i2s_channel_read()` with a 30ms timeout. Because it ran at Priority 5 without explicit cooperative yielding (`vTaskDelay`), it completely starved lower-priority tasks running on Core 1, including the main `loop()` (Priority 1), which handles the Arduino framework background tasks, Wi-Fi TCP/IP stack events, and SSL/TLS handshakes (`voicebot.loop()`).

#### Applied Fix
1. Changed `captureTask` and `playbackTask` creation from `xTaskCreatePinnedToCore` to standard `xTaskCreate` (unpinned), allowing the FreeRTOS scheduler to dynamically execute them across available cores.
2. Reduced task priority from **Priority 5** to **Priority 2** (just above idle).
3. Added an explicit `vTaskDelay(pdMS_TO_TICKS(1))` at the end of each `captureTask` iteration to guarantee CPU slot availability for background network processing.

---

### Issue 2: Immediate Premature Disconnection (`Reason: TCP connection cleanup`)
#### Symptom
During connection attempts, the Serial log repeatedly outputted disconnections every 5 seconds:
```text
21:10:41.733 -> [Voicebot] ⚠️ WebSocket Disconnected (Reason: TCP connection cleanup)
21:10:46.829 -> [Voicebot] ⚠️ WebSocket Disconnected (Reason: TCP connection cleanup)
```

#### Root Cause Analysis
In `src/cloud_websockets/WebSocketsClient.cpp`, the internal method `clientIsConnected()` contained logic that prematurely invoked `clientDisconnect(client, "TCP connection cleanup")` if the TCP socket state returned unready while an asynchronous TLS handshake was still in progress. This destroyed the active socket before `WiFiClientSecure` could complete the TLS exchange with Cloudflare/Botnoi servers.

#### Applied Fix
Removed the aggressive `clientDisconnect(client, "TCP connection cleanup")` trigger inside `clientIsConnected()`, allowing `WiFiClientSecure` sufficient time to negotiate the SSL certificate and establish the WebSocket frame state.

---

### Issue 3: Uninitialized `_CA_bundle` Pointer & SSL Handshake Crash
#### Symptom
Intermittent ESP32 kernel panic / illegal memory access crash when initiating secure WebSockets via `beginSSL()`.

#### Root Cause Analysis
In ESP32 Arduino Core v3.x, `WebSocketsClient.cpp` maintained an internal `_CA_bundle` pointer. In certain code paths of `beginSSL()`, `_CA_bundle` was left uninitialized (garbage memory address), causing `WiFiClientSecure` to dereference invalid memory when attempting CA bundle validation.

Furthermore, calling `beginSSL(host, port, url, NULL, "")` passed `NULL` as `const uint8_t*`, causing compiler permissive warnings/errors or invalid pointer conversions.

#### Applied Fix
1. Initialized `_CA_bundle = NULL;` explicitly in `WebSocketsClient::begin()` and `beginSSL()`.
2. Created a dedicated helper in `voicebot_client.h`:
   ```cpp
   if (ca && strlen(ca) > 50 && strstr(ca, "-----BEGIN CERTIFICATE-----")) {
     beginSslWithCA(host, port, urlPath.c_str(), ca, "");
   } else {
     beginSSL(host, port, urlPath.c_str());
   }
   ```
   When no valid PEM certificate is supplied, `beginSSL()` relies on `setInsecure()` mode within `WiFiClientSecure` to cleanly establish TLS without memory faults.

---

### Issue 4: Memory Exhaustion (SRAM) & Audio Buffer Overflow on Initial Greeting
#### Symptom
When connecting to Botnoi Voicebot API, the server immediately sends a session `opened` event followed by a large initial welcome greeting TTS audio payload (~64KB to 400KB of raw 16kHz PCM audio). This resulted in heap allocation failures or WebSocket buffer truncation.

#### Root Cause Analysis
1. The default `WEBSOCKETS_MAX_DATA_SIZE` in `WebSockets.h` was set to `64KB` (`64 * 1024`), causing payloads larger than 64KB to be rejected or dropped.
2. Large dynamic payload buffers were being allocated in internal DRAM (`malloc()`), quickly exhausting the limited ~300KB internal SRAM of the ESP32-S3.

#### Applied Fix
1. Expanded `WEBSOCKETS_MAX_DATA_SIZE` in `src/cloud_websockets/WebSockets.h` to **256KB** (`256 * 1024`).
2. Modified buffer allocations in `WebSockets.cpp` to explicitly request PSRAM (SPIRAM):
   ```cpp
   _payload = (uint8_t *) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!_payload) {
     _payload = (uint8_t *) malloc(size); // DRAM Fallback
   }
   ```
3. Configured `micQueue` and `spkQueue` in `ESP-Voicebot.ino` to be allocated in PSRAM using `heap_caps_malloc(count * sizeof(AudioFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.

---

### Issue 5: INMP441 Microphone 32-bit Sample Scaling & Audio Distortion
#### Symptom
Recorded user audio sent over WebSocket was unreadable or interpreted as loud static noise by Botnoi Voicebot server.

#### Root Cause Analysis
The INMP441 MEMS microphone outputs 24-bit audio samples left-aligned inside a 32-bit I2S slot (`int32_t`). Botnoi Voicebot WebSocket API expects **16kHz 16-bit Mono Signed Little-Endian PCM** binary frames. Sending raw 32-bit buffer bytes corrupted the audio waveform.

#### Applied Fix
In `ESP-Voicebot.ino` `captureTask`, converted the 32-bit raw I2S samples into 16-bit signed PCM samples with clipping protection before pushing to the WebSocket queue:
```cpp
const size_t samples = min(received / sizeof(int32_t), FRAME_BYTES / 2);
for (size_t i = 0; i < samples; ++i) {
  const int32_t sample = raw[i] >> 16;
  pcm[i] = static_cast<int16_t>(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
}
```

---

### Issue 6: Botnoi Server VAD Not Triggering Upon Releasing Button
#### Symptom
After the user held GPIO46 button, spoke a command, and released the button, the Botnoi server remained silent and did not process the user's turn.

#### Root Cause Analysis
Botnoi Voicebot API uses server-side Voice Activity Detection (VAD). If binary PCM transmission stops abruptly when the button is released, the server's VAD buffer remains waiting for trailing silence to confirm the end of speech.

#### Applied Fix
Added `sendSilencePadding(25)` inside `VoicebotClient`. When recording toggles `OFF` (button release), the ESP32 sends 25 frames (500ms total) of zeroed PCM binary frames (`0x00`), signaling the end of speech to Botnoi's server-side VAD and instantly triggering the bot's response generation.

---

## 4. Verification & Testing Results

1. **Standalone Inspection (`scratch/deep_inspect_voicebot.py`)**:
   - Verified Botnoi Voicebot protocol:
     - URL: `wss://voicebot-stg.botnoigroup.com:443/v1/preview_call?api_key=...&agent_id=...`
     - Connection message: `{"type": "opened", "session_id": "..."}`
     - Welcome message: `{"type": "bot_turn_response", "text": "สวัสดีครับ..."}`
     - Received ~437KB binary payload (13.68 seconds of 16kHz PCM audio).
2. **ESP32 Firmware Compile & Execution**:
   - Firmware compiles cleanly under Arduino IDE / ESP32 Core 3.x.
   - Wi-Fi connection and NTP time sync execute reliably.
   - Core tasks (`captureTask`, `playbackTask`) execute seamlessly alongside `voicebot.loop()` without starving Core 1.
