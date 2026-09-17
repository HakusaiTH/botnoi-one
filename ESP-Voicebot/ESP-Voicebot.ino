// Standalone ESP32-S3, INMP441 microphone and MAX98357A speaker.
// Only loop() owns WebSocket/TLS. Audio tasks use fixed queues.
#include <Arduino.h>
#include <WiFi.h>
#include <ESP_I2S.h>
#include <atomic>
#include <time.h>
#include <esp_heap_caps.h>
#if __has_include("config.local.h")
#include "config.local.h"
#endif
#include "config.example.h"
#include "audio_pipeline.h"
#include "voicebot_client.h"
#include "tls_roots.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Select ESP32-S3 Dev Module. Other ESP32 variants need a reviewed pin map."
#endif

constexpr int BUTTON_TALK = VOICEBOT_BUTTON_PIN;
constexpr int MIC_SCK = 3, MIC_WS = 2, MIC_SD = 1;
constexpr int SPK_BCLK = 38, SPK_LRC = 39, SPK_DIN = 40;
constexpr int LED_PIN = 48;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t FRAME_BYTES = voicebot_audio::kFrameBytes;
constexpr size_t MIC_QUEUE_FRAMES = 12, SPK_PSRAM_FRAMES = 800, SPK_INTERNAL_FRAMES = 24;
constexpr uint32_t CAPTURE_STACK_BYTES = 4096, PLAYBACK_STACK_BYTES = 4096;
constexpr uint32_t INTERNAL_RESERVE = 64 * 1024, TLS_LARGEST_BLOCK = 32 * 1024;
using voicebot_audio::AudioFrame;

I2SClass microphone, speaker;
QueueHandle_t micQueue = nullptr, spkQueue = nullptr;
StaticQueue_t micQueueState, spkQueueState;
uint8_t *micStorage = nullptr, *spkStorage = nullptr;
TaskHandle_t captureHandle = nullptr, playbackHandle = nullptr;
std::atomic<bool> sessionActive{false};
std::atomic<uint32_t> recordingEpoch{0}, finishingEpoch{0};
std::atomic<uint32_t> sessionEpoch{1}, playbackGeneration{1};
std::atomic<uint32_t> pendingSpeakerFrames{0}, playbackStartedGeneration{0};
std::atomic<uint32_t> audioFault{0};
VoicebotClient voicebot;
voicebot_audio::PcmAssembler ttsAssembler;
voicebot_audio::SilenceTail silenceTail;
bool ready = false, started = false, playbackAnnounced = false;
uint32_t lastTtsAt = 0, retryAt = 0;

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
  while (spkQueue && xQueueReceive(spkQueue, &discarded, 0) == pdTRUE) pendingSpeakerFrames.fetch_sub(1);
  playbackAnnounced = false;
}

void resetSession() {
  sessionActive.store(false);
  sessionEpoch.fetch_add(1);
  recordingEpoch.store(0);
  finishingEpoch.store(0);
  digitalWrite(LED_PIN, LOW);
  silenceTail.reset();
  if (micQueue) xQueueReset(micQueue);
  clearPlayback();
}

void stopRecording(uint32_t epoch) {
  recordingEpoch.store(0);
  digitalWrite(LED_PIN, LOW);
  finishingEpoch.store(epoch);
  AudioFrame end{};
  end.generation = epoch;
  // Capture always reserves one queue slot for this ordered end marker.
  if (xQueueSend(micQueue, &end, 0) != pdTRUE) audioFault.store(1);
  Serial.println("[MIC] Recording OFF; finishing this turn.");
}

void captureTask(void*) {
  int stable = digitalRead(BUTTON_TALK), previous = stable;
  uint32_t changed = millis(), epoch = sessionEpoch.load();
  int32_t raw[FRAME_BYTES / 2];
  AudioFrame frame{};
  for (;;) {
    const uint32_t readEpoch = sessionEpoch.load();
    const bool recordThisRead = recordingEpoch.load() == readEpoch && sessionActive.load();
    size_t received = 0;
    const esp_err_t status = i2s_channel_read(microphone.rxChan(), raw, sizeof(raw), &received, 30);
    const uint32_t now = millis();
    if (epoch != sessionEpoch.load()) {
      epoch = sessionEpoch.load();
      recordingEpoch.store(0);
      digitalWrite(LED_PIN, LOW);
    }
    if (status != ESP_OK && status != ESP_ERR_TIMEOUT) {
      audioFault.store(2);
      recordingEpoch.store(0);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (recordThisRead && sessionActive.load() && readEpoch == epoch && received) {
      const size_t samples = received / sizeof(int32_t);
      frame.generation = epoch;
      frame.length = samples * sizeof(int16_t);
      for (size_t i = 0; i < samples; ++i) {
        const int16_t sample = static_cast<int16_t>(raw[i] >> 16);
        frame.pcm[2 * i] = static_cast<uint8_t>(sample);
        frame.pcm[2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(sample) >> 8);
      }
      if (frame.length && (uxQueueSpacesAvailable(micQueue) <= 1 || xQueueSend(micQueue, &frame, 0) != pdTRUE)) {
        // Do not silently splice speech after a slow/failed network write.
        recordingEpoch.store(0);
        digitalWrite(LED_PIN, LOW);
        audioFault.store(1);
      }
    }
    const int reading = digitalRead(BUTTON_TALK);
    if (reading != previous) changed = now;
    previous = reading;
    if (reading != stable && now - changed >= 30) {
      stable = reading;
      if (stable == LOW) {
        if (recordingEpoch.load() == epoch) stopRecording(epoch);
        else if (sessionActive.load() && epoch == sessionEpoch.load() &&
                 finishingEpoch.load() != epoch && !audioFault.load()) {
          recordingEpoch.store(epoch);
          digitalWrite(LED_PIN, HIGH);
          Serial.println("[MIC] Recording ON. Tap again to finish.");
        } else Serial.println("[MIC] Wait for the session/previous turn to be ready.");
      }
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
      playbackStartedGeneration.store(frame.generation);
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
        if (written) lastProgress = millis();
        if ((status != ESP_OK && status != ESP_ERR_TIMEOUT) || millis() - lastProgress > 200) {
          audioFault.store(3);
          break;
        }
        if (!written) vTaskDelay(1);
      }
    }
    pendingSpeakerFrames.fetch_sub(1);
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
  heap_caps_free(micStorage); micStorage = nullptr;
  heap_caps_free(spkStorage); spkStorage = nullptr;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\nESP-Voicebot: bounded streaming audio for ESP32-S3");
  pinMode(BUTTON_TALK, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  micQueue = makeAudioQueue(MIC_QUEUE_FRAMES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &micQueueState, &micStorage);
  size_t speakerFrames = SPK_PSRAM_FRAMES;
  spkQueue = makeAudioQueue(speakerFrames, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, &spkQueueState, &spkStorage);
  if (!spkQueue) {
    speakerFrames = SPK_INTERNAL_FRAMES;
    spkQueue = makeAudioQueue(speakerFrames, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, &spkQueueState, &spkStorage);
  }
  if (!micQueue || !spkQueue) {
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
    return audioFault.load() ? 0 : ttsAssembler.capacity(uxQueueSpacesAvailable(spkQueue));
  });
  voicebot.setTtsAudioCallback([](const uint8_t* pcm, size_t length) {
    lastTtsAt = millis();
    const bool ok = ttsAssembler.append(pcm, length, playbackGeneration.load(), [](const AudioFrame& frame) {
      pendingSpeakerFrames.fetch_add(1);
      if (xQueueSend(spkQueue, &frame, 0) == pdTRUE) return true;
      pendingSpeakerFrames.fetch_sub(1);
      return false;
    });
    if (!ok) audioFault.store(4);
  });
  voicebot.setBargeInCallback([]() {
    clearPlayback();
    Serial.println("[BARGE_IN] Queued playback cancelled.");
  });
  voicebot.setOpenedCallback([](const String&) {
    resetSession();
    sessionActive.store(true);
    Serial.println("[SESSION] Ready. Tap the talk button to record.");
    reportMemory();
  });
  voicebot.setDisconnectCallback([](const String&) { resetSession(); });
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  ready = true;
  reportMemory();
}

void abortConnection(const char* reason) {
  Serial.printf("[SESSION] %s; retrying in 5 seconds.\n", reason);
  voicebot.stop();
  resetSession();
  started = false;
  retryAt = millis() + 5000;
}

void serviceMicrophone() {
  if (!voicebot.isOpened()) return;
  if (silenceTail.active()) {
    const uint32_t now = millis();
    if (silenceTail.due(now)) {
      static const uint8_t silence[FRAME_BYTES] = {};
      if (!voicebot.sendAudioFrame(silence, sizeof(silence))) {
        abortConnection("Silence send failed");
        return;
      }
      silenceTail.sent(millis());
      if (!silenceTail.active()) finishingEpoch.store(0);
    }
    return;
  }
  AudioFrame frame;
  // Bound work so downlink, controls and keepalive keep progressing.
  for (size_t i = 0; i < 2 && xQueueReceive(micQueue, &frame, 0) == pdTRUE; ++i) {
    if (frame.generation != sessionEpoch.load()) continue;
    if (!frame.length) { silenceTail.start(millis()); break; }
    if (!voicebot.sendAudioFrame(frame.pcm, frame.length)) {
      abortConnection("Microphone send failed");
      break;
    }
  }
}

void loop() {
  if (!ready) { delay(1000); return; }
  const uint32_t fault = audioFault.exchange(0);
  if (fault) {
    Serial.printf("[AUDIO] fault=%u (1=mic backlog, 2=mic I2S, 3=speaker I2S, 4=TTS queue).\n", unsigned(fault));
    abortConnection("Audio pipeline stopped");
  }
  const uint32_t now = millis();
  static uint32_t lastReport = 0, lastWifiRetry = 0;
  if (now - lastReport >= 30000) { lastReport = now; reportMemory(); }
  if (WiFi.status() != WL_CONNECTED) {
    if (started) abortConnection("Wi-Fi lost");
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
  if (!started && static_cast<int32_t>(now - retryAt) >= 0) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < INTERNAL_RESERVE || heap_caps_get_largest_free_block(caps) < TLS_LARGEST_BLOCK) {
      Serial.println("[RAM] Not enough internal TLS headroom; connection deferred.");
      reportMemory();
      retryAt = now + 5000;
    } else {
      started = voicebot.start(BOTNOI_WS_HOST, BOTNOI_WS_PORT, BOTNOI_WS_PATH,
                              BOTNOI_API_KEY, BOTNOI_AGENT_ID, BOTNOI_ROOT_CA);
      if (!started) retryAt = now + 5000;
    }
  }
  if (started) {
    voicebot.loop();
    serviceMicrophone();
    if (voicebot.isOpened()) {
      if (!playbackAnnounced && playbackStartedGeneration.load() == playbackGeneration.load()) {
        if (voicebot.sendPlaybackStarted()) playbackAnnounced = true;
        else abortConnection("Playback notification failed");
      }
      if (playbackAnnounced && !pendingSpeakerFrames.load() && !voicebot.isReceivingAudio() && millis() - lastTtsAt >= 400) {
        if (!voicebot.sendPlaybackCompleted()) abortConnection("Playback notification failed");
        playbackAnnounced = false;
        playbackStartedGeneration.store(0);
      }
    }
  }
  delay(1);
}
