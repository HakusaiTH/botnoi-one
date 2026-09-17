// Standalone ESP32-S3, INMP441 microphone and MAX98357A speaker.
// Only loop() owns WebSocket/TLS. Audio tasks use fixed queues.
#include <Arduino.h>
#include <WiFi.h>
#include <ESP_I2S.h>
#include <atomic>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_system.h>
#if __has_include("config.local.h")
#include "config.local.h"
#endif
#include "config.example.h"
#include "audio_pipeline.h"
#include "microphone_flow.h"
#include "voicebot_client.h"
#include "tls_roots.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Select ESP32-S3 Dev Module. Other ESP32 variants need a reviewed pin map."
#endif

constexpr int BUTTON_SESSION = VOICEBOT_BUTTON_PIN;
constexpr int MIC_SCK = 3, MIC_WS = 2, MIC_SD = 1;
constexpr int SPK_BCLK = 38, SPK_LRC = 39, SPK_DIN = 40;
constexpr int LED_PIN = 48;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t TTS_END_IDLE_MS = 600;
constexpr size_t FRAME_BYTES = voicebot_audio::kFrameBytes;
constexpr size_t MIC_QUEUE_FRAMES = voicebot_audio::kMicrophoneQueueFrames;
constexpr size_t SPK_PSRAM_FRAMES = 800, SPK_INTERNAL_FRAMES = 24;
constexpr size_t SESSION_COMMAND_QUEUE_DEPTH = 8;
constexpr uint32_t CAPTURE_STACK_BYTES = 4096, PLAYBACK_STACK_BYTES = 4096;
constexpr uint32_t INTERNAL_RESERVE = 64 * 1024, TLS_LARGEST_BLOCK = 32 * 1024;
using voicebot_audio::AudioFrame;

I2SClass microphone, speaker;
QueueHandle_t micQueue = nullptr, spkQueue = nullptr, sessionCommandQueue = nullptr;
StaticQueue_t micQueueState, spkQueueState, sessionCommandQueueState;
uint8_t *micStorage = nullptr, *spkStorage = nullptr;
uint32_t sessionCommandStorage[SESSION_COMMAND_QUEUE_DEPTH]{};
TaskHandle_t captureHandle = nullptr, playbackHandle = nullptr;
std::atomic<bool> sessionActive{false};
std::atomic<bool> sessionRequested{false};
std::atomic<bool> micSuppressed{false};
enum : uint32_t { SESSION_COMMAND_NONE, SESSION_COMMAND_START, SESSION_COMMAND_STOP };
std::atomic<uint32_t> micFullDrops{0};
std::atomic<uint32_t> recordingEpoch{0};
std::atomic<uint32_t> sessionEpoch{1}, playbackGeneration{1};
std::atomic<uint32_t> pendingSpeakerFrames{0}, playbackStartedGeneration{0};
std::atomic<uint32_t> playbackDrainedAt{0};
std::atomic<uint32_t> audioFault{0};
VoicebotClient voicebot;
voicebot_audio::PcmAssembler ttsAssembler;
voicebot_audio::ReplyGate replyGate;
voicebot_audio::MicrophonePacer micPacer;
bool ready = false, playbackAnnounced = false, playbackCompletionPending = false, micPauseAnnounced = false;
bool ttsBurstActive = false, ttsEndHint = false;
uint32_t lastTtsAt = 0, retryAt = 0, dropTtsUntil = 0;
uint32_t micExpiredDrops = 0, micPausedDrops = 0, maxSocketMs = 0, maxMicSendMs = 0;

QueueHandle_t makeAudioQueue(size_t count, uint32_t caps, StaticQueue_t* state, uint8_t** storage) {
  *storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame), caps));
  if (!*storage) return nullptr;
  QueueHandle_t queue = xQueueCreateStatic(count, sizeof(AudioFrame), *storage, state);
  if (!queue) { heap_caps_free(*storage); *storage = nullptr; }
  return queue;
}

void reportMemory() {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Serial.printf("[RAM] internal free=%u min=%u largest=%u; PSRAM free=%u; speaker queued=%u\n",
      unsigned(heap_caps_get_free_size(caps)), unsigned(heap_caps_get_minimum_free_size(caps)),
      unsigned(heap_caps_get_largest_free_block(caps)),
      unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), unsigned(pendingSpeakerFrames.load()));
  Serial.printf("[FLOW] mic queued=%u replaced/expired=%u/%u paused=%s; max socket=%ums send=%ums\n",
      unsigned(micQueue ? uxQueueMessagesWaiting(micQueue) : 0), unsigned(micFullDrops.load()),
      unsigned(micExpiredDrops), micSuppressed.load() ? "yes" : "no",
      unsigned(maxSocketMs), unsigned(maxMicSendMs));
  if (captureHandle && playbackHandle) {
    Serial.printf("[STACK] minimum unused bytes: capture=%u playback=%u loop=%u\n",
        unsigned(uxTaskGetStackHighWaterMark(captureHandle)),
        unsigned(uxTaskGetStackHighWaterMark(playbackHandle)),
        unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
}

void clearPlayback() {
  playbackGeneration.fetch_add(1);
  ttsAssembler.reset();
  AudioFrame discarded;
  while (spkQueue && xQueueReceive(spkQueue, &discarded, 0) == pdTRUE) {
    if (pendingSpeakerFrames.fetch_sub(1) == 1) playbackDrainedAt.store(millis());
  }
  if (playbackAnnounced && !pendingSpeakerFrames.load()) playbackDrainedAt.store(millis());
  playbackAnnounced = false;
  playbackStartedGeneration.store(0);
  ttsBurstActive = false;
  ttsEndHint = false;
}

void resetSession() {
  sessionActive.store(false);
  sessionEpoch.fetch_add(1);
  recordingEpoch.store(0);
  digitalWrite(LED_PIN, LOW);
  micPacer.reset();
  replyGate.reset();
  micSuppressed.store(false);
  micPauseAnnounced = false;
  playbackCompletionPending = false;
  dropTtsUntil = 0;
  if (micQueue) xQueueReset(micQueue);
  clearPlayback();
  playbackDrainedAt.store(0);
}

void captureTask(void*) {
  int stable = digitalRead(BUTTON_SESSION), previous = stable;
  uint32_t changed = millis(), epoch = sessionEpoch.load();
  bool pressedToStart = !sessionRequested.load();
  int32_t raw[FRAME_BYTES / 2];
  AudioFrame frame{};
  for (;;) {
    const uint32_t readEpoch = sessionEpoch.load();
    const bool recordThisRead = recordingEpoch.load() == readEpoch && sessionActive.load() && !micSuppressed.load();
    size_t received = 0;
    const esp_err_t status = i2s_channel_read(microphone.rxChan(), raw, sizeof(raw), &received, 30);
    const uint32_t now = millis();
    if (epoch != sessionEpoch.load()) {
      epoch = sessionEpoch.load();
    }
    const int reading = digitalRead(BUTTON_SESSION);
    if (reading != previous) {
      changed = now;
      // Snapshot the intended action on the physical edge. A transport failure
      // during the debounce window must not reinterpret Hang up as Start.
      if (reading == LOW) pressedToStart = !sessionRequested.load();
    }
    previous = reading;
    if (reading != stable && now - changed >= 30) {
      stable = reading;
      if (stable == LOW) {
        sessionRequested.store(pressedToStart);
        if (!pressedToStart) {
          // Stop capture immediately; loop() owns the protocol close handshake.
          recordingEpoch.store(0);
          micSuppressed.store(true);
          digitalWrite(LED_PIN, LOW);
        }
        // Preserve physical ordering even if loop() is briefly blocked in TLS.
        const uint32_t command = pressedToStart ? SESSION_COMMAND_START : SESSION_COMMAND_STOP;
        if (xQueueSend(sessionCommandQueue, &command, 0) != pdTRUE) audioFault.store(6);
      }
    }
    // Keep the hangup control responsive even if microphone I2S is failing.
    if (status != ESP_OK && status != ESP_ERR_TIMEOUT) {
      audioFault.store(2);
      recordingEpoch.store(0);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (recordThisRead && sessionActive.load() && !micSuppressed.load() && readEpoch == epoch && received) {
      const size_t samples = received / sizeof(int32_t);
      frame.generation = epoch;
      frame.capturedAt = now;
      frame.length = samples * sizeof(int16_t);
      for (size_t i = 0; i < samples; ++i) {
        const int16_t sample = static_cast<int16_t>(raw[i] >> 16);
        frame.pcm[2 * i] = static_cast<uint8_t>(sample);
        frame.pcm[2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8);
      }
      const bool replaced = uxQueueSpacesAvailable(micQueue) == 0;
      if (frame.length && !voicebot_audio::overwriteMicrophone(frame,
          [](const AudioFrame& packet) { return xQueueOverwrite(micQueue, &packet) == pdPASS; })) {
        audioFault.store(5);
      } else if (replaced) micFullDrops.fetch_add(1);
    }
    vTaskDelay(1);
  }
}

void playbackTask(void*) {
  AudioFrame frame{};
  int16_t stereo[FRAME_BYTES];
  for (;;) {
    if (xQueueReceive(spkQueue, &frame, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (frame.generation == playbackGeneration.load()) {
      for (size_t i = 0; i < frame.length / 2; ++i) {
        const int16_t sample = static_cast<int16_t>(frame.pcm[2 * i] | (uint16_t(frame.pcm[2 * i + 1]) << 8));
        stereo[2 * i] = stereo[2 * i + 1] = static_cast<int16_t>(int32_t(sample) * 70 / 100);
      }
      size_t offset = 0;
      const size_t length = frame.length * 2;
      uint32_t lastProgress = millis();
      while (offset < length && frame.generation == playbackGeneration.load()) {
        size_t written = 0;
        const esp_err_t status = i2s_channel_write(speaker.txChan(),
            reinterpret_cast<uint8_t*>(stereo) + offset, length - offset, &written, 20);
        offset += written;
        if (written) {
          // Announce playback only after I2S accepted real samples, not when a
          // frame was merely removed from the software queue.
          playbackStartedGeneration.store(frame.generation);
          lastProgress = millis();
        }
        if ((status != ESP_OK && status != ESP_ERR_TIMEOUT) || millis() - lastProgress > 200) {
          audioFault.store(3);
          break;
        }
        if (!written) vTaskDelay(1);
      }
    }
    if (pendingSpeakerFrames.fetch_sub(1) == 1) playbackDrainedAt.store(millis());
  }
}

void releaseAudio() {
  // Only used before ready=true; loop() cannot touch partial initialization.
  if (captureHandle) { vTaskDelete(captureHandle); captureHandle = nullptr; }
  if (playbackHandle) { vTaskDelete(playbackHandle); playbackHandle = nullptr; }
  microphone.end();
  speaker.end();
  if (micQueue) { vQueueDelete(micQueue); micQueue = nullptr; }
  if (spkQueue) { vQueueDelete(spkQueue); spkQueue = nullptr; }
  if (sessionCommandQueue) { vQueueDelete(sessionCommandQueue); sessionCommandQueue = nullptr; }
  heap_caps_free(micStorage); micStorage = nullptr;
  heap_caps_free(spkStorage); spkStorage = nullptr;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP-Voicebot: bounded streaming audio for ESP32-S3");
  Serial.printf("[BOOT] reset reason=%d (1=power-on, 3=software, 4=panic, 5/6/7=watchdog, 9=brownout)\n",
      int(esp_reset_reason()));
#ifdef BOARD_HAS_PSRAM
  constexpr bool psramRequested = true;
#else
  constexpr bool psramRequested = false;
#endif
  Serial.printf("[PSRAM] build enabled=%s initialized=%s total=%u free=%u largest=%u\n",
      psramRequested ? "yes" : "no", esp_psram_is_initialized() ? "yes" : "no",
      unsigned(ESP.getPsramSize()), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  Serial.printf("[MIC] %s\n", VOICEBOT_FULL_DUPLEX ? "Full duplex enabled." : "Mic pauses during bot replies and resumes automatically.");
  Serial.println("[SESSION] Tap once to start a hands-free call; tap again to hang up.");
  pinMode(BUTTON_SESSION, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  sessionCommandQueue = xQueueCreateStatic(SESSION_COMMAND_QUEUE_DEPTH, sizeof(uint32_t),
      reinterpret_cast<uint8_t*>(sessionCommandStorage), &sessionCommandQueueState);
  micQueue = makeAudioQueue(MIC_QUEUE_FRAMES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &micQueueState, &micStorage);
  size_t speakerFrames = SPK_PSRAM_FRAMES;
  spkQueue = makeAudioQueue(speakerFrames, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, &spkQueueState, &spkStorage);
  if (!spkQueue) {
    speakerFrames = SPK_INTERNAL_FRAMES;
    spkQueue = makeAudioQueue(speakerFrames, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &spkQueueState, &spkStorage);
  }
  if (!sessionCommandQueue || !micQueue || !spkQueue) {
    Serial.println("[ERROR] Audio queue allocation failed; network disabled.");
    releaseAudio();
    return;
  }
  Serial.printf("[RAM] mic=%u bytes; speaker=%u bytes (%s). No full TTS allocation.\n",
      unsigned(MIC_QUEUE_FRAMES * sizeof(AudioFrame)), unsigned(speakerFrames * sizeof(AudioFrame)),
      speakerFrames == SPK_PSRAM_FRAMES ? "PSRAM" : "small internal buffer");
  microphone.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN, -1);
  if (!microphone.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT) ||
      !speaker.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[ERROR] I2S initialization failed; network disabled.");
    releaseAudio();
    return;
  }
  if (xTaskCreate(captureTask, "capture", CAPTURE_STACK_BYTES, nullptr, 2, &captureHandle) != pdPASS ||
      xTaskCreate(playbackTask, "playback", PLAYBACK_STACK_BYTES, nullptr, 2, &playbackHandle) != pdPASS) {
    Serial.println("[ERROR] Audio task allocation failed; network disabled.");
    releaseAudio();
    return;
  }
  voicebot.setTtsAudioCapacityCallback([]() -> size_t {
    const uint32_t now = millis();
    if (playbackCompletionPending ||
        (dropTtsUntil && static_cast<int32_t>(now - dropTtsUntil) < 0)) return FRAME_BYTES;
    return audioFault.load() ? 0 : ttsAssembler.capacity(uxQueueSpacesAvailable(spkQueue));
  });
  voicebot.setTtsAudioCallback([](const uint8_t* pcm, size_t length) {
    const uint32_t now = millis();
    // Do not start a new audible burst before the cancelled burst's
    // playback_completed has reached the server.
    if (playbackCompletionPending) return;
    if (dropTtsUntil && static_cast<int32_t>(now - dropTtsUntil) < 0) return;
    dropTtsUntil = 0;
    lastTtsAt = now;
    if (!ttsBurstActive) {
      ttsBurstActive = true;
      ttsEndHint = false;
    }
    playbackDrainedAt.store(0);
    replyGate.onAudio(lastTtsAt);
    if (!VOICEBOT_FULL_DUPLEX) micSuppressed.store(true);
    const bool ok = ttsAssembler.append(pcm, length, playbackGeneration.load(), [](const AudioFrame& frame) {
      pendingSpeakerFrames.fetch_add(1);
      if (xQueueSend(spkQueue, &frame, 0) == pdTRUE) return true;
      pendingSpeakerFrames.fetch_sub(1);
      return false;
    });
    if (!ok) audioFault.store(4);
  });
  voicebot.setBargeInCallback([]() {
    if (playbackAnnounced) playbackCompletionPending = true;
    clearPlayback();
    dropTtsUntil = millis() + 2000;
    replyGate.reset();
    Serial.println("[BARGE_IN] Playback cancelled; late TTS will be discarded.");
  });
  voicebot.setUserSttCallback([](const String&, bool isFinal) {
    if (isFinal) dropTtsUntil = 0;
  });
  voicebot.setBotReplyCallback([](const String&) {
    if (ttsBurstActive) ttsEndHint = true;
  });
  voicebot.setOpenedCallback([](const String&) {
    if (!sessionRequested.load()) {
      voicebot.requestClose();
      return;
    }
    resetSession();
    sessionActive.store(true);
    recordingEpoch.store(sessionEpoch.load());
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[SESSION] Ready; microphone streams continuously until hangup.");
    reportMemory();
  });
  voicebot.setDisconnectCallback([](const String&) {
    // A lost transport cannot resume the same Preview Call. Reconnecting here
    // would silently create a new session with new conversation context.
    sessionRequested.store(false);
    resetSession();
    Serial.println("[SESSION] Call ended by the server or network; tap to start a new session.");
  });
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  ready = true;
  Serial.println("[SYSTEM] Ready. Tap the button to open a Voicebot session.");
  reportMemory();
}

void abortConnection(const char* reason) {
  Serial.printf("[SESSION] %s; call ended. Tap to start a new session.\n", reason);
  sessionRequested.store(false);
  voicebot.stop();
  resetSession();
}

void serviceMicrophone() {
  if (!voicebot.isOpened()) return;
  const bool pause = !VOICEBOT_FULL_DUPLEX &&
      (playbackAnnounced || playbackCompletionPending ||
       replyGate.paused(millis(), voicebot.isReceivingAudio(), pendingSpeakerFrames.load() != 0));
  micSuppressed.store(pause);
  if (pause != micPauseAnnounced) {
    micPauseAnnounced = pause;
    if (recordingEpoch.load() == sessionEpoch.load()) {
      Serial.println(pause ? "[MIC] Paused during bot reply; recording stays ON."
                           : "[MIC] Listening resumed.");
    }
  }
  AudioFrame frame;
  if (pause) {
    // Without AEC, do not upload loudspeaker echo. Recording intent remains ON
    // and capture resumes automatically after playback_completed.
    for (size_t i = 0; i < MIC_QUEUE_FRAMES && xQueueReceive(micQueue, &frame, 0) == pdTRUE; ++i) {
      if (frame.generation == sessionEpoch.load() && frame.length) ++micPausedDrops;
    }
    micPacer.reset();
    return;
  }
  // A congested socket is not a session error. Capture atomically overwrites
  // this one-slot mailbox, so the next send is always the latest PCM frame.
  const uint32_t now = millis();
  if (!micPacer.due(now) || !voicebot.canSendNow()) return;
  if (xQueueReceive(micQueue, &frame, 0) != pdTRUE) return;
  if (frame.generation != sessionEpoch.load() ||
      voicebot_audio::microphoneFrameExpired(now, frame.capturedAt)) {
    ++micExpiredDrops;
    return;
  }
  const uint32_t sendAt = millis();
  if (!voicebot.sendAudioFrame(frame.pcm, frame.length)) {
    abortConnection("Microphone send failed");
    return;
  }
  maxMicSendMs = max(maxMicSendMs, uint32_t(millis() - sendAt));
  micPacer.sent(sendAt, frame.length);
}

void loop() {
  if (!ready) { delay(1000); return; }
  const uint32_t now = millis();
  uint32_t command = SESSION_COMMAND_NONE;
  while (xQueueReceive(sessionCommandQueue, &command, 0) == pdTRUE) {
    const bool requested = command == SESSION_COMMAND_START;
    sessionRequested.store(requested);
    if (requested) {
      retryAt = now;
      if (voicebot.isClosing()) {
        // The old close handshake cannot be cancelled. Keep START intent and
        // open a fresh call as soon as that transport has stopped.
        resetSession();
        Serial.println("[SESSION] New call queued after the current hangup completes.");
      } else if (voicebot.isOpened()) {
        // Defensive no-op: an already applied START keeps the current context.
        sessionActive.store(true);
        recordingEpoch.store(sessionEpoch.load());
        digitalWrite(LED_PIN, HIGH);
        Serial.println("[SESSION] Call remains active; microphone listening continues.");
      } else if (voicebot.isRunning()) {
        Serial.println("[SESSION] Voicebot call is still connecting.");
      } else {
        resetSession();
        Serial.println("[SESSION] Starting one persistent Voicebot call.");
      }
    } else {
      resetSession();
      if (voicebot.isRunning()) voicebot.requestClose();
      Serial.println("[SESSION] Hangup requested; microphone stopped.");
    }
  }
  const uint32_t fault = audioFault.exchange(0);
  if (fault) {
    Serial.printf("[AUDIO] fault=%u (2=mic I2S, 3=speaker I2S, 4=TTS queue, 5=mic mailbox, 6=button queue).\n",
                  unsigned(fault));
    abortConnection("Audio pipeline stopped");
  }
  static uint32_t lastReport = 0, lastWifiRetry = 0;
  if (now - lastReport >= 30000) { lastReport = now; reportMemory(); }
  static uint32_t lastDropNotice = 0, reportedDrops = 0;
  const uint32_t dropped = micFullDrops.load() + micExpiredDrops;
  if (dropped != reportedDrops && now - lastDropNotice >= 5000) {
    lastDropNotice = now;
    reportedDrops = dropped;
    Serial.printf("[MIC] Upload backlog: dropped %u frames total; keeping session.\n", unsigned(dropped));
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (voicebot.isRunning()) abortConnection("Wi-Fi lost");
    if (now - lastWifiRetry >= 15000) { lastWifiRetry = now; WiFi.reconnect(); }
    delay(10);
    return;
  }
  // NTP failure must not block audio tasks or bypass certificate dates.
  if (time(nullptr) < 1704067200) {
    static uint32_t lastNtpNotice = 0;
    if (now - lastNtpNotice >= 15000) {
      lastNtpNotice = now;
      Serial.println("[NTP] Waiting for valid time before TLS connection.");
    }
    delay(10);
    return;
  }
  if (sessionRequested.load() && !voicebot.isRunning() && static_cast<int32_t>(now - retryAt) >= 0) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < INTERNAL_RESERVE || heap_caps_get_largest_free_block(caps) < TLS_LARGEST_BLOCK) {
      Serial.println("[RAM] Not enough internal TLS headroom; connection deferred.");
      reportMemory();
      retryAt = now + 5000;
    } else {
      if (!voicebot.start(BOTNOI_WS_HOST, BOTNOI_WS_PORT, BOTNOI_WS_PATH,
                          BOTNOI_API_KEY, BOTNOI_AGENT_ID, BOTNOI_ROOT_CA)) {
        retryAt = now + 5000;
      }
    }
  }
  if (voicebot.isRunning()) {
    const uint32_t socketAt = millis();
    voicebot.loop();
    maxSocketMs = max(maxSocketMs, uint32_t(millis() - socketAt));
    if (voicebot.isOpened()) {
      const uint32_t controlNow = millis();
      const uint32_t drainedAt = playbackDrainedAt.load();
      const bool playbackDrainGuardPassed = drainedAt &&
          static_cast<uint32_t>(controlNow - drainedAt) >= voicebot_audio::kPlaybackDrainGuardMs;
      if (playbackCompletionPending && !pendingSpeakerFrames.load() &&
          playbackDrainGuardPassed && voicebot.canSendNow()) {
        if (voicebot.sendPlaybackCompleted()) {
          playbackCompletionPending = false;
          playbackDrainedAt.store(0);
          replyGate.reset();
        } else abortConnection("Barge-in playback notification failed");
      }
      if (!playbackCompletionPending && !playbackAnnounced &&
          playbackStartedGeneration.load() == playbackGeneration.load() && voicebot.canSendNow()) {
        if (voicebot.sendPlaybackStarted()) playbackAnnounced = true;
        else abortConnection("Playback notification failed");
      }
      if (playbackAnnounced && !pendingSpeakerFrames.load() && !voicebot.isReceivingAudio() &&
          playbackDrainGuardPassed && (ttsEndHint || controlNow - lastTtsAt >= TTS_END_IDLE_MS) &&
          voicebot.canSendNow()) {
        if (!voicebot.sendPlaybackCompleted()) {
          abortConnection("Playback notification failed");
        } else {
          playbackAnnounced = false;
          playbackStartedGeneration.store(0);
          playbackDrainedAt.store(0);
          ttsBurstActive = false;
          ttsEndHint = false;
          replyGate.reset();
        }
      }
      serviceMicrophone();
    }
  }
  delay(1);
}
