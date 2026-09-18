// Standalone ESP32-S3, INMP441 microphone and MAX98357A speaker.
// Only loop() owns WebSocket/TLS. Audio tasks use fixed queues.
#include <Arduino.h>
#include <WiFi.h>
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
#include "capture_pipeline.h"
#include "speaker_packetizer.h"
#include "duplex_audio.h"
#include "echo_canceller.h"
#include "playback_flow.h"
#include "session_flow.h"
#include "voicebot_client.h"
#include "tls_roots.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "Select ESP32-S3 Dev Module. Other ESP32 variants need a reviewed pin map."
#endif

constexpr int BUTTON_SESSION = VOICEBOT_BUTTON_PIN;
constexpr int LED_PIN = 13;
constexpr size_t FRAME_BYTES = voicebot_audio::kFrameBytes;
constexpr size_t MIC_QUEUE_FRAMES = voicebot_audio::kMicrophoneQueueFrames;
constexpr size_t SPK_PSRAM_FRAMES = 800, SPK_INTERNAL_FRAMES = 24;
constexpr uint32_t CAPTURE_STACK_BYTES = 8192, PLAYBACK_STACK_BYTES = 4096;
constexpr uint32_t INTERNAL_RESERVE = 64 * 1024, TLS_LARGEST_BLOCK = 32 * 1024;
using voicebot_audio::AudioFrame;

voicebot_audio::DuplexAudio duplexAudio;
voicebot_audio::EchoCanceller echoCanceller;
voicebot_audio::CapturePipeline capturePipeline;
QueueHandle_t micQueue = nullptr, spkQueue = nullptr;
StaticQueue_t micQueueState, spkQueueState;
uint8_t *micStorage = nullptr, *spkStorage = nullptr;
TaskHandle_t captureHandle = nullptr, playbackHandle = nullptr;
std::atomic<bool> sessionActive{false};
std::atomic<bool> micSuppressed{false};
std::atomic<uint32_t> micFullDrops{0};
std::atomic<uint32_t> recordingEpoch{0};
std::atomic<uint32_t> recordingStartedAt{0};
std::atomic<uint32_t> sessionEpoch{1}, playbackGeneration{1};
std::atomic<uint32_t> pendingSpeakerFrames{0}, playbackPartialSamples{0};
std::atomic<uint32_t> playbackDrainedAt{0};
std::atomic<bool> playbackDrainedValid{false};
std::atomic<uint32_t> audioFault{0};
std::atomic<bool> audioHealthy{true};
VoicebotClient voicebot;
voicebot_audio::PcmAssembler ttsAssembler;
voicebot_audio::ReplyGate replyGate;
voicebot_audio::MicrophonePacer micPacer;
voicebot_audio::PlaybackFlow playbackFlow;
voicebot_session::SessionIntent sessionIntent;
voicebot_session::ReconnectBackoff reconnectBackoff;
bool ready = false, micPauseAnnounced = false;
bool aecActive = false, fullDuplex = false;  // Fixed before audio tasks start.
std::atomic<uint32_t> captureDiscontinuities{0};
uint32_t connectionIntent = 0;
uint32_t micExpiredDrops = 0, micPausedDrops = 0, maxSocketMs = 0, maxMicSendMs = 0;

void reportAudioFault(uint32_t fault) {
  if (audioHealthy.exchange(false)) audioFault.store(fault);
}

QueueHandle_t makeAudioQueue(size_t count, uint32_t caps, StaticQueue_t* state, uint8_t** storage) {
  *storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame), caps));
  if (!*storage) return nullptr;
  QueueHandle_t queue = xQueueCreateStatic(count, sizeof(AudioFrame), *storage, state);
  if (!queue) { heap_caps_free(*storage); *storage = nullptr; }
  return queue;
}

void reportMemory() {
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Serial.printf("[RAM] internal free=%u min=%u largest=%u; PSRAM free=%u; speaker queued=%u staged/DMA=%u\n",
      unsigned(heap_caps_get_free_size(caps)), unsigned(heap_caps_get_minimum_free_size(caps)),
      unsigned(heap_caps_get_largest_free_block(caps)),
      unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), unsigned(pendingSpeakerFrames.load()), unsigned(duplexAudio.pendingBlocks()));
  Serial.printf("[FLOW] mic queued=%u replaced/expired=%u/%u paused=%s; max socket=%ums send=%ums\n",
      unsigned(micQueue ? uxQueueMessagesWaiting(micQueue) : 0), unsigned(micFullDrops.load()),
      unsigned(micExpiredDrops), micSuppressed.load() ? "yes" : "no",
      unsigned(maxSocketMs), unsigned(maxMicSendMs));
  const auto diagnostic = duplexAudio.diagnostics();
  const auto aec = echoCanceller.stats();
  Serial.printf("[AEC] active=%s frames=%u max process=%uus; resync=%u RX/ref drops=%u/%u alignment=%u DMA gaps=%u\n",
      aecActive ? "yes" : "no", unsigned(aec.processedFrames), unsigned(aec.maxProcessMicros),
      unsigned(captureDiscontinuities.load()), unsigned(diagnostic.rxOverflows), unsigned(diagnostic.referenceOverflows),
      unsigned(diagnostic.alignmentDrops), unsigned(diagnostic.dmaDiscontinuities));
  if (captureHandle && playbackHandle) {
    Serial.printf("[STACK] minimum unused bytes: capture=%u playback=%u loop=%u\n",
        unsigned(uxTaskGetStackHighWaterMark(captureHandle)),
        unsigned(uxTaskGetStackHighWaterMark(playbackHandle)),
        unsigned(uxTaskGetStackHighWaterMark(nullptr)));
  }
}

void invalidatePlayback() {
  uint32_t previous = playbackGeneration.load();
  uint32_t next;
  do {
    next = previous + 1;
    if (!next) next = 1;  // Zero means no I2S samples have started.
  } while (!playbackGeneration.compare_exchange_weak(previous, next));
  duplexAudio.invalidate(next);
}

void releaseSpeakerFrame() {
  // Publish drain telemetry before the last frame makes pending==0 visible.
  // Timestamp zero is valid at millis() rollover.
  playbackDrainedAt.store(millis());
  playbackDrainedValid.store(true);
  pendingSpeakerFrames.fetch_sub(1);
}

void clearPlayback() {
  invalidatePlayback();
  ttsAssembler.reset();
  AudioFrame discarded;
  while (spkQueue && xQueueReceive(spkQueue, &discarded, 0) == pdTRUE) {
    releaseSpeakerFrame();
  }
  if (!pendingSpeakerFrames.load()) {
    playbackDrainedAt.store(millis());
    playbackDrainedValid.store(true);
  }
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
  playbackFlow.reset();
  if (micQueue) xQueueReset(micQueue);
  clearPlayback();
  duplexAudio.clearPlaybackProgress();
  playbackDrainedValid.store(false);
}

void captureTask(void*) {
  int stable = digitalRead(BUTTON_SESSION), previous = stable;
  uint32_t changed = millis(), lastCapture = changed;
  bool pressedToStart = !sessionIntent.requested();
  voicebot_audio::AlignedAudioBlock block{};
  for (;;) {
    const uint32_t readEpoch = sessionEpoch.load();
    const bool received = duplexAudio.receive(block, 20);
    const uint32_t now = millis();
    const int reading = digitalRead(BUTTON_SESSION);
    if (reading != previous) {
      changed = now;
      if (reading == LOW) pressedToStart = !sessionIntent.requested();
    }
    previous = reading;
    if (reading != stable && now - changed >= 30) {
      stable = reading;
      if (stable == LOW) {
        sessionIntent.publish(pressedToStart);
        if (!pressedToStart) {
          recordingEpoch.store(0);
          micSuppressed.store(true);
          invalidatePlayback();
          digitalWrite(LED_PIN, LOW);
        }
      }
    }
    if (!received) {
      if (now - lastCapture >= 500 || duplexAudio.invalidDmaEvents()) reportAudioFault(2);
      continue;  // Keep the physical Stop button usable after a hardware error.
    }
    lastCapture = now;
    if (block.discontinuity) captureDiscontinuities.fetch_add(1);
    const uint32_t epoch = sessionEpoch.load();
    const bool upload = readEpoch == epoch && recordingEpoch.load() == epoch &&
        sessionActive.load() && !micSuppressed.load() &&
        static_cast<int32_t>(block.capturedAtMillis - 10 - recordingStartedAt.load()) >= 0;
    const bool ok = capturePipeline.push(block.mic, block.reference, voicebot_audio::kDuplexSamples,
        block.capturedAtMillis, epoch, upload, block.discontinuity,
        [](const int16_t* mic, const int16_t* reference, int16_t* output, size_t samples) {
          if (aecActive) return echoCanceller.process(mic, reference, output, samples);
          memcpy(output, mic, samples * sizeof(int16_t));
          return true;
        },
        [](const AudioFrame& frame) {
          // DSP continues through every sample. Only network packets are
          // evicted under TLS congestion; the acoustic reference never skips.
          if (!sessionActive.load() || recordingEpoch.load() != frame.generation || micSuppressed.load()) return true;
          return voicebot_audio::enqueueRecentMicrophone(frame,
              [](const AudioFrame& value) { return xQueueSend(micQueue, &value, 0) == pdTRUE; },
              []() {
                AudioFrame discarded;
                const bool dropped = xQueueReceive(micQueue, &discarded, 0) == pdTRUE;
                if (dropped) micFullDrops.fetch_add(1);
                return dropped;
              });
        });
    if (!ok) reportAudioFault(7);
    vTaskDelay(1);
  }
}

void playbackTask(void*) {
  AudioFrame frame{};
  voicebot_audio::SpeakerPacketizer packetizer(70);
  bool haveFrame = false;
  size_t offset = 0;
  uint32_t lastInput = millis(), lastProgress = lastInput;
  for (;;) {
    const uint32_t generation = playbackGeneration.load();
    if (packetizer.size() && packetizer.generation() != generation) {
      packetizer.reset();
      playbackDrainedAt.store(millis());
      playbackDrainedValid.store(true);
      playbackPartialSamples.store(0);
    }
    // A new-current frame may already be held while only the old partial is
    // stale. Retire each ownership independently so Stop/Start cannot lose it.
    if (haveFrame && frame.generation != generation) { releaseSpeakerFrame(); haveFrame = false; }
    // A delayed player can wake with contiguous PCM already in the queue.
    // Consume that before treating elapsed wall time as an end-of-burst gap.
    bool partialIdle = packetizer.size() && !packetizer.full() && !haveFrame && millis() - lastInput >= 20;
    if (partialIdle && xQueueReceive(spkQueue, &frame, 0) == pdTRUE) {
      haveFrame = true;
      offset = 0;
      partialIdle = false;
      if (frame.generation != playbackGeneration.load()) { releaseSpeakerFrame(); haveFrame = false; continue; }
    }
    // Pack across arbitrary WebSocket boundaries. Only a final partial block
    // with an empty source queue and 20ms input idle is padded by the adapter.
    if (packetizer.full() || partialIdle) {
      if (duplexAudio.submit(packetizer.data(), packetizer.size(), packetizer.generation())) {
        // Driver accounting is visible before removing our buffered samples.
        packetizer.reset();
        playbackPartialSamples.store(0);
        lastProgress = millis();
      } else {
        if (millis() - lastProgress > 200 && packetizer.generation() == playbackGeneration.load()) reportAudioFault(3);
        vTaskDelay(1);
      }
      continue;
    }
    if (!haveFrame) {
      const TickType_t wait = packetizer.size() ? pdMS_TO_TICKS(1) : pdMS_TO_TICKS(20);
      if (xQueueReceive(spkQueue, &frame, wait) != pdTRUE) { lastProgress = millis(); continue; }
      haveFrame = true;
      offset = 0;
      if (frame.generation != playbackGeneration.load()) { releaseSpeakerFrame(); haveFrame = false; continue; }
    }
    // Stop can happen between the loop-top check and dequeue. Revalidate both
    // objects before append; a cancelled partial is not a pipeline failure.
    const uint32_t currentGeneration = playbackGeneration.load();
    if (packetizer.size() && packetizer.generation() != currentGeneration) {
      packetizer.reset();
      playbackDrainedAt.store(millis());
      playbackDrainedValid.store(true);
      playbackPartialSamples.store(0);
    }
    if (frame.generation != currentGeneration) { releaseSpeakerFrame(); haveFrame = false; continue; }
    const size_t copied = packetizer.append(frame, offset);
    if (!copied) {
      if (frame.generation == playbackGeneration.load()) reportAudioFault(4);
      releaseSpeakerFrame();
      haveFrame = false;
      continue;
    }
    offset += copied;
    lastInput = millis();
    // Publish the partial block before releasing the source queue frame.
    playbackPartialSamples.store(packetizer.size());
    if (offset == frame.length / 2) { releaseSpeakerFrame(); haveFrame = false; }
  }
}

void releaseAudio() {
  // Only used before ready=true; loop() cannot touch partial initialization.
  if (captureHandle) { vTaskDelete(captureHandle); captureHandle = nullptr; }
  if (playbackHandle) { vTaskDelete(playbackHandle); playbackHandle = nullptr; }
  duplexAudio.end();
  echoCanceller.end();
  if (micQueue) { vQueueDelete(micQueue); micQueue = nullptr; }
  if (spkQueue) { vQueueDelete(spkQueue); spkQueue = nullptr; }
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
  Serial.println("[SESSION] Tap once to start a hands-free call; tap again to hang up.");
  pinMode(BUTTON_SESSION, INPUT_PULLUP);
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
  // Native AEC allocates once before Wi-Fi or any competing audio tasks. The
  // library requires PSRAM internally even if its public caps say otherwise.
  if (VOICEBOT_AEC_ENABLED) {
    aecActive = echoCanceller.begin() == voicebot_audio::EchoCancellerStatus::Ready;
    const auto aec = echoCanceller.stats();
    Serial.printf("[AEC] %s; allocated internal=%u PSRAM=%u bytes.\n", echoCanceller.statusMessage(),
        unsigned(aec.internalBytesUsed), unsigned(aec.psramBytesUsed));
    if (!aecActive) Serial.println("[AEC] Half-duplex fallback: voice barge-in unavailable. Check PSRAM/OPI selection and memory diagnostics.");
  }
  fullDuplex = aecActive || (!VOICEBOT_AEC_ENABLED && VOICEBOT_FULL_DUPLEX);
  Serial.println(aecActive ? "[MIC] Echo-cancelled full duplex: keep speaking to interrupt the bot."
                          : (fullDuplex ? "[MIC] Raw full duplex explicitly enabled; external echo control required."
                                        : "[MIC] Half duplex: silence during bot replies, then listening resumes."));
  if (!capturePipeline.configure(aecActive ? echoCanceller.frameSamples() : voicebot_audio::kDuplexSamples, aecActive) ||
      !duplexAudio.begin()) {
    Serial.println("[ERROR] Duplex I2S initialization failed; network disabled.");
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
    // Leave new audio in TCP until the cancelled playback has drained and its
    // notifications have been sent. Never discard a timed window of TTS.
    if (!audioHealthy.load() || !sessionIntent.current(connectionIntent) || playbackFlow.blocksAudio()) return 0;
    return ttsAssembler.capacity(uxQueueSpacesAvailable(spkQueue));
  });
  voicebot.setTtsAudioCallback([](const uint8_t* pcm, size_t length) {
    const uint32_t now = millis();
    // Snapshot before intent: a concurrent Stop must leave these samples on
    // the invalidated generation, never relabel old PCM as new playback.
    const uint32_t generation = playbackGeneration.load();
    if (!sessionIntent.current(connectionIntent)) return; // A concurrent Stop wins.
    if (!playbackFlow.onAudio(now, generation)) {
      reportAudioFault(4);
      return;
    }
    playbackDrainedValid.store(false);
    replyGate.onAudio(now);
    if (!fullDuplex) micSuppressed.store(true);
    const bool ok = ttsAssembler.append(pcm, length, generation, [](const AudioFrame& frame) {
      pendingSpeakerFrames.fetch_add(1);
      if (xQueueSend(spkQueue, &frame, 0) == pdTRUE) return true;
      pendingSpeakerFrames.fetch_sub(1);
      return false;
    });
    if (!ok) reportAudioFault(4);
    if (!pendingSpeakerFrames.load()) {
      // Even a malformed burst containing only one dangling PCM byte must
      // reach the idle completion path instead of muting capture forever.
      playbackDrainedAt.store(millis());
      playbackDrainedValid.store(true);
    }
  });
  voicebot.setBargeInCallback([]() {
    if (playbackFlow.cancel(playbackGeneration.load())) clearPlayback();
    Serial.println("[BARGE_IN] Queued playback cancelled; preserving the next reply.");
  });
  voicebot.setBotReplyCallback([](const String&) {
    playbackFlow.onEndHint();
  });
  voicebot.setOpenedCallback([](const String&) {
    if (!sessionIntent.current(connectionIntent)) {
      voicebot.requestClose();
      return;
    }
    resetSession();
    sessionActive.store(true);
    reconnectBackoff.opened(millis());
    recordingStartedAt.store(millis());
    recordingEpoch.store(sessionEpoch.load());
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[SESSION] Ready; microphone streams continuously until hangup.");
    reportMemory();
  });
  voicebot.setDisconnectCallback([](const String&) {
    handleSessionFailure("Server or transport closed", voicebot.retryableDisconnect());
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

void handleSessionFailure(const char* reason, bool retryable) {
  resetSession();
  if (!sessionIntent.current(connectionIntent)) return;
  if (retryable) {
    const uint32_t wait = reconnectBackoff.failed(millis(), esp_random());
    Serial.printf("[SESSION] %s; retry in %ums while Start is active (new server session).\n", reason, unsigned(wait));
  } else {
    sessionIntent.end(connectionIntent);
    Serial.printf("[SESSION] %s; call ended. Tap Start after resolving the reported error, if any.\n", reason);
  }
}

void abortConnection(const char* reason, bool retryable) {
  voicebot.stop();
  handleSessionFailure(reason, retryable);
}

void serviceMicrophone() {
  if (!sessionActive.load() || !sessionIntent.current(connectionIntent) || !voicebot.isOpened()) return;
  const bool pause = !fullDuplex &&
      (playbackFlow.micPaused() ||
       replyGate.paused(millis(), voicebot.isReceivingAudio(),
           pendingSpeakerFrames.load() != 0 || playbackPartialSamples.load() != 0 || duplexAudio.pendingBlocks() != 0));
  micSuppressed.store(pause);
  if (pause != micPauseAnnounced) {
    micPauseAnnounced = pause;
    if (recordingEpoch.load() == sessionEpoch.load()) {
      Serial.println(pause ? "[MIC] Echo muted during bot reply; sending silence."
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
  }
  // A congested socket is not a session error. The fixed recent-audio queue
  // absorbs DSP packet bursts and evicts old PCM; the age check bounds latency.
  const uint32_t now = millis();
  if (!micPacer.due(now) || !voicebot.canSendNow()) return;
  if (pause) {
    // Preserve the API's continuous PCM cadence without uploading loudspeaker
    // echo on hardware without AEC. Congestion still skips stale time slots.
    frame.length = FRAME_BYTES;
    memset(frame.pcm, 0, frame.length);
  } else {
    if (xQueueReceive(micQueue, &frame, 0) != pdTRUE) return;
    if (frame.generation != sessionEpoch.load() ||
        voicebot_audio::microphoneFrameExpired(now, frame.capturedAt)) {
      ++micExpiredDrops;
      return;
    }
  }
  if (!sessionIntent.current(connectionIntent)) return;
  const uint32_t sendAt = millis();
  if (!voicebot.sendAudioFrame(frame.pcm, frame.length)) {
    // VoicebotClient reports/classifies a transport write failure itself.
    if (voicebot.isRunning()) abortConnection("Microphone send failed", true);
    return;
  }
  maxMicSendMs = max(maxMicSendMs, uint32_t(millis() - sendAt));
  micPacer.sent(sendAt, frame.length);
}

void loop() {
  if (!ready) { delay(1000); return; }
  const uint32_t now = millis();
  uint32_t intent = 0;
  if (sessionIntent.consume(intent)) {
    // A newer Start can include a Stop received while TLS was busy. Finish the
    // old close first; only then may the latest intent open a fresh session.
    resetSession();
    if (voicebot.isRunning()) voicebot.requestClose();
    reconnectBackoff.reset(now);
    if (sessionIntent.requested(intent)) {
      if (!audioHealthy.load()) {
        sessionIntent.end(intent);
        Serial.println("[SESSION] Audio hardware/pipeline fault is latched; restart after checking the reported fault.");
      } else Serial.println("[SESSION] Start requested; one persistent call until Stop.");
    } else Serial.println("[SESSION] Hangup requested; microphone and queued playback stopped.");
  }
  const uint32_t fault = audioFault.exchange(0);
  if (fault) {
    Serial.printf("[AUDIO] fault=%u (2=duplex I2S, 3=speaker staging, 4=TTS queue, 7=AEC/capture pipeline).\n",
                  unsigned(fault));
    abortConnection("Audio pipeline stopped", false);
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
    if (voicebot.isRunning()) abortConnection("Wi-Fi lost", true);
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
  if (sessionIntent.requested() && audioHealthy.load() && !voicebot.isRunning() && reconnectBackoff.due(now)) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < INTERNAL_RESERVE || heap_caps_get_largest_free_block(caps) < TLS_LARGEST_BLOCK) {
      Serial.println("[RAM] Not enough internal TLS headroom; connection deferred.");
      reportMemory();
      reconnectBackoff.failed(now, esp_random());
    } else {
      connectionIntent = sessionIntent.snapshot();
      if (!voicebot.start(BOTNOI_WS_HOST, BOTNOI_WS_PORT, BOTNOI_WS_PATH,
                          BOTNOI_API_KEY, BOTNOI_AGENT_ID, BOTNOI_ROOT_CA)) {
        handleSessionFailure("Connection could not start", voicebot.retryableDisconnect());
      }
    }
  }
  if (voicebot.isRunning()) {
    // A button edge can arrive during start()/TLS work, after this iteration's
    // intent snapshot. Stop the old attempt before doing more socket work.
    if (!sessionIntent.current(connectionIntent) && !voicebot.isClosing()) voicebot.requestClose();
    const uint32_t socketAt = millis();
    voicebot.loop();
    maxSocketMs = max(maxSocketMs, uint32_t(millis() - socketAt));
    if (voicebot.isOpened()) {
      const uint32_t controlNow = millis();
      reconnectBackoff.maintain(controlNow);
      if (sessionIntent.current(connectionIntent)) {
        using voicebot_audio::PlaybackAction;
        // Read pending first: pending==0 acquires the worker's prior drain
        // telemetry. Function argument evaluation order alone cannot do this.
        const uint32_t softwarePending = pendingSpeakerFrames.load();
        const uint32_t partialPending = playbackPartialSamples.load() ? 1 : 0;
        const uint32_t pending = softwarePending + partialPending + duplexAudio.pendingBlocks();
        const bool softwareDrainedValid = playbackDrainedValid.load();
        const bool dmaDrainedValid = duplexAudio.drainedValid();
        const bool drainedValid = softwareDrainedValid || dmaDrainedValid;
        const uint32_t softwareDrainedAt = playbackDrainedAt.load();
        const uint32_t dmaDrainedAt = duplexAudio.drainedAt();
        const uint32_t drainedAt = !softwareDrainedValid ? dmaDrainedAt :
            (!dmaDrainedValid ? softwareDrainedAt :
             (static_cast<int32_t>(dmaDrainedAt - softwareDrainedAt) > 0 ? dmaDrainedAt : softwareDrainedAt));
        const uint32_t startedGeneration = duplexAudio.startedGeneration();
        const bool wasActive = playbackFlow.micPaused();
        // Take now after the worker timestamps, including at tick rollover.
        const PlaybackAction action = playbackFlow.nextAction(millis(),
            startedGeneration, pending, drainedValid, drainedAt, voicebot.isReceivingAudio());
        if (action != PlaybackAction::None && voicebot.canSendNow()) {
          const bool sent = action == PlaybackAction::Started ? voicebot.sendPlaybackStarted()
                                                             : voicebot.sendPlaybackCompleted();
          if (sent) {
            playbackFlow.acknowledge(action);
          } else if (voicebot.isRunning()) abortConnection("Playback notification failed", true);
        }
        if (wasActive && !playbackFlow.micPaused()) {
          ttsAssembler.reset();  // Never carry a dangling byte into a new burst.
          duplexAudio.clearPlaybackProgress();
          playbackDrainedValid.store(false);
          replyGate.reset();
        }
      }
      serviceMicrophone();
    }
  }
  delay(1);
}
