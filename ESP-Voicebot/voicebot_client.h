#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>
#include "src/cloud_websockets/WebSocketsClient.h"

// All methods and callbacks run on the Arduino loop task. Audio tasks exchange
// PCM and playback notifications through queues; they must never call the socket.
class VoicebotClient : private WebSocketsClient {
 public:
  using TtsAudioCallback = std::function<void(const uint8_t* pcm, size_t length)>;
  using TtsAudioCapacityCallback = std::function<size_t()>;
  using OpenedCallback = std::function<void(const String& sessionId)>;
  using UserSttCallback = std::function<void(const String& text, bool isFinal)>;
  using BotReplyCallback = std::function<void(const String& text)>;
  using BargeInCallback = std::function<void()>;
  using DisconnectCallback = std::function<void(const String& info)>;

  VoicebotClient() {
    onEvent([this](WStype_t type, uint8_t* bytes, size_t length) {
      handleEvent(type, bytes, length);
    });
  }

  ~VoicebotClient() { stop(); }

  void setTtsAudioCallback(TtsAudioCallback cb) { onTtsAudio_ = std::move(cb); }
  // Return writable PCM bytes. Zero pauses TCP consumption until playback frees
  // space, so a server audio burst never needs a frame-sized heap allocation.
  void setTtsAudioCapacityCallback(TtsAudioCapacityCallback cb) {
    setBinaryReceiveCapacity(std::move(cb));
  }
  void setOpenedCallback(OpenedCallback cb) { onOpened_ = std::move(cb); }
  void setUserSttCallback(UserSttCallback cb) { onUserStt_ = std::move(cb); }
  void setBotReplyCallback(BotReplyCallback cb) { onBotReply_ = std::move(cb); }
  void setBargeInCallback(BargeInCallback cb) { onBargeIn_ = std::move(cb); }
  void setDisconnectCallback(DisconnectCallback cb) { onDisconnect_ = std::move(cb); }

  bool start(const char* host, uint16_t port, const char* path,
             const char* apiKey, const char* agentId, const char* ca) {
    stop();
    if (!validHost(host) || !port || !path || path[0] != '/' ||
        strnlen(path, 513) > 512 || strchr(path, '\r') || strchr(path, '\n') ||
        !apiKey || !*apiKey || strnlen(apiKey, 513) > 512 ||
        !agentId || !*agentId || strnlen(agentId, 129) > 128) {
      Serial.println("[Voicebot] Invalid endpoint or missing credentials.");
      return false;
    }
    // A missing/broken CA must fail closed; never silently call setInsecure().
    // The CA storage must outlive the connection (tls_roots.h is static flash).
    if (!ca || !strstr(ca, "-----BEGIN CERTIFICATE-----") ||
        !strstr(ca, "-----END CERTIFICATE-----")) {
      Serial.println("[Voicebot] A valid TLS root certificate is required.");
      return false;
    }

    String urlPath;
    const size_t capacity = strlen(path) + 3 * (strlen(apiKey) + strlen(agentId)) + 24;
    if (!urlPath.reserve(capacity)) return false;
    urlPath += path;
    urlPath += strchr(path, '?') ? "&api_key=" : "?api_key=";
    appendQueryValue(urlPath, apiKey);
    urlPath += "&agent_id=";
    appendQueryValue(urlPath, agentId);
    // Query strings contain the API key. Do not print the request URL.
    Serial.printf("[Voicebot] Connecting to %s:%u with verified TLS.\n", host, port);
    setExtraHeaders("User-Agent: ESP32-Voicebot\r\nOrigin: https://voicebot-stg.botnoigroup.com");
    beginSslWithCA(host, port, urlPath.c_str(), ca, "");
    setReconnectInterval(5000);
    running_ = true;
    disconnectNotified_ = false;
    return true;
  }

  void stop() {
    // disconnect() alone leaves the underlying reconnect timer enabled.
    running_ = false;
    disconnectNotified_ = true;
    clearSession();
    WebSocketsClient::disconnect();
  }

  void loop() {
    if (!running_) return;
    WebSocketsClient::loop();
    const uint32_t now = millis();
    if (awaitingOpened_ && static_cast<uint32_t>(now - connectedAt_) >= kOpenedTimeoutMs) {
      failConnection("Timed out waiting for session opened", true);
      return;
    }
    if (opened_ && static_cast<uint32_t>(now - lastPing_) >= kPingIntervalMs && canSendNow()) {
      if (sendPing()) lastPing_ = now;
    }
  }

  bool isOpened() { return running_ && opened_ && WebSocketsClient::isConnected(); }
  // Advisory zero-timeout readiness; a failed/partial TLS send still closes.
  bool canSendNow() const { return running_ && opened_ && WebSocketsClient::canSendNow(); }
  bool isReceivingAudio() const { return opened_ && isReceivingBinary(); }
  String getSessionId() const { return sessionId_; }

  bool sendAudioFrame(const uint8_t* pcm, size_t length) {
    // Microphone packets are 20 ms PCM16 mono, 640 bytes at 16 kHz. The same
    // method sends the loop's paced end-of-speech silence packets.
    if (!isOpened() || !pcm || !length || length > 640 || (length & 1)) return false;
    if (sendBIN(pcm, length)) return true;
    failConnection("Audio send failed", true);
    return false;
  }

  bool sendPing() { return sendJson("ping"); }
  bool sendPlaybackStarted() { return sendJson("playback_started"); }
  bool sendPlaybackCompleted() { return sendJson("playback_completed"); }

 private:
  static constexpr size_t kMaxJsonBytes = 8192;
  static constexpr size_t kJsonArenaBytes = 8192;
  static constexpr size_t kMaxSessionIdBytes = 128;
  static constexpr size_t kMaxDisplayTextBytes = 2048;
  static constexpr size_t kMaxLogTextBytes = 256;
  static constexpr size_t kLogBudgetPerMessage = 512;
  static constexpr uint32_t kOpenedTimeoutMs = 15000;
  static constexpr uint32_t kPingIntervalMs = 20000;

  // ArduinoJson 7's StaticJsonDocument is also heap-backed. Give it a real
  // fixed arena instead. Each document has a synchronous, message-local life;
  // memory is reclaimed together before parsing the next message. The most recent
  // allocation can grow/shrink in place; other frees wait until the next reset.
  class JsonArena : public ArduinoJson::Allocator {
   public:
    void reset() { used_ = 0; }
    void* allocate(size_t bytes) override {
      if (!bytes || bytes > kJsonArenaBytes - sizeof(Header)) return nullptr;
      const size_t aligned = alignSize(bytes);
      if (used_ > kJsonArenaBytes - sizeof(Header) ||
          aligned > kJsonArenaBytes - sizeof(Header) - used_) return nullptr;
      auto* header = reinterpret_cast<Header*>(storage_ + used_);
      header->bytes = bytes;
      used_ += sizeof(Header) + aligned;
      return header + 1;
    }
    void deallocate(void* pointer) override {
      if (!pointer) return;
      auto* header = static_cast<Header*>(pointer) - 1;
      const size_t offset = reinterpret_cast<uint8_t*>(header) - storage_;
      if (offset + sizeof(Header) + alignSize(header->bytes) == used_) used_ = offset;
    }
    void* reallocate(void* pointer, size_t bytes) override {
      if (!pointer) return allocate(bytes);
      if (!bytes) {
        deallocate(pointer);
        return nullptr;
      }
      auto* header = static_cast<Header*>(pointer) - 1;
      const size_t offset = reinterpret_cast<uint8_t*>(header) - storage_;
      if (offset + sizeof(Header) + alignSize(header->bytes) == used_) {
        if (bytes > kJsonArenaBytes - sizeof(Header) - offset ||
            alignSize(bytes) > kJsonArenaBytes - sizeof(Header) - offset) return nullptr;
        header->bytes = bytes;
        used_ = offset + sizeof(Header) + alignSize(bytes);
        return pointer;
      }
      if (bytes <= header->bytes) return pointer;
      void* replacement = allocate(bytes);
      if (replacement) memcpy(replacement, pointer, header->bytes);
      return replacement;
    }

   private:
    struct alignas(std::max_align_t) Header { size_t bytes; };
    static size_t alignSize(size_t bytes) {
      return (bytes + alignof(Header) - 1) & ~(alignof(Header) - 1);
    }
    alignas(std::max_align_t) uint8_t storage_[kJsonArenaBytes];
    size_t used_ = 0;
  } jsonArena_;

  bool running_ = false;
  bool opened_ = false;
  bool awaitingOpened_ = false;
  bool disconnectNotified_ = true;
  uint32_t seq_ = 0;
  String sessionId_;
  uint32_t lastPing_ = 0;
  uint32_t connectedAt_ = 0;

  TtsAudioCallback onTtsAudio_;
  OpenedCallback onOpened_;
  UserSttCallback onUserStt_;
  BotReplyCallback onBotReply_;
  BargeInCallback onBargeIn_;
  DisconnectCallback onDisconnect_;

  static bool validHost(const char* host) {
    if (!host || !*host || strnlen(host, 254) > 253) return false;
    for (const char* p = host; *p; ++p) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    }
    return true;
  }

  static void appendQueryValue(String& out, const char* value) {
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
      const unsigned char c = *p;
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        out += static_cast<char>(c);
      } else {
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
      }
    }
  }

  static bool validSessionId(const char* id) {
    if (!id || !*id || strnlen(id, kMaxSessionIdBytes + 1) > kMaxSessionIdBytes) return false;
    for (const char* p = id; *p; ++p) {
      // These protocol identifiers need no escaping in the fixed JSON envelope.
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' ||
            *p == '_' || *p == ':')) return false;
    }
    return true;
  }

  static size_t textPrefixLength(const char* text, size_t limit) {
    size_t length = strnlen(text, limit);
    // Keep Thai and other UTF-8 text intact when truncating diagnostics/callbacks.
    if (length == limit) {
      while (length && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80) --length;
    }
    return length;
  }

  void clearSession() {
    opened_ = false;
    awaitingOpened_ = false;
    sessionId_ = "";
    seq_ = 0;
    connectedAt_ = 0;
    lastPing_ = 0;
  }

  void failConnection(const char* reason, bool closeSocket) {
    const bool notify = running_ && !disconnectNotified_;
    disconnectNotified_ = true;
    clearSession();
    // Clear first: disconnect() can synchronously emit WStype_DISCONNECTED.
    if (closeSocket) WebSocketsClient::disconnect();
    if (notify) {
      Serial.printf("[Voicebot] %s.\n", reason);
      if (onDisconnect_) onDisconnect_(String(reason));
    }
  }

  bool sendJson(const char* type) {
    if (!isOpened()) return false;
    if (seq_ == UINT32_MAX) {
      failConnection("Session sequence exhausted", true);
      return false;
    }
    const uint32_t nextSeq = seq_ + 1;
    char envelope[256];
    const int length = snprintf(envelope, sizeof(envelope),
        "{\"version\":\"2\",\"type\":\"%s\",\"seq\":%lu,\"id\":\"%s\",\"parameters\":{}}",
        type, static_cast<unsigned long>(nextSeq), sessionId_.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(envelope)) return false;
    if (!sendTXT(envelope, static_cast<size_t>(length))) {
      failConnection("Control send failed", true);
      return false;
    }
    seq_ = nextSeq;
    return true;
  }

  void handleEvent(WStype_t type, uint8_t* bytes, size_t length) {
    if (!running_) return;
    switch (type) {
      case WStype_CONNECTED:
        clearSession();
        disconnectNotified_ = false;
        awaitingOpened_ = true;
        connectedAt_ = millis();
        Serial.println("[Voicebot] TLS connected; waiting for session opened.");
        break;
      case WStype_DISCONNECTED:
        // Transport reasons may contain a request URL: use a safe fixed message.
        failConnection("WebSocket disconnected", false);
        break;
      case WStype_ERROR:
        failConnection("WebSocket error", true);
        break;
      case WStype_BIN:
        // The bounded transport emits chunks for both ordinary and fragmented
        // binary messages, including their final bytes. Text never reaches here.
        if (opened_ && onTtsAudio_ && bytes && length) onTtsAudio_(bytes, length);
        break;
      case WStype_TEXT:
        if (!bytes || !length || length > kMaxJsonBytes) {
          failConnection("Invalid JSON message size", true);
          break;
        }
        parseJson(bytes, length);
        break;
      default:
        break;
    }
  }

  void parseJson(uint8_t* bytes, size_t length) {
    jsonArena_.reset();
    JsonDocument doc(&jsonArena_);
    const DeserializationError error = deserializeJson(
        doc, bytes, length, DeserializationOption::NestingLimit(12));
    if (error || doc.overflowed() || !doc.is<JsonObject>()) {
      failConnection("Invalid JSON or JSON memory limit exceeded", true);
      return;
    }
    parseServerJson(doc);
  }

  void parseServerJson(const JsonDocument& doc) {
    const char* type = doc["type"] | "";
    if (strcmp(type, "opened") == 0) {
      const char* id = doc["id"] | "";
      const char* version = doc["version"] | "";
      if (!awaitingOpened_ || opened_ || strcmp(version, "2") || !validSessionId(id)) {
        failConnection("Invalid opened session", true);
        return;
      }
      // Playback and microphone hardware are configured for this format only.
      JsonArrayConst media = doc["parameters"]["media"].as<JsonArrayConst>();
      bool supportedAudio = false;
      for (JsonObjectConst entry : media) {
        if (strcmp(entry["type"] | "", "audio/L16") == 0 &&
            (entry["sampleRateHz"] | 0) == 16000 &&
            entry["channels"].as<JsonArrayConst>().size() == 1) {
          supportedAudio = true;
        }
      }
      if (!supportedAudio || (doc["parameters"]["startPaused"] | false)) {
        failConnection("Unsupported session audio format", true);
        return;
      }
      sessionId_ = id;
      if (sessionId_.length() != strlen(id)) {
        failConnection("Could not allocate session identifier", true);
        return;
      }
      seq_ = doc["clientseq"] | static_cast<uint32_t>(1);
      opened_ = true;
      awaitingOpened_ = false;
      lastPing_ = millis();
      Serial.println("[Voicebot] Session opened: PCM16 mono at 16 kHz.");
      if (onOpened_) onOpened_(sessionId_);
      return;
    }

    if (strcmp(type, "disconnect") == 0) {
      failConnection("Server ended the session", true);
      return;
    }
    if (!opened_ || strcmp(type, "event")) return;

    JsonArrayConst entities = doc["parameters"]["entities"].as<JsonArrayConst>();
    size_t logBudget = kLogBudgetPerMessage;
    for (JsonObjectConst entity : entities) {
      const char* eventType = entity["type"] | "";
      JsonObjectConst data = entity["data"];
      if (strcmp(eventType, "user_turn_response") == 0) {
        const char* transcript = data["transcript"]["result"]["text"] | "";
        const bool isFinal = data["is_final"] | false;
        const size_t logLimit = logBudget < kMaxLogTextBytes ? logBudget : kMaxLogTextBytes;
        const size_t logLength = textPrefixLength(transcript, logLimit);
        if (logLength) {
          Serial.printf("[USER_STT] %.*s%s\n", static_cast<int>(logLength), transcript,
                        isFinal ? " (final)" : "");
          logBudget -= logLength;
        }
        if (onUserStt_ && *transcript) {
          onUserStt_(String(transcript, textPrefixLength(transcript, kMaxDisplayTextBytes)), isFinal);
        }
      } else if (strcmp(eventType, "bot_turn_response") == 0) {
        const char* output = data["output"] | "";
        const size_t logLimit = logBudget < kMaxLogTextBytes ? logBudget : kMaxLogTextBytes;
        const size_t logLength = textPrefixLength(output, logLimit);
        if (logLength) {
          Serial.printf("[BOT_REPLY] %.*s\n", static_cast<int>(logLength), output);
          logBudget -= logLength;
        }
        if (onBotReply_ && *output) {
          onBotReply_(String(output, textPrefixLength(output, kMaxDisplayTextBytes)));
        }
      } else if (strcmp(eventType, "barge_in") == 0) {
        if (onBargeIn_) onBargeIn_();
      }
    }
  }
};
