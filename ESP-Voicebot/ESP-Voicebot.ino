// Standalone ESP32-S3: Botnoi Voicebot Client (No PC bridge needed!)
// Connects directly to Botnoi Voicebot WebSocket API over Wi-Fi.

#include <Arduino.h>
#include <WiFi.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>
#include <atomic>
#include <time.h>
#include <esp_heap_caps.h>

#include "voice_activity.h"
#include "voicebot_client.h"
#include "tls_roots.h"

#if __has_include("config.local.h")
#include "config.local.h"
#else
#include "config.example.h"
#endif

// Hardware Pin Configuration (Same as ESP-BytePlus)
constexpr int BUTTON_TALK = 46;
constexpr int MIC_SCK = 3, MIC_WS = 2, MIC_SD = 1;
constexpr int SPK_BCLK = 38, SPK_LRC = 39, SPK_DIN = 40;
constexpr int LED_PIN = 48;

// Audio Configuration
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t FRAME_BYTES = 640;         // 20ms frame @ 16kHz 16-bit mono
constexpr size_t MIC_QUEUE_FRAMES = 100;
constexpr size_t SPK_QUEUE_FRAMES = 200;

struct AudioFrame {
  uint16_t length;
  uint8_t pcm[FRAME_BYTES];
};

// Global Hardware & Pipeline Handles
I2SClass microphone, speaker;
QueueHandle_t micQueue = nullptr, spkQueue = nullptr;
StaticQueue_t micQueueState, spkQueueState;

std::atomic<bool> isRecording{false};
std::atomic<bool> isPlaying{false};
std::atomic<bool> sessionActive{false};
std::atomic<uint32_t> lastSpeechTime{0};

VoiceActivity voiceDetector;
VoicebotClient voicebot;

// Create static queues in PSRAM or DRAM fallback
QueueHandle_t makeAudioQueue(size_t count, StaticQueue_t* state) {
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!storage) storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame), MALLOC_CAP_8BIT));
  if (!storage) return nullptr;
  return xQueueCreateStatic(count, sizeof(AudioFrame), storage, state);
}

// Microphone Capture & Button Debounce Task
void captureTask(void*) {
  int stable = digitalRead(BUTTON_TALK), previous = stable;
  uint32_t changed = millis();
  int32_t raw[FRAME_BYTES / 2];
  int16_t pcm[FRAME_BYTES / 2];
  AudioFrame frame{};

  bool wasRecording = false;

  while (true) {
    if (!microphone.rxChan()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    size_t received = 0;
    esp_err_t readStatus = i2s_channel_read(microphone.rxChan(), raw, sizeof(raw), &received, 30);
    const uint32_t now = millis();

    // Button debouncing (Pin 46 INPUT_PULLUP)
    const int reading = digitalRead(BUTTON_TALK);
    if (reading != previous) changed = now;
    previous = reading;

    if (reading != stable && now - changed >= 30) {
      stable = reading;
      if (stable == LOW) {
        // Toggle recording state on button press
        bool nextState = !isRecording.load();
        isRecording.store(nextState);
        digitalWrite(LED_PIN, nextState ? HIGH : LOW);
        Serial.printf("[BUTTON] Recording Toggled: %s\n", nextState ? "ON 🔴 (Recording...)" : "OFF ⏹️ (Processing...)");
        if (!nextState) {
          // Send 500ms silence padding when stopping recording to trigger server VAD
          voicebot.sendSilencePadding(25);
        }
      }
    }

    if (readStatus != ESP_OK && readStatus != ESP_ERR_TIMEOUT) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Convert 24-bit INMP441 samples to signed PCM16
    memset(pcm, 0, sizeof(pcm));
    const size_t samples = min(received / sizeof(int32_t), FRAME_BYTES / 2);
    for (size_t i = 0; i < samples; ++i) {
      const int32_t sample = raw[i] >> 16;
      pcm[i] = static_cast<int16_t>(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
    }

    const bool speech = voiceDetector.speech(pcm, FRAME_BYTES / 2);
    if (speech) {
      lastSpeechTime.store(now);
    }

    const bool currentlyRecording = isRecording.load();

    if (currentlyRecording) {
      frame.length = FRAME_BYTES;
      memcpy(frame.pcm, pcm, sizeof(pcm));
      if (micQueue) {
        xQueueSend(micQueue, &frame, 0);
      }
      wasRecording = true;
    } else {
      if (wasRecording) {
        wasRecording = false;
        Serial.println("[MIC] Recording finished.");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Speaker Output Playback Task
void playbackTask(void*) {
  AudioFrame frame{};
  int16_t stereo[FRAME_BYTES];

  while (true) {
    if (xQueueReceive(spkQueue, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
      if (isPlaying.load() && uxQueueMessagesWaiting(spkQueue) == 0) {
        isPlaying.store(false);
        voicebot.sendPlaybackCompleted();
        Serial.println("[SPEAKER] Audio playback completed.");
      }
      continue;
    }

    if (!isPlaying.load()) {
      isPlaying.store(true);
      voicebot.sendPlaybackStarted();
      Serial.println("[SPEAKER] Audio playback started.");
    }

    // Convert mono PCM16 to stereo with 70% volume scaling
    for (size_t i = 0; i < frame.length / 2; ++i) {
      int16_t sample = static_cast<int16_t>(frame.pcm[2 * i] | (frame.pcm[2 * i + 1] << 8));
      stereo[2 * i] = stereo[2 * i + 1] = static_cast<int16_t>(static_cast<int32_t>(sample) * 70 / 100);
    }

    size_t length = frame.length * 2, offset = 0;
    while (offset < length) {
      size_t written = 0;
      esp_err_t status = i2s_channel_write(speaker.txChan(),
          reinterpret_cast<uint8_t*>(stereo) + offset, length - offset, &written, 50);
      offset += written;
      if ((status != ESP_OK && status != ESP_ERR_TIMEOUT) || !written) {
        break;
      }
    }
  }
}

// Synchronize NTP time for SSL Certificate Validation
void syncNtpTime() {
  Serial.print("[NTP] Synchronizing NTP time...");
  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("\n[NTP] Time synchronized successfully!");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==================================================");
  Serial.println("🤖 ESP-Voicebot: Standalone ESP32 Botnoi Voicebot Client");
  Serial.println("==================================================");

  pinMode(BUTTON_TALK, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize Queues
  micQueue = makeAudioQueue(MIC_QUEUE_FRAMES, &micQueueState);
  spkQueue = makeAudioQueue(SPK_QUEUE_FRAMES, &spkQueueState);
  if (!micQueue || !spkQueue) {
    Serial.println("[ERROR] Failed to allocate audio queues!");
    return;
  }

  // Initialize WebRTC VAD
  voiceDetector.begin();

  // Initialize Microphone I2S (INMP441: BCLK=3, WS=2, SD=1)
  microphone.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);

  // Initialize Speaker I2S (MAX98357A: BCLK=38, LRC=39, DIN=40)
  speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN, -1);

  if (!microphone.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT) ||
      !speaker.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[ERROR] Failed to initialize I2S Microphone or Speaker!");
    return;
  }
  Serial.println("[I2S] Microphone (INMP441) & Speaker (MAX98357A) initialized successfully.");

  // Connect Wi-Fi
  Serial.printf("[Wi-Fi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[Wi-Fi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Sync NTP Time
  syncNtpTime();

  // Setup Voicebot Client Callbacks
  voicebot.setTtsAudioCallback([](const uint8_t* pcm, size_t length) {
    AudioFrame frame{};
    while (length > 0) {
      size_t chunk = min(length, (size_t)FRAME_BYTES);
      frame.length = chunk;
      memcpy(frame.pcm, pcm, chunk);
      if (spkQueue) {
        xQueueSend(spkQueue, &frame, 0);
      }
      pcm += chunk;
      length -= chunk;
    }
  });

  voicebot.setBargeInCallback([]() {
    // On Barge-in: instant clear speaker queue
    if (spkQueue) {
      xQueueReset(spkQueue);
    }
    isPlaying.store(false);
    Serial.println("[BARGE_IN] Speaker queue cleared.");
  });

  // Start FreeRTOS Core Tasks
  xTaskCreate(captureTask, "captureTask", 8192, nullptr, 2, nullptr);
  xTaskCreate(playbackTask, "playbackTask", 8192, nullptr, 2, nullptr);

  Serial.println("[SYSTEM] System Ready! Press button on GPIO46 to toggle speech recording.\n");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    return;
  }

  // Initiate WebSocket client connection once
  static bool started = false;
  if (!started) {
    started = true;
    Serial.println("[Voicebot] Connecting to Botnoi Voicebot WebSocket...");
    voicebot.start(BOTNOI_WS_HOST, BOTNOI_WS_PORT, BOTNOI_WS_PATH, BOTNOI_API_KEY, BOTNOI_AGENT_ID, nullptr);
  }

  // Process WebSocket background events & auto-reconnect continuously
  voicebot.loop();

  // Stream microphone audio from queue to WebSocket when opened
  if (voicebot.isOpened()) {
    AudioFrame frame{};
    while (micQueue && xQueueReceive(micQueue, &frame, 0) == pdTRUE) {
      voicebot.sendAudioFrame(frame.pcm, frame.length);
    }
  } else {
    // Drain mic queue when not connected
    if (micQueue) {
      AudioFrame dummy{};
      while (xQueueReceive(micQueue, &dummy, 0) == pdTRUE);
    }
  }

  delay(1);
}
