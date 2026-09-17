#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>
#include "src/cloud_websockets/WebSocketsClient.h"

class VoicebotClient : private WebSocketsClient {
 public:
  using TtsAudioCallback = std::function<void(const uint8_t* pcm, size_t length)>;
  using OpenedCallback   = std::function<void(const String& sessionId)>;
  using UserSttCallback  = std::function<void(const String& text, bool isFinal)>;
  using BotReplyCallback = std::function<void(const String& text)>;
  using BargeInCallback  = std::function<void()>;
  using DisconnectCallback = std::function<void(const String& info)>;

  VoicebotClient() {
    onEvent([this](WStype_t type, uint8_t* bytes, size_t length) {
      handleEvent(type, bytes, length);
    });
  }

  ~VoicebotClient() { stop(); }

  void setTtsAudioCallback(TtsAudioCallback cb) { onTtsAudio_ = cb; }
  void setOpenedCallback(OpenedCallback cb) { onOpened_ = cb; }
  void setUserSttCallback(UserSttCallback cb) { onUserStt_ = cb; }
  void setBotReplyCallback(BotReplyCallback cb) { onBotReply_ = cb; }
  void setBargeInCallback(BargeInCallback cb) { onBargeIn_ = cb; }
  void setDisconnectCallback(DisconnectCallback cb) { onDisconnect_ = cb; }

  bool start(const char* host, uint16_t port, const char* path, const char* apiKey, const char* agentId, const char* ca = nullptr) {
    stop();
    if (!host || !apiKey || !agentId) return false;

    String urlPath = String(path) + "?api_key=" + apiKey + "&agent_id=" + agentId;
    Serial.printf("[Voicebot] Connecting to wss://%s:%d%s\n", host, port, urlPath.c_str());

    setExtraHeaders("User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\nOrigin: https://voicebot-stg.botnoigroup.com");
    
    if (ca && strlen(ca) > 50 && strstr(ca, "-----BEGIN CERTIFICATE-----")) {
      beginSslWithCA(host, port, urlPath.c_str(), ca, "");
    } else {
      beginSSL(host, port, urlPath.c_str());
    }

    setReconnectInterval(5000);
    opened_ = false;
    seq_ = 0;
    sessionId_ = "";
    lastPing_ = millis();

    return true;
  }

  void stop() {
    opened_ = false;
    WebSocketsClient::disconnect();
  }

  void loop() {
    WebSocketsClient::loop();

    // Ping Keep-Alive every 20 seconds
    if (opened_ && millis() - lastPing_ >= 20000) {
      sendPing();
      lastPing_ = millis();
    }
  }

  bool isOpened() { return opened_ && WebSocketsClient::isConnected(); }
  String getSessionId() const { return sessionId_; }

  bool sendAudioFrame(const uint8_t* pcm, size_t length) {
    if (!isOpened() || !pcm || length == 0) return false;
    return sendBIN(pcm, length);
  }

  void sendSilencePadding(size_t frames = 25) {
    if (!isOpened()) return;
    static const uint8_t silence[640] = {0};
    for (size_t i = 0; i < frames; ++i) {
      sendBIN(silence, sizeof(silence));
      delay(20);
    }
    Serial.println("[Voicebot] Sent 500ms End-of-Speech Silence Padding.");
  }

  bool sendPing() {
    return sendJson("ping");
  }

  bool sendPlaybackStarted() {
    return sendJson("playback_started");
  }

  bool sendPlaybackCompleted() {
    return sendJson("playback_completed");
  }

 private:
  bool opened_ = false;
  uint32_t seq_ = 0;
  String sessionId_ = "";
  uint32_t lastPing_ = 0;

  TtsAudioCallback   onTtsAudio_;
  OpenedCallback     onOpened_;
  UserSttCallback    onUserStt_;
  BotReplyCallback   onBotReply_;
  BargeInCallback    onBargeIn_;
  DisconnectCallback onDisconnect_;

  bool sendJson(const char* type, const JsonDocument* params = nullptr) {
    if (!WebSocketsClient::isConnected()) return false;

    JsonDocument doc;
    doc["version"] = "2";
    doc["type"] = type;
    doc["seq"] = ++seq_;
    doc["id"] = sessionId_;
    if (params) {
      doc["parameters"] = *params;
    } else {
      doc["parameters"].to<JsonObject>();
    }

    String jsonStr;
    serializeJson(doc, jsonStr);
    return sendTXT(jsonStr);
  }

  void handleEvent(WStype_t type, uint8_t* bytes, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        Serial.println("[Voicebot] 🔌 WebSocket TLS Connected! Awaiting session 'opened' event...");
        break;

      case WStype_DISCONNECTED:
        {
          String reason = (bytes && length > 0) ? String((char*)bytes, length) : "Unknown Reason";
          Serial.printf("[Voicebot] ⚠️ WebSocket Disconnected (Reason: %s)\n", reason.c_str());
          if (opened_ && onDisconnect_) onDisconnect_(reason);
          opened_ = false;
        }
        break;

      case WStype_ERROR:
        Serial.println("[Voicebot] ❌ WebSocket Connection Error.");
        opened_ = false;
        break;

      case WStype_BIN:
      case WStype_FRAGMENT_BIN_START:
      case WStype_FRAGMENT:
        if (onTtsAudio_ && bytes && length > 0) {
          onTtsAudio_(bytes, length);
        }
        break;

      case WStype_TEXT:
        if (bytes && length > 0) {
          JsonDocument doc;
          DeserializationError err = deserializeJson(doc, bytes, length);
          if (!err) {
            parseServerJson(doc);
          }
        }
        break;

      default:
        break;
    }
  }

  void parseServerJson(const JsonDocument& doc) {
    const char* type = doc["type"] | "";

    if (strcmp(type, "opened") == 0) {
      sessionId_ = doc["id"] | "";
      seq_ = doc["clientseq"] | 1;
      opened_ = true;
      lastPing_ = millis();
      Serial.printf("[Voicebot] 🎉 Session Opened! Session ID: %s\n", sessionId_.c_str());
      if (onOpened_) onOpened_(sessionId_);
    }
    else if (strcmp(type, "event") == 0) {
      JsonArrayConst entities = doc["parameters"]["entities"].as<JsonArrayConst>();
      for (JsonObjectConst entity : entities) {
        const char* eType = entity["type"] | "";
        JsonObjectConst eData = entity["data"];

        if (strcmp(eType, "user_turn_response") == 0) {
          const char* transcript = eData["transcript"]["result"]["text"] | "";
          bool isFinal = eData["is_final"] | false;
          if (strlen(transcript) > 0) {
            Serial.printf("[USER_STT] 🗣️ User: '%s' %s\n", transcript, isFinal ? "(Final)" : "(Listening...)");
            if (onUserStt_) onUserStt_(transcript, isFinal);
          }
        }
        else if (strcmp(eType, "bot_turn_response") == 0) {
          const char* output = eData["output"] | "";
          if (strlen(output) > 0) {
            Serial.printf("[BOT_REPLY] 🤖 Bot: '%s'\n", output);
            if (onBotReply_) onBotReply_(output);
          }
        }
        else if (strcmp(eType, "barge_in") == 0) {
          Serial.println("[BARGE_IN] ⚡ User interrupted Bot while speaking!");
          if (onBargeIn_) onBargeIn_();
        }
      }
    }
    else if (strcmp(type, "disconnect") == 0) {
      const char* info = doc["parameters"]["info"] | "";
      Serial.printf("[Voicebot] ⚠️ Server disconnected session: %s\n", info);
      opened_ = false;
      if (onDisconnect_) onDisconnect_(info);
    }
  }
};
