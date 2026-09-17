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
  enum class FailureReason : uint8_t {
    None, Network, Timeout, ServerError, ServerCompleted, Unauthorized, Protocol, Resource
  };
  FailureReason failureReason() const { return failureReason_; }
  bool retryableDisconnect() const {
    return failureReason_ == FailureReason::Network || failureReason_ == FailureReason::Timeout ||
           failureReason_ == FailureReason::ServerError || failureReason_ == FailureReason::Resource;
  }

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
    failureReason_ = FailureReason::None;
    if (!validHost(host) || !port || !path || path[0] != '/' ||
        strnlen(path, 513) > 512 || strchr(path, '\r') || strchr(path, '\n') ||
        !apiKey || !*apiKey || strnlen(apiKey, 513) > 512 ||
        !agentId || !*agentId || strnlen(agentId, 129) > 128) {
      failureReason_ = FailureReason::Protocol;
      Serial.println("[Voicebot] Invalid endpoint or missing credentials.");
      return false;
    }
    // A missing/broken CA must fail closed; never silently call setInsecure().
    // The CA storage must outlive the connection (tls_roots.h is static flash).
    if (!ca || !strstr(ca, "-----BEGIN CERTIFICATE-----") ||
        !strstr(ca, "-----END CERTIFICATE-----")) {
      failureReason_ = FailureReason::Protocol;
      Serial.println("[Voicebot] A valid TLS root certificate is required.");
      return false;
    }

    String urlPath;
    const size_t capacity = strlen(path) + 3 * (strlen(apiKey) + strlen(agentId)) + 24;
    if (!urlPath.reserve(capacity)) {
      failureReason_ = FailureReason::Resource;
      return false;
    }
    urlPath += path;
    urlPath += strchr(path, '?') ? "&api_key=" : "?api_key=";
    appendQueryValue(urlPath, apiKey);
    urlPath += "&agent_id=";
    appendQueryValue(urlPath, agentId);
    // Query strings contain the API key. Do not print the request URL.
    Serial.printf("[Voicebot] Connecting to %s:%u with verified TLS.\n", host, port);
    setExtraHeaders("");  // Preview Call requires no custom Origin/auth headers.
    beginSslWithCA(host, port, urlPath.c_str(), ca, "");
    running_ = true;
    disconnectNotified_ = false;
    return true;
  }

  void stop() {
    // disconnect() alone leaves the underlying reconnect timer enabled.
    running_ = false;
    disconnectNotified_ = true;
    closeRequested_ = false;
    closeSent_ = false;
    closeRequestedAt_ = 0;
    closeSentAt_ = 0;
    clearSession();
    WebSocketsClient::disconnect();
  }

  // End a user-requested call without tearing down the transport before the
  // protocol close reaches the server. If no session has opened yet, there is
  // no session id to close and the pending connection is stopped immediately.
  void requestClose() {
    if (!running_ || closeRequested_) return;
    if (!opened_) {
      stop();
      return;
    }
    closeRequested_ = true;
    closeSent_ = false;
    closeRequestedAt_ = millis();
    closeSentAt_ = 0;
    // A transport close during this bounded grace period is expected hangup,
    // not a failure that should notify the reconnect path.
    disconnectNotified_ = true;
  }

  void loop() {
    if (!running_) return;
    WebSocketsClient::loop();
    if (!running_) return;
    const uint32_t now = millis();
    if (closeRequested_) {
      serviceClose(now);
      return;
    }
    updateReceiveProgress(now);
    if (awaitingOpened_ && static_cast<uint32_t>(now - connectedAt_) >= kOpenedTimeoutMs) {
      failConnection("Timed out waiting for session opened", true, FailureReason::Timeout);
      return;
    }
    if (opened_ && !serviceLiveness(now)) return;
    if (opened_ && static_cast<uint32_t>(now - lastPing_) >= kPingIntervalMs && canSendNow()) {
      sendPing();
    }
  }

  bool isRunning() const { return running_; }
  bool isClosing() const { return running_ && closeRequested_; }
  bool isOpened() { return running_ && opened_ && !closeRequested_ && WebSocketsClient::isConnected(); }
  // Advisory zero-timeout readiness; a failed/partial TLS send still closes.
  bool canSendNow() const { return running_ && opened_ && !closeRequested_ && WebSocketsClient::canSendNow(); }
  bool isReceivingAudio() const { return opened_ && !closeRequested_ && isReceivingBinary(); }
  String getSessionId() const { return sessionId_; }

  bool sendAudioFrame(const uint8_t* pcm, size_t length) {
    // Microphone packets are continuous 20 ms PCM16 mono, 640 bytes at 16 kHz.
    if (!isOpened() || !pcm || !length || length > 640 || (length & 1)) return false;
    if (sendBIN(pcm, length)) return true;
    if (running_) failConnection("Audio send failed", true, FailureReason::Network);
    return false;
  }

  bool sendPing() {
    if (!sendJson("ping")) return false;
    if (!pingOutstanding_) pendingPingAt_ = millis();
    pingOutstanding_ = true;
    pendingPingSeq_ = seq_;
    lastPing_ = millis();
    return true;
  }
  bool sendPlaybackStarted() { return sendJson("playback_started"); }
  bool sendPlaybackCompleted() { return sendJson("playback_completed"); }

 private:
  static constexpr size_t kMaxJsonBytes = 8192;
  // A 7000-byte string needs ArduinoJson's 8191-byte growing string buffer
  // plus object/array nodes. This fixed budget supports the 8 KiB wire limit.
  static constexpr size_t kJsonArenaBytes = 12 * 1024;
  static constexpr size_t kMaxSessionIdBytes = 128;
  static constexpr size_t kMaxDisplayTextBytes = 2048;
  static constexpr size_t kMaxLogTextBytes = 256;
  static constexpr size_t kLogBudgetPerMessage = 512;
  static constexpr uint32_t kOpenedTimeoutMs = 15000;
  static constexpr uint32_t kPingIntervalMs = 20000;
  static constexpr uint32_t kNoInboundTimeoutMs = 60000;
  static constexpr uint32_t kPingResponseGraceMs = 10000;
  static constexpr uint32_t kCloseDrainMs = 300;
  static constexpr uint32_t kCloseSendTimeoutMs = 1000;

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

  FailureReason failureReason_ = FailureReason::None;
  bool running_ = false;
  bool opened_ = false;
  bool awaitingOpened_ = false;
  bool disconnectNotified_ = true;
  bool closeRequested_ = false;
  bool closeSent_ = false;
  uint32_t seq_ = 0;
  String sessionId_;
  uint32_t lastPing_ = 0;
  uint32_t connectedAt_ = 0;
  uint32_t closeRequestedAt_ = 0;
  uint32_t closeSentAt_ = 0;
  uint32_t lastInboundAt_ = 0;
  uint32_t observedReceiveProgress_ = 0;
  uint32_t pendingPingAt_ = 0;
  uint32_t pendingPingSeq_ = 0;
  uint32_t writeBlockedAt_ = 0;
  bool pingOutstanding_ = false;
  bool writeBlocked_ = false;
  bool receiveWasBackpressured_ = false;

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

  static bool validSessionId(JsonVariantConst value) {
    const JsonString encoded = value.as<JsonString>();
    const char* id = encoded.c_str();
    if (!id || !encoded.size() || encoded.size() > kMaxSessionIdBytes ||
        encoded.size() != strlen(id)) return false;
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
    lastInboundAt_ = millis();
    observedReceiveProgress_ = receiveProgress();
    pendingPingAt_ = 0;
    pendingPingSeq_ = 0;
    writeBlockedAt_ = 0;
    pingOutstanding_ = false;
    writeBlocked_ = false;
    receiveWasBackpressured_ = false;
  }

  void updateReceiveProgress(uint32_t now) {
    const uint32_t progress = receiveProgress();
    const bool paused = isReceiveBackpressured();
    if (progress != observedReceiveProgress_ || paused || receiveWasBackpressured_) lastInboundAt_ = now;
    observedReceiveProgress_ = progress;
    receiveWasBackpressured_ = paused;
  }

  bool serviceLiveness(uint32_t now) {
    if (canSendNow()) {
      writeBlocked_ = false;
    } else if (!writeBlocked_) {
      writeBlocked_ = true;
      writeBlockedAt_ = now;
    }
    const bool waitingForPong = pingOutstanding_ &&
        static_cast<uint32_t>(now - pendingPingAt_) >= kPingResponseGraceMs;
    const bool stalledWrite = writeBlocked_ &&
        static_cast<uint32_t>(now - writeBlockedAt_) >= kNoInboundTimeoutMs;
    if (static_cast<uint32_t>(now - lastInboundAt_) >= kNoInboundTimeoutMs &&
        (waitingForPong || stalledWrite)) {
      failConnection("No inbound progress from the voice service", true, FailureReason::Timeout);
      return false;
    }
    return true;
  }

  void failConnection(const char* reason, bool closeSocket,
                      FailureReason failure = FailureReason::Protocol) {
    const bool notify = running_ && !disconnectNotified_;
    // Stop this transport before notifying the owner. Only the application
    // decides whether user intent permits a retry; a retry creates a new id.
    failureReason_ = failure;
    running_ = false;
    disconnectNotified_ = true;
    closeRequested_ = false;
    closeSent_ = false;
    closeRequestedAt_ = 0;
    closeSentAt_ = 0;
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
      if (running_) failConnection("Control send failed", true, FailureReason::Network);
      return false;
    }
    seq_ = nextSeq;
    return true;
  }

  bool sendCloseNow() {
    if (!running_ || !opened_ || !WebSocketsClient::isConnected() || seq_ == UINT32_MAX) return false;
    const uint32_t nextSeq = seq_ + 1;
    char envelope[256];
    const int length = snprintf(envelope, sizeof(envelope),
        "{\"version\":\"2\",\"type\":\"close\",\"seq\":%lu,\"id\":\"%s\",\"parameters\":{\"reason\":\"end\"}}",
        static_cast<unsigned long>(nextSeq), sessionId_.c_str());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(envelope) ||
        !sendTXT(envelope, static_cast<size_t>(length))) return false;
    seq_ = nextSeq;
    return true;
  }

  void serviceClose(uint32_t now) {
    if (!opened_ || !WebSocketsClient::isConnected()) {
      stop();
      return;
    }
    if (!closeSent_) {
      if (WebSocketsClient::canSendNow()) {
        if (!sendCloseNow()) {
          stop();
          return;
        }
        closeSent_ = true;
        // sendTXT is synchronous and can itself consume time. Start the ACK
        // grace only after the complete close frame has left this client.
        closeSentAt_ = millis();
        Serial.println("[Voicebot] Session close sent; waiting for server acknowledgment.");
      } else if (static_cast<uint32_t>(now - closeRequestedAt_) >= kCloseSendTimeoutMs) {
        stop();
      }
      return;
    }
    if (static_cast<uint32_t>(now - closeSentAt_) >= kCloseDrainMs) stop();
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
        if (closeRequested_) stop();
        else handleTransportFailure(false);
        break;
      case WStype_ERROR:
        if (closeRequested_) stop();
        else handleTransportFailure(true);
        break;
      case WStype_BIN:
        if (bytes && length) lastInboundAt_ = millis();
        // The bounded transport emits chunks for both ordinary and fragmented
        // binary messages, including their final bytes. Text never reaches here.
        if (opened_ && !closeRequested_ && onTtsAudio_ && bytes && length) onTtsAudio_(bytes, length);
        break;
      case WStype_TEXT:
        if (bytes && length) lastInboundAt_ = millis();
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

  void handleTransportFailure(bool closeSocket) {
    const uint16_t code = lastCloseCode();
    if (code == 1008) {
      failConnection("Server rejected session authorization or policy", closeSocket, FailureReason::Unauthorized);
    } else if (code == 1000 && opened_) {
      failConnection("Server completed the session", closeSocket, FailureReason::ServerCompleted);
    } else if (code == 1011) {
      failConnection("Server reported an upstream error", closeSocket, FailureReason::ServerError);
    } else if (code == 1002 || code == 1003 || code == 1007 || code == 1009) {
      failConnection("WebSocket protocol or payload limit error", closeSocket, FailureReason::Protocol);
    } else {
      failConnection("WebSocket connection lost", closeSocket, FailureReason::Network);
    }
  }

  // Legacy bare events remain supported. When an envelope identifies itself,
  // apply it only to this version/session; never let stale closed/events affect
  // the current call. JsonString compares length, including embedded NUL bytes.
  bool matchesSessionEnvelope(const JsonDocument& doc) const {
    if (!doc["version"].isUnbound() && doc["version"] != "2") return false;
    if (!doc["id"].isUnbound()) {
      const JsonString id = doc["id"].as<JsonString>();
      if (!id.c_str() || id.size() > kMaxSessionIdBytes || id.size() != strlen(id.c_str())) return false;
      // Before opened there is no local id to compare: the service can reject
      // authentication/upstream setup with a newly assigned id in its error.
      if (sessionId_.length() && (id.size() != sessionId_.length() ||
          memcmp(id.c_str(), sessionId_.c_str(), id.size()) != 0)) return false;
    }
    return true;
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
    const JsonVariantConst type = doc["type"];
    const JsonString encodedType = type.as<JsonString>();
    if (!encodedType.c_str() || !encodedType.size() || encodedType.size() != strlen(encodedType.c_str())) {
      failConnection("Invalid message type", true, FailureReason::Protocol);
      return;
    }
    if (closeRequested_ && type != "closed" && type != "disconnect") return;
    if (opened_ && !matchesSessionEnvelope(doc)) {
      Serial.println("[Voicebot] Ignored an envelope for another version or session.");
      return;
    }
    if (type == "opened") {
      const char* id = doc["id"] | "";
      if (!awaitingOpened_ || opened_ || doc["version"] != "2" || !validSessionId(doc["id"])) {
        failConnection("Invalid opened session", true);
        return;
      }
      JsonVariantConst clientseq = doc["clientseq"];
      if (!clientseq.is<uint32_t>() || clientseq.as<uint32_t>() == UINT32_MAX) {
        failConnection("Invalid opened client sequence", true);
        return;
      }
      // Playback and microphone hardware are configured for exactly this one
      // negotiated stream. Do not guess a channel or sequence on malformed
      // handshakes; doing so would corrupt later control-message ordering.
      JsonArrayConst media = doc["parameters"]["media"].as<JsonArrayConst>();
      JsonVariantConst startPaused = doc["parameters"]["startPaused"];
      JsonObjectConst entry = media.size() == 1 ? media[0].as<JsonObjectConst>() : JsonObjectConst();
      JsonArrayConst channels = entry["channels"].as<JsonArrayConst>();
      const bool supportedAudio = !entry.isNull() &&
          entry["type"] == "audio/L16" &&
          (entry["sampleRateHz"] | 0) == 16000 && channels.size() == 1 &&
          channels[0] == "external";
      if (!supportedAudio || !startPaused.is<bool>() || startPaused.as<bool>()) {
        failConnection("Unsupported session audio format", true);
        return;
      }
      sessionId_ = id;
      if (sessionId_.length() != strlen(id)) {
        failConnection("Could not allocate session identifier", true, FailureReason::Resource);
        return;
      }
      seq_ = clientseq.as<uint32_t>();
      opened_ = true;
      awaitingOpened_ = false;
      lastPing_ = millis();
      lastInboundAt_ = lastPing_;
      observedReceiveProgress_ = receiveProgress();
      Serial.println("[Voicebot] Session opened: PCM16 mono at 16 kHz.");
      if (onOpened_) onOpened_(sessionId_);
      return;
    }

    if (!matchesSessionEnvelope(doc)) {
      Serial.println("[Voicebot] Ignored an envelope for another version or session.");
      return;
    }
    if (type == "closed") {
      if (closeRequested_) stop();
      else failConnection("Server completed the session", true, FailureReason::ServerCompleted);
      return;
    }
    if (type == "disconnect") {
      if (closeRequested_) { stop(); return; }
      const JsonVariantConst reason = doc["parameters"]["reason"];
      if (reason == "completed") {
        failConnection("Server completed the session", true, FailureReason::ServerCompleted);
      } else if (reason == "unauthorized") {
        failConnection("Server rejected session authorization", true, FailureReason::Unauthorized);
      } else if (reason == "error") {
        failConnection("Server reported an error", true, FailureReason::ServerError);
      } else {
        // Do not echo arbitrary server info: it may contain credentials or URLs.
        failConnection("Server ended the session with an unknown reason", true, FailureReason::Protocol);
      }
      return;
    }
    if (type == "pong") {
      const JsonVariantConst ack = doc["clientseq"];
      if (ack.isUnbound() || (ack.is<uint32_t>() && ack.as<uint32_t>() == pendingPingSeq_)) {
        pingOutstanding_ = false;
      }
      return;
    }
    if (opened_ && type == "barge_in") {
      if (onBargeIn_) onBargeIn_();
      return;
    }
    if (!opened_ || type != "event") return;

    JsonArrayConst entities = doc["parameters"]["entities"].as<JsonArrayConst>();
    size_t logBudget = kLogBudgetPerMessage;
    for (JsonObjectConst entity : entities) {
      const JsonVariantConst eventType = entity["type"];
      JsonObjectConst data = entity["data"];
      if (eventType == "user_turn_response") {
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
      } else if (eventType == "bot_turn_response") {
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
      } else if (eventType == "barge_in") {
        if (onBargeIn_) onBargeIn_();
      }
    }
  }
};
