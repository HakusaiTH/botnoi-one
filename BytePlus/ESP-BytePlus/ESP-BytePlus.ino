// Standalone ESP32-S3: tap-to-toggle conversation -> ASR -> ModelArk -> PCM TTS.
// See README.md for board selection, configuration and verification.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <ArduinoJson.h>
#include <atomic>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "asr_protocol.h"
#include "tts_protocol.h"
#include "cloud_socket.h"
#include "tls_roots.h"
#include "voice_activity.h"
#include "conversation_session.h"
#include "model_stream.h"
#if __has_include("config.local.h")
#include "config.local.h"
#else
#include "config.example.h"
#endif

constexpr int BUTTON_TALK = 46;
constexpr int MIC_SCK = 3, MIC_WS = 2, MIC_SD = 1;
constexpr int SPK_BCLK = 38, SPK_LRC = 39, SPK_DIN = 40;
constexpr int LED_PIN = 48;
constexpr uint32_t SAMPLE_RATE = 16000, MAX_RECORD_MS = 20000;
constexpr size_t FRAME_BYTES = 640, ASR_BATCH_BYTES = 6400;
constexpr size_t MIC_QUEUE_FRAMES = 150, SPK_QUEUE_FRAMES = 48;
constexpr size_t PRE_ROLL_FRAMES = 12, HISTORY_TURNS = 4;
static_assert(FRAME_BYTES == VoiceActivity::kFrameSamples * sizeof(int16_t) &&
              SAMPLE_RATE == VoiceActivity::kSampleRate, "VAD requires 20 ms PCM16 at 16 kHz");
constexpr char VOICE_HOST[] = "voice.ap-southeast-1.bytepluses.com";
constexpr char ARK_URL[] = "https://ark.ap-southeast.bytepluses.com/api/v3/chat/completions";

struct AudioFrame {
  uint32_t generation;
  uint16_t length;
  uint8_t pcm[FRAME_BYTES];
};
I2SClass microphone, speaker;
QueueHandle_t micQueue = nullptr, spkQueue = nullptr;
StaticQueue_t micQueueState, spkQueueState;
std::atomic<uint32_t> generation{0}, recordingEnded{0}, audioFault{0}, pendingAudio{0};
std::atomic<uint32_t> requestedTurn{0}, sessionEpoch{0};
std::atomic<uint32_t> lastSpeechAt{0}, utteranceEndedAt{0};
std::atomic<uint32_t> serverEndpoint{0};
std::atomic<bool> sessionEnabled{false}, networkReady{false}, pipelineBusy{false};
enum class FaultReason : uint8_t { Audio, Asr, Model, Tts };
std::atomic<FaultReason> faultReason{FaultReason::Audio};
VoiceActivity voiceDetector;
AudioFrame preRoll[PRE_ROLL_FRAMES];
String historyUsers[HISTORY_TURNS], historyReplies[HISTORY_TURNS];
size_t historyCount = 0;
uint8_t outbound[ASR_BATCH_BYTES + FRAME_BYTES + 12];
CloudSocket cloud;
bool ready = false;

String uuid() {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  char value[37];
  snprintf(value, sizeof(value),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return String(value);
}

bool active(uint32_t turn) {
  return sessionEnabled.load() && generation.load() == turn && audioFault.load() != turn &&
         WiFi.status() == WL_CONNECTED;
}

QueueHandle_t makeAudioQueue(size_t count, StaticQueue_t* state) {
  auto* storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame),
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!storage) storage = static_cast<uint8_t*>(heap_caps_malloc(count * sizeof(AudioFrame), MALLOC_CAP_8BIT));
  if (!storage) return nullptr;
  return xQueueCreateStatic(count, sizeof(AudioFrame), storage, state);
}

void captureTask(void*) {
  using byteplus::conversation::Action;
  byteplus::conversation::Controller session;
  int stable = digitalRead(BUTTON_TALK), previous = stable;
  uint32_t changed = millis(), turn = 0;
  size_t preRollNext = 0, preRollCount = 0;
  bool awaitingAnnounced = false, detectorWasActive = false;
  uint32_t utteranceStarted = 0, previousFrameAt = 0, maxFrameGap = 0;
  uint32_t vadFrames = 0, voicedFrames = 0, shortReads = 0;
  int32_t raw[FRAME_BYTES / 2];
  int16_t pcm[FRAME_BYTES / 2];
  AudioFrame frame{};

  auto stopSession = [&](bool timedOut, const char* error) {
    session.stop();
    sessionEnabled.store(false);
    digitalWrite(LED_PIN, LOW);
    const uint32_t stoppedTurn = generation.fetch_add(1) + 1;
    requestedTurn.store(0);
    preRollNext = preRollCount = 0;
    awaitingAnnounced = detectorWasActive = false;
    voiceDetector.reset();
    Serial.println(timedOut ? "[SESSION] OFF: no speech for 30 seconds. Tap to restart."
                           : "[SESSION] OFF: tap to start a new conversation.");
  };

  while (true) {
    // Drain DMA even while off or replying. Speaker audio never enters the VAD
    // or the next utterance's pre-roll.
    size_t received = 0;
    const esp_err_t readStatus = i2s_channel_read(microphone.rxChan(), raw, sizeof(raw), &received, 30);
    const uint32_t now = millis();
    const int reading = digitalRead(BUTTON_TALK);
    if (reading != previous) changed = now;
    previous = reading;
    bool pressed = false;
    if (reading != stable && now - changed >= 30) {
      stable = reading;
      pressed = stable == LOW;
    }

    if (session.enabled() && audioFault.load() == generation.load()) {
      const char* error = "Audio stopped. Tap to restart.";
      switch (faultReason.load()) {
        case FaultReason::Asr: error = "No speech recognized. Tap to restart."; break;
        case FaultReason::Model: error = "No answer received. Tap to restart."; break;
        case FaultReason::Tts: error = "Speech failed. Tap to restart."; break;
        default: break;
      }
      stopSession(false, error);
      continue;
    }
    if (readStatus != ESP_OK && readStatus != ESP_ERR_TIMEOUT) {
      if (session.enabled()) stopSession(false, "Microphone error. Tap to restart.");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    const bool available = networkReady.load() && WiFi.status() == WL_CONNECTED;
    // Read the request first: main reserves busy before consuming that request.
    const bool canStart = !requestedTurn.load() && !pipelineBusy.load() && !pendingAudio.load();
    const bool detect = session.enabled() && (session.utteranceActive() || canStart);
    if (detect != detectorWasActive) {
      voiceDetector.reset();
      preRollNext = preRollCount = 0;
      detectorWasActive = detect;
    }
    memset(pcm, 0, sizeof(pcm));
    const size_t samples = min(received / sizeof(int32_t), FRAME_BYTES / 2);
    for (size_t i = 0; i < samples; ++i) {
      // INMP441 sends signed 24-bit samples left-aligned in 32-bit slots.
      // Preserve full scale; >>14 applied 4x gain and clipped speech/noise.
      const int32_t sample = raw[i] >> 16;
      pcm[i] = static_cast<int16_t>(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
    }
    frame.length = FRAME_BYTES;
    memcpy(frame.pcm, pcm, sizeof(pcm));
    const bool speech = detect && voiceDetector.speech(pcm, FRAME_BYTES / 2);
    if (detect) {
      if (speech) lastSpeechAt.store(now);
      if (session.utteranceActive()) {
        ++vadFrames;
        if (speech) ++voicedFrames;
        if (received != sizeof(raw)) ++shortReads;
        const uint32_t gap = now - previousFrameAt;
        if (gap > maxFrameGap) maxFrameGap = gap;
      }
      previousFrameAt = now;
    }
    if (detect && !session.utteranceActive()) {
      preRoll[preRollNext] = frame;
      preRollNext = (preRollNext + 1) % PRE_ROLL_FRAMES;
      if (preRollCount < PRE_ROLL_FRAMES) ++preRollCount;
    }

    const auto result = session.update(now, pressed, available, canStart, speech,
                                       serverEndpoint.load() == turn);
    if (result.has(Action::StopSession)) {
      stopSession(result.has(Action::Timeout), available ? nullptr : "Wi-Fi lost. Reconnect and tap.");
      continue;
    }
    if (result.has(Action::StartSession)) {
      const uint32_t startedTurn = generation.fetch_add(1) + 1;
      sessionEpoch.store(startedTurn);
      requestedTurn.store(0);
      sessionEnabled.store(true);
      digitalWrite(LED_PIN, HIGH);
      preRollNext = preRollCount = 0;
      voiceDetector.reset();
      detectorWasActive = false;
      awaitingAnnounced = canStart;
      Serial.println("[SESSION] ON: speak naturally; tap again to stop. Silence timeout: 30 seconds.");
    }
    if (!result.enabled) continue;

    bool queued = true;
    if (result.has(Action::StartUtterance)) {
      turn = generation.fetch_add(1) + 1;
      utteranceStarted = now;
      utteranceEndedAt.store(0);
      serverEndpoint.store(0);
      vadFrames = voicedFrames = shortReads = maxFrameGap = 0;
      faultReason.store(FaultReason::Audio);
      awaitingAnnounced = false;
      xQueueReset(micQueue);  // Previous pipeline is idle before new speech can start.
      for (size_t i = 0; i < preRollCount && queued; ++i) {
        AudioFrame& buffered = preRoll[(preRollNext + PRE_ROLL_FRAMES - preRollCount + i) % PRE_ROLL_FRAMES];
        buffered.generation = turn;
        queued = xQueueSend(micQueue, &buffered, 0) == pdTRUE;
      }
      preRollNext = preRollCount = 0;
      // Publish only after pre-roll is queued; this also reserves the pipeline.
      if (queued) requestedTurn.store(turn);
      Serial.println("[MIC] Speech detected; recording until a pause.");
    } else if (result.utteranceActive || result.has(Action::EndUtterance)) {
      frame.generation = turn;
      queued = xQueueSend(micQueue, &frame, 0) == pdTRUE;
    }
    if (!queued) {
      audioFault.store(turn);
      stopSession(false, "Audio queue full. Tap to restart.");
      continue;
    }
    if (result.has(Action::EndUtterance)) {
      utteranceEndedAt.store(now);
      recordingEnded.store(turn);  // The final PCM frame is already queued.
      Serial.printf("[MIC] Endpoint (%s) after %lu ms; silence=%lu ms voiced=%lu/%lu short=%lu max_gap=%lu ms.\n",
          serverEndpoint.load() == turn ? "ASR" : "local",
          static_cast<unsigned long>(now - utteranceStarted),
          static_cast<unsigned long>(now - lastSpeechAt.load()),
          static_cast<unsigned long>(voicedFrames), static_cast<unsigned long>(vadFrames),
          static_cast<unsigned long>(shortReads), static_cast<unsigned long>(maxFrameGap));
    } else if (result.listening && !result.utteranceActive && !awaitingAnnounced) {
      awaitingAnnounced = true;
      Serial.println("[SESSION] Listening for the next utterance.");
    }
    if (!result.listening) awaitingAnnounced = false;
  }
}

void playbackTask(void*) {
  AudioFrame frame{};
  int16_t stereo[FRAME_BYTES];
  while (true) {
    if (xQueueReceive(spkQueue, &frame, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    if (active(frame.generation)) {
      for (size_t i = 0; i < frame.length / 2; ++i) {
        int16_t sample = static_cast<int16_t>(frame.pcm[2 * i] | (frame.pcm[2 * i + 1] << 8));
        stereo[2 * i] = stereo[2 * i + 1] = static_cast<int16_t>(static_cast<int32_t>(sample) * 60 / 100);
      }
      size_t length = frame.length * 2, offset = 0;
      while (offset < length && active(frame.generation)) {
        size_t written = 0;
        esp_err_t status = i2s_channel_write(speaker.txChan(),
            reinterpret_cast<uint8_t*>(stereo) + offset, length - offset, &written, 50);
        offset += written;
        if ((status != ESP_OK && status != ESP_ERR_TIMEOUT) || !written) {
          audioFault.store(frame.generation);
          break;
        }
      }
    }
    pendingAudio.fetch_sub(1);
  }
}

bool enqueueSpeech(const uint8_t* pcm, size_t length, uint32_t turn) {
  if (length % 2) return false;
  AudioFrame frame{};
  frame.generation = turn;
  while (length && active(turn)) {
    frame.length = static_cast<uint16_t>(min(length, FRAME_BYTES));
    memcpy(frame.pcm, pcm, frame.length);
    uint32_t started = millis();
    pendingAudio.fetch_add(1);
    while (xQueueSend(spkQueue, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
      if (!active(turn) || millis() - started > 2000) {
        pendingAudio.fetch_sub(1);
        return false;
      }
    }
    pcm += frame.length;
    length -= frame.length;
  }
  return length == 0;
}

String recognize(uint32_t turn) {
  if (!active(turn)) return "";
  String headers = String("X-Api-App-Key: ") + BYTEPLUS_ASR_APP_ID + "\r\nX-Api-Access-Key: " +
      BYTEPLUS_ASR_ACCESS_TOKEN + "\r\nX-Api-Resource-Id: volc.bigasr.sauc.duration\r\nX-Api-Request-Id: " + uuid() + "\r\n";
  if (!cloud.connect(VOICE_HOST, "/api/v3/sauc/bigmodel_nostream", headers, BYTEPLUS_ROOT_CA)) {
    Serial.printf("[ASR] Connection failed: %s\n", cloud.lastError());
    return "";
  }
  if (!active(turn)) return "";
  JsonDocument request;
  request["user"]["uid"] = "esp32-voicebot";
  auto audio = request["audio"].to<JsonObject>();
  audio["format"] = "pcm"; audio["codec"] = "raw";
  audio["rate"] = SAMPLE_RATE; audio["bits"] = 16; audio["channel"] = 1;
  audio["language"] = strcmp(BYTEPLUS_LANGUAGE, "th") == 0 ? "th-TH" : "en-US";
  auto options = request["request"].to<JsonObject>();
  options["model_name"] = "bigmodel"; options["enable_itn"] = true;
  options["enable_punc"] = true; options["show_utterances"] = true; options["result_type"] = "full";
  // The multilingual endpoint supports incremental finalized utterances with
  // this mode. Its spectral endpoint can stop a turn even if local noise keeps
  // WebRTC VAD positive. Keep the Thai endpoint: bigmodel_async ignores th-TH.
  options["enable_nonstream"] = true;
  options["end_window_size"] = 400;
  options["force_to_speech_time"] = 100;
  String json;
  serializeJson(request, json);
  int32_t sequence = 1;
  size_t encoded = byteplus::asr::buildRequest(outbound, sizeof(outbound),
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length(), sequence++, true, false);
  if (!active(turn) || !encoded || !cloud.sendBinary(outbound, encoded)) {
    Serial.printf("[ASR] Request failed: %s\n", cloud.lastError());
    return "";
  }
  bool finalSent = false, finalReceived = false, failed = false;
  uint32_t started = millis(), finalizedAt = 0;
  String transcript;
  size_t buffered = 0;
  size_t micBytes = 0, nonzeroSamples = 0, clippedSamples = 0, responses = 0;
  int32_t peak = 0;
  AudioFrame frame{};
  while (active(turn) && !failed && !finalReceived && millis() - started < MAX_RECORD_MS + 15000) {
    cloud.poll([&](const uint8_t* data, size_t length) {
      ++responses;
      byteplus::asr::Frame response;
      if (!byteplus::asr::parse(data, length, response) || response.compression != 0 || response.type == 15) {
        failed = true;
        Serial.println("[ASR] Rejected response or provider error.");
        return;
      }
      if (response.payloadLength > 16384) { failed = true; return; }
      JsonDocument result;
      if (deserializeJson(result, response.payload, response.payloadLength)) { failed = true; return; }
      const char* text = result["result"]["text"] | "";
      if (strlen(text) > 2048) { failed = true; return; }
      if (*text) transcript = text;
      const JsonArray utterances = result["result"]["utterances"].as<JsonArray>();
      if (utterances.size()) {
        const auto last = utterances[utterances.size() - 1];
        const char* utteranceText = last["text"] | "";
        if (active(turn) && *utteranceText && (last["definite"] | false)) {
          if (serverEndpoint.exchange(turn) != turn) {
            Serial.println("[ASR] Finalized utterance; ending capture now.");
          }
        }
      }
      if (response.final) finalReceived = true;
    });
    if (!cloud.connected() && !finalReceived) break;
    // Preserve 20 ms capture frames, batch approximately 200 ms per cloud message.
    while (!finalSent && buffered < ASR_BATCH_BYTES && xQueueReceive(micQueue, &frame, 0) == pdTRUE) {
      if (frame.generation != turn) continue;
      micBytes += frame.length;
      for (size_t i = 0; i < frame.length; i += 2) {
        int32_t sample = static_cast<int16_t>(frame.pcm[i] | (frame.pcm[i + 1] << 8));
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
        if (magnitude) ++nonzeroSamples;
        if (sample == 32767 || sample == -32768) ++clippedSamples;
      }
      memcpy(outbound + 12 + buffered, frame.pcm, frame.length);
      buffered += frame.length;
    }
    const bool ended = recordingEnded.load() == turn && uxQueueMessagesWaiting(micQueue) == 0;
    if (!active(turn)) break;
    if (!finalSent && (buffered >= ASR_BATCH_BYTES || ended)) {
      encoded = byteplus::asr::buildRequest(outbound, sizeof(outbound), outbound + 12,
                                          buffered, sequence++, false, ended);
      if (!encoded || !cloud.sendBinary(outbound, encoded)) break;
      buffered = 0;
      if (ended) {
        finalSent = true;
        finalizedAt = millis();
        Serial.printf("[LATENCY] ASR final sent %lu ms after endpoint.\n",
            static_cast<unsigned long>(finalizedAt - utteranceEndedAt.load()));
      }
    }
    if (finalSent && millis() - finalizedAt > 12000) break;
    delay(1);
  }
  Serial.printf("[ASR] Mic bytes=%u peak=%ld nonzero=%u clipped=%u responses=%u final=%d socket=%s\n",
      static_cast<unsigned>(micBytes), static_cast<long>(peak), static_cast<unsigned>(nonzeroSamples),
      static_cast<unsigned>(clippedSamples), static_cast<unsigned>(responses), finalReceived, cloud.lastError());
  cloud.close();
  if (!active(turn) || failed || !finalSent || !finalReceived) return "";
  Serial.printf("[LATENCY] ASR final received %lu ms after endpoint.\n",
      static_cast<unsigned long>(millis() - utteranceEndedAt.load()));
  return transcript;
}

bool sendEvent(uint32_t event, const String& session, const String& json) {
  size_t length = byteplus::tts::buildEvent(outbound, sizeof(outbound), event, session.c_str(),
      reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
  return length && cloud.sendBinary(outbound, length);
}

// Owned by loop(), just like the ASR socket. Model reads and TTS polling share
// this task; microphone/button and I2S playback continue in their own tasks.
class TtsStream {
 public:
  bool begin(uint32_t turn) {
    turn_ = turn;
    session_ = uuid();
    failed_ = finished_ = finishing_ = false;
    stage_ = 0;
    audioBytes_ = 0;
    started_ = lastResponse_ = millis();
    if (!active(turn_)) return false;
    String headers = String("X-Api-App-Key: ") + BYTEPLUS_TTS_APP_ID + "\r\nX-Api-Access-Key: " +
        BYTEPLUS_TTS_TOKEN + "\r\nX-Api-Resource-Id: " + BYTEPLUS_TTS_RESOURCE_ID +
        "\r\nX-Api-Connect-Id: " + uuid() + "\r\n";
    if (!cloud.connect(VOICE_HOST, "/api/v3/tts/bidirection", headers, BYTEPLUS_ROOT_CA)) return false;
    request_.clear();
    request_["user"]["uid"] = "esp32-voicebot";
    request_["namespace"] = "BidirectionalTTS";
    request_["event"] = 100;
    request_["req_params"]["speaker"] = BYTEPLUS_TTS_VOICE;
    request_["req_params"]["audio_params"]["format"] = "pcm";
    request_["req_params"]["audio_params"]["sample_rate"] = SAMPLE_RATE;
    String startJson;
    serializeJson(request_, startJson);
    startJson_ = startJson;
    if (!active(turn_) || !sendEvent(1, "", "{}")) return false;
    while (active(turn_) && stage_ < 2 && pump()) delay(1);
    return active(turn_) && !failed_ && stage_ == 2;
  }

  bool text(const char* value, size_t length) {
    if (!active(turn_) || failed_ || finished_ || finishing_ || stage_ != 2) return false;
    if (!length) return true;
    request_["event"] = 200;
    request_["req_params"]["text"] = String(value, length);
    String json;
    serializeJson(request_, json);
    if (!active(turn_) || !sendEvent(200, session_, json)) {
      failed_ = true;
      return false;
    }
    return pump();
  }

  bool finishText() {
    if (!active(turn_) || failed_ || finishing_ || stage_ != 2) return false;
    finishing_ = true;
    return sendEvent(102, session_, "{}");
  }

  bool pump() {
    if (!active(turn_) || failed_) return false;
    if (finished_) return true;
    if (millis() - started_ > 60000 || millis() - lastResponse_ > 15000) {
      failed_ = true;
      return false;
    }
    cloud.poll([&](const uint8_t* data, size_t length) {
      if (!active(turn_)) { failed_ = true; return; }
      byteplus::tts::Frame response;
      if (!byteplus::tts::parse(data, length, response) || response.compression != 0 || response.type == 15) {
        failed_ = true;
        return;
      }
      lastResponse_ = millis();
      if (response.event >= 100 &&
          (response.sessionIdLength != session_.length() || memcmp(response.sessionId, session_.c_str(), session_.length()))) {
        failed_ = true;
        return;
      }
      if (response.event == 50 && stage_ == 0) {
        stage_ = 1;
        if (!sendEvent(100, session_, startJson_)) failed_ = true;
      } else if (response.event == 150 && stage_ == 1) {
        stage_ = 2;
      } else if (response.event == 352 && stage_ == 2) {
        if (response.type != 11 || response.serialization != 0) { failed_ = true; return; }
        if (!audioBytes_ && response.payloadLength) {
          Serial.printf("[LATENCY] First TTS PCM %lu ms after endpoint; %lu ms since last detected speech.\n",
              static_cast<unsigned long>(millis() - utteranceEndedAt.load()),
              static_cast<unsigned long>(millis() - lastSpeechAt.load()));
        }
        if (!enqueueSpeech(response.payload, response.payloadLength, turn_)) failed_ = true;
        audioBytes_ += response.payloadLength;
      } else if (response.event == 152 && stage_ == 2) {
        if (!finishing_) failed_ = true;
        else finished_ = true;
      } else if (response.event == 51 || response.event == 153 || response.event == 151) {
        failed_ = true;
      }
    });
    if (!cloud.connected() && !finished_) failed_ = true;
    return !failed_ && active(turn_);
  }

  bool drain() {
    while (!finished_ && pump()) delay(1);
    const uint32_t drainStarted = millis();
    while (active(turn_) && !failed_ && pendingAudio.load() && millis() - drainStarted < 5000) delay(5);
    Serial.printf("[TTS] Received %u PCM bytes.\n", static_cast<unsigned>(audioBytes_));
    return active(turn_) && !failed_ && finished_ && audioBytes_ && !pendingAudio.load();
  }

  void close() {
    if (cloud.connected()) {
      if (!finished_ && stage_) sendEvent(101, session_, "{}");
      sendEvent(2, "", "{}");
    }
    cloud.close();
    request_.clear();
    startJson_ = "";
  }

 private:
  JsonDocument request_;
  String session_, startJson_;
  uint32_t turn_ = 0, started_ = 0, lastResponse_ = 0;
  size_t audioBytes_ = 0;
  uint8_t stage_ = 0;
  bool failed_ = false, finished_ = false, finishing_ = false;
};

TtsStream tts;
// Fixed-size parsers live outside the small Arduino loop stack.
byteplus::stream::HttpBodyDecoder modelBody;
byteplus::stream::SseParser<4096> modelEvents;
byteplus::stream::TextChunker<512> modelChunks;

void clearHistory() {
  for (size_t i = 0; i < HISTORY_TURNS; ++i) {
    historyUsers[i] = "";
    historyReplies[i] = "";
  }
  historyCount = 0;
}

void rememberReply(const String& transcript, const String& reply) {
  if (historyCount == HISTORY_TURNS) {
    for (size_t i = 1; i < HISTORY_TURNS; ++i) {
      historyUsers[i - 1] = historyUsers[i];
      historyReplies[i - 1] = historyReplies[i];
    }
    --historyCount;
  }
  historyUsers[historyCount] = transcript;
  historyReplies[historyCount++] = reply;
}

bool streamReply(const String& transcript, uint32_t turn, String& answer, FaultReason& failure) {
  if (!active(turn)) return false;
  failure = FaultReason::Tts;
  if (!tts.begin(turn)) { tts.close(); return false; }
  failure = FaultReason::Model;
  Serial.println("[LLM] Streaming answer into TTS...");
  WiFiClientSecure client;
  client.setCACert(BYTEPLUS_ROOT_CA);
  client.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(15000);
  http.setReuse(false);
  if (!http.begin(client, ARK_URL)) { tts.close(); return false; }
  const char* responseHeaders[] = {"Transfer-Encoding", "Content-Type", "Content-Length", "Content-Encoding"};
  http.collectHeaders(responseHeaders, 4);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "text/event-stream");
  http.addHeader("Authorization", String("Bearer ") + BYTEPLUS_ARK_API_KEY);
  JsonDocument request;
  request["model"] = BYTEPLUS_ARK_ENDPOINT_ID;
  request["stream"] = true;
  request["max_tokens"] = 192;
  request["thinking"]["type"] = "disabled";
  JsonArray messages = request["messages"].to<JsonArray>();
  auto system = messages.add<JsonObject>();
  system["role"] = "system";
  system["content"] = strcmp(BYTEPLUS_LANGUAGE, "th") == 0
      ? "คุณเป็นผู้ช่วยเสียงที่สุภาพ ตอบภาษาไทยสั้นๆ หนึ่งถึงสองประโยค ไม่ใช้มาร์กดาวน์"
      : "You are a helpful voice assistant. Reply in English in one or two short sentences without markdown.";
  for (size_t i = 0; i < historyCount; ++i) {
    auto previousUser = messages.add<JsonObject>();
    previousUser["role"] = "user";
    previousUser["content"] = historyUsers[i];
    auto previousReply = messages.add<JsonObject>();
    previousReply["role"] = "assistant";
    previousReply["content"] = historyReplies[i];
  }
  auto user = messages.add<JsonObject>();
  user["role"] = "user";
  user["content"] = transcript;
  String body;
  serializeJson(request, body);
  if (!active(turn)) { http.end(); tts.close(); return false; }
  int status = http.POST(body);
  if (status != HTTP_CODE_OK || !http.header("Content-Type").startsWith("text/event-stream")) {
    Serial.printf("[LLM] HTTP status %d\n", status);
    http.end();
    tts.close();
    return false;
  }
  using byteplus::stream::BodyMode;
  const String encoding = http.header("Transfer-Encoding");
  if ((encoding.length() && (!encoding.equalsIgnoreCase("chunked") || http.hasHeader("Content-Length"))) ||
      (http.hasHeader("Content-Encoding") && !http.header("Content-Encoding").equalsIgnoreCase("identity"))) {
    http.end(); tts.close(); return false;
  }
  if (!modelBody.reset(encoding.length() ? BodyMode::Chunked :
      (http.getSize() >= 0 ? BodyMode::ContentLength : BodyMode::UntilClose),
      http.getSize() >= 0 ? static_cast<uint32_t>(http.getSize()) : 0)) {
    http.end(); tts.close(); return false;
  }
  modelEvents.reset();
  modelChunks.reset();
  struct Context {
    uint32_t turn;
    String* answer;
    FaultReason* failure;
    bool done = false, finishSeen = false;
    uint32_t lastTextFlush = 0;
  } context{turn, &answer, &failure};
  context.lastTextFlush = millis();
  auto textSink = +[](const char* text, size_t length, void* opaque) -> bool {
    auto& state = *static_cast<Context*>(opaque);
    if (!active(state.turn)) return false;
    if (!tts.text(text, length)) { *state.failure = FaultReason::Tts; return false; }
    state.lastTextFlush = millis();
    return true;
  };
  // C callbacks keep the pure parsers independent of Arduino/network state.
  struct Sinks {
    Context* context;
    byteplus::stream::TextSink text;
  } sinks{&context, textSink};
  auto eventSink = +[](const char* event, size_t length, void* opaque) -> bool {
    auto& callbacks = *static_cast<Sinks*>(opaque);
    auto& state = *callbacks.context;
    if (!active(state.turn) || state.done) return false;
    if (length == 6 && memcmp(event, "[DONE]", 6) == 0) {
      state.done = state.finishSeen;
      return state.done;
    }
    JsonDocument result;
    if (deserializeJson(result, event, length) || !result["error"].isNull()) return false;
    const auto choice = result["choices"][0];
    const char* finish = choice["finish_reason"] | "";
    if (*finish) {
      if (strcmp(finish, "stop") && strcmp(finish, "length")) return false;
      state.finishSeen = true;
    }
    const char* delta = choice["delta"]["content"] | "";
    const size_t size = strlen(delta);
    if (!size) return true;
    if (size > 2048 - state.answer->length()) return false;
    if (!state.answer->length()) {
      Serial.printf("[LATENCY] First model text %lu ms after endpoint.\n",
          static_cast<unsigned long>(millis() - utteranceEndedAt.load()));
    }
    if (!state.answer->concat(delta, size)) return false;
    return modelChunks.feed(delta, size, callbacks.text, &state);
  };
  struct EventSink {
    Sinks* sinks;
    byteplus::stream::TextSink event;
  } events{&sinks, eventSink};
  auto bodySink = +[](const uint8_t* bytes, size_t length, void* opaque) -> bool {
    auto& callbacks = *static_cast<EventSink*>(opaque);
    return modelEvents.feed(bytes, length, callbacks.event, callbacks.sinks);
  };
  auto* stream = http.getStreamPtr();
  if (!stream) { http.end(); tts.close(); return false; }
  const uint32_t started = millis();
  uint32_t lastBytes = started;
  bool ok = true;
  uint8_t bytes[512];
  while (active(turn) && ok && !context.done && millis() - started < 45000 && millis() - lastBytes < 15000) {
    if (!tts.pump()) { failure = FaultReason::Tts; ok = false; break; }
    const int available = stream->available();
    if (available > 0) {
      const int count = stream->read(bytes, min(static_cast<size_t>(available), sizeof(bytes)));
      if (count <= 0) { ok = false; break; }
      lastBytes = millis();
      ok = modelBody.feed(bytes, count, bodySink, &events);
    } else if (!stream->connected() || modelBody.done()) {
      ok = modelBody.finish() && modelEvents.finish();
      break;
    }
    if (ok && modelChunks.pendingBytes() && millis() - context.lastTextFlush >= 250) {
      ok = modelChunks.flush(textSink, &context);
    }
    delay(1);
  }
  http.end();
  ok = active(turn) && ok && context.done && answer.length();
  if (ok) {
    Serial.printf("[LLM] Text complete %lu ms after endpoint.\n",
        static_cast<unsigned long>(millis() - utteranceEndedAt.load()));
    failure = FaultReason::Tts;
    ok = modelChunks.finish(textSink, &context) && tts.finishText() && tts.drain();
  }
  tts.close();
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nStandalone BytePlus ESP32 voicebot");
  if (!strlen(WIFI_SSID) || !strlen(BYTEPLUS_ASR_APP_ID) || !strlen(BYTEPLUS_ASR_ACCESS_TOKEN) ||
      !strlen(BYTEPLUS_TTS_APP_ID) || !strlen(BYTEPLUS_TTS_TOKEN) || !strlen(BYTEPLUS_ARK_API_KEY) || !strlen(BYTEPLUS_ARK_ENDPOINT_ID)) {
    Serial.println("[CONFIG] Fill config.local.h, then upload again.");
    return;
  }
  pinMode(BUTTON_TALK, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  microphone.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN, -1);
  if (!microphone.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT) ||
      !speaker.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[I2S] Initialization failed; check board and pins.");
    return;
  }
  if (!voiceDetector.begin()) {
    Serial.println("[VAD] Speech detector initialization failed.");
    return;
  }
  micQueue = makeAudioQueue(MIC_QUEUE_FRAMES, &micQueueState);
  spkQueue = makeAudioQueue(SPK_QUEUE_FRAMES, &spkQueueState);
  if (!micQueue || !spkQueue) {
    Serial.println("[MEMORY] Audio queues could not be allocated; enable PSRAM if available.");
    return;
  }
  if (xTaskCreate(captureTask, "mic", 8192, nullptr, 2, nullptr) != pdPASS ||
      xTaskCreate(playbackTask, "speaker", 6144, nullptr, 2, nullptr) != pdPASS) {
    Serial.println("[MEMORY] Audio tasks could not start.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  ready = true;
}

void loop() {
  static uint32_t lastNotice = 0, historyEpoch = 0;
  static bool announcedReady = false;
  if (!ready) { delay(100); return; }
  if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) {
    networkReady.store(false);
    announcedReady = false;
    if (millis() - lastNotice > 5000) {
      Serial.println("[WAIT] Waiting for Wi-Fi and clock synchronization for verified TLS.");
      lastNotice = millis();
    }
    delay(20);
    return;
  }
  if (!announcedReady) {
    Serial.println("[READY] Wi-Fi and clock synchronized. Tap to start a conversation.");
    announcedReady = true;
  }
  networkReady.store(true);
  const uint32_t currentSession = sessionEpoch.load();
  if (historyEpoch != currentSession || (historyCount && !sessionEnabled.load())) {
    clearHistory();
    historyEpoch = currentSession;
  }
  uint32_t turn = requestedTurn.load();
  if (!turn) { delay(5); return; }
  // Reserve before removing the request, so capture never sees a free pipeline
  // between the queue handoff and the synchronous cloud calls.
  pipelineBusy.store(true);
  if (!requestedTurn.compare_exchange_strong(turn, 0)) {
    pipelineBusy.store(false);
    return;
  }
  const uint32_t requestSession = sessionEpoch.load();
  if (historyEpoch != requestSession) {
    clearHistory();
    historyEpoch = requestSession;
  }
  if (!active(turn)) { pipelineBusy.store(false); return; }
  const uint32_t started = millis();
  bool completed = false;
  FaultReason failure = FaultReason::Asr;
  String transcript = recognize(turn);
  String reply;
  if (transcript.length() && active(turn)) {
    Serial.printf("[USER] %s\n", transcript.c_str());
    failure = FaultReason::Model;
    completed = streamReply(transcript, turn, reply, failure);
    if (completed && active(turn)) {
      Serial.printf("[BOT] %s\n", reply.c_str());
    }
  }
  cloud.close();
  if (generation.load() == turn && sessionEnabled.load()) {
    if (completed) rememberReply(transcript, reply);
    else {
      faultReason.store(failure);
      audioFault.store(turn);  // Capture stops the session and updates the TFT.
      Serial.println("[ERROR] Voice request failed; tap to start again.");
    }
  }
  // Let cancelled software buffers and the I2S DMA/acoustic tail finish before
  // opening a new listening window. Capture still handles the stop button.
  const uint32_t drainStarted = millis();
  while (pendingAudio.load() && millis() - drainStarted < 2000) delay(5);
  delay(250);
  Serial.printf("[TURN] Completed in %lu ms.\n", static_cast<unsigned long>(millis() - started));
  pipelineBusy.store(false);  // Capture owns the next AwaitingSpeech transition.
}
