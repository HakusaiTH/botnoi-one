#include <Arduino.h>
#include <cassert>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// Inject a deterministic transport at the existing include guard. This tests
// the real session/JSON client without a socket, FreeRTOS, or actual credentials.
#define WEBSOCKETSCLIENT_H_
enum WStype_t {
  WStype_ERROR, WStype_DISCONNECTED, WStype_CONNECTED, WStype_TEXT, WStype_BIN,
  WStype_FRAGMENT_TEXT_START, WStype_FRAGMENT_BIN_START, WStype_FRAGMENT,
  WStype_FRAGMENT_FIN, WStype_PING, WStype_PONG
};
class WebSocketsClient {
 public:
  using Event = std::function<void(WStype_t, uint8_t*, size_t)>;
  inline static WebSocketsClient* current = nullptr;
  WebSocketsClient() { current = this; }
  virtual ~WebSocketsClient() = default;
  void onEvent(Event callback) { event = std::move(callback); }
  void setBinaryReceiveCapacity(std::function<size_t()> callback) { capacity = std::move(callback); }
  bool isReceivingBinary() const { return receivingBinary; }
  void beginSslWithCA(const char*, uint16_t, const char* path, const char* ca, const char*) {
    beginCount++;
    url = path;
    certificate = ca;
  }
  void setExtraHeaders(const char* headers) { extraHeaders = headers ? headers : ""; }
  void setReconnectInterval(unsigned long interval) { (void)interval; }
  void loop() { loopCount++; }
  bool isConnected() { return connected; }
  bool canSendNow() const { return connected && writable; }
  void disconnect() {
    disconnectCount++;
    if (connected) {
      connected = false;
      emit(WStype_DISCONNECTED);
    }
  }
  bool sendBIN(const uint8_t* pcm, size_t length) {
    if (!sendSucceeds || !writable) return false;
    binary.emplace_back(pcm, pcm + length);
    return true;
  }
  bool sendTXT(char* text, size_t length) {
    if (!sendSucceeds || !writable) return false;
    messages.emplace_back(text, length);
    fakeMillis += sendDelayMs;
    return true;
  }
  void emit(WStype_t type, const std::string& data = "") {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    if (type == WStype_CONNECTED) connected = true;
    if (type == WStype_DISCONNECTED) connected = false;
    event(type, bytes.empty() ? nullptr : bytes.data(), bytes.size());
  }
  Event event;
  std::function<size_t()> capacity;
  bool connected = false;
  bool receivingBinary = false;
  bool sendSucceeds = true;
  bool writable = true;
  unsigned beginCount = 0, loopCount = 0, disconnectCount = 0;
  uint32_t sendDelayMs = 0;
  std::string url;
  std::string extraHeaders = "Origin: file://";
  const char* certificate = nullptr;
  std::vector<std::string> messages;
  std::vector<std::vector<uint8_t>> binary;
};

#include "../../voicebot_client.h"

static constexpr char ca[] = "-----BEGIN CERTIFICATE-----\nTEST-ONLY\n-----END CERTIFICATE-----";
static const std::string opened = R"({"version":"2","type":"opened","id":"session-001","clientseq":1,"parameters":{"startPaused":false,"media":[{"type":"audio/L16","sampleRateHz":16000,"channels":["external"]}]}})";
static const std::string event = R"({"version":"2","type":"event","parameters":{"entities":[{"type":"user_turn_response","data":{"transcript":{"result":{"text":"hello"}},"is_final":true}},{"type":"bot_turn_response","data":{"output":"world"}},{"type":"barge_in","data":{}}]}})";

static WebSocketsClient& ws() { return *WebSocketsClient::current; }
static void start(VoicebotClient& client) {
  assert(client.start("voice.example.com", 443, "/v1/call", "test-secret-key", "agent1", ca));
}
static void connect(VoicebotClient& client) {
  start(client);
  ws().emit(WStype_CONNECTED);
  ws().emit(WStype_TEXT, opened);
  assert(client.isOpened());
}

static void testVerifiedTlsAndCredentialRedaction() {
  VoicebotClient client;
  assert(!client.start("voice.example.com", 443, "/", "key", "agent", nullptr));
  assert(!client.start("voice.example.com", 443, "/", "key", "agent", "-----BEGIN CERTIFICATE-----"));
  assert(ws().beginCount == 0);
  Serial.log.clear();
  assert(client.start("voice.example.com", 443, "/v1/call?preview=1", "private+key&x=y", "agent/id", ca));
  assert(ws().certificate == ca);
  assert(ws().extraHeaders.empty());
  assert(ws().url == "/v1/call?preview=1&api_key=private%2Bkey%26x%3Dy&agent_id=agent%2Fid");
  assert(Serial.log.find("private") == std::string::npos);
  assert(Serial.log.find("api_key") == std::string::npos);
  assert(!client.start("voice.example.com\r\nBad", 443, "/", "key", "agent", ca));
  assert(!client.start("voice.example.com", 443, nullptr, "key", "agent", ca));
  assert(!client.start("voice.example.com", 443, "/", "", "agent", ca));
}

static void testSessionAndOutboundPackets() {
  VoicebotClient client;
  size_t audioBytes = 0;
  unsigned opens = 0;
  client.setTtsAudioCallback([&](const uint8_t*, size_t n) { audioBytes += n; });
  client.setTtsAudioCapacityCallback([] { return 640; });
  client.setOpenedCallback([&](const String& id) { assert(std::string(id.c_str()) == "session-001"); opens++; });
  start(client);
  assert(!client.sendPing());
  ws().emit(WStype_CONNECTED);
  assert(!client.sendPlaybackStarted());
  ws().emit(WStype_BIN, std::string(640, 'x'));
  assert(audioBytes == 0);
  assert(ws().capacity() == 640);
  ws().emit(WStype_TEXT, opened);
  assert(opens == 1 && client.isOpened());
  assert(client.sendPlaybackStarted());
  assert(client.sendPing());
  assert(client.sendPlaybackCompleted());
  for (size_t i = 0; i < ws().messages.size(); ++i) {
    JsonDocument document;
    assert(!deserializeJson(document, ws().messages[i]));
    assert(document["seq"].as<unsigned>() == i + 2);
    assert(document["id"].as<std::string>() == "session-001");
    assert(document["parameters"].is<JsonObject>());
  }
  uint8_t pcm[642] = {};
  assert(client.sendAudioFrame(pcm, 640));
  assert(!client.sendAudioFrame(pcm, 641));
  assert(!client.sendAudioFrame(pcm, 642));
  assert(!client.sendAudioFrame(pcm, 0));
  assert(ws().binary.size() == 1 && ws().binary[0].size() == 640);
  ws().emit(WStype_BIN, std::string(1024, 'a'));
  ws().emit(WStype_BIN, std::string(2, 'b'));
  assert(audioBytes == 1026);
  ws().emit(WStype_FRAGMENT_TEXT_START, "not PCM");
  ws().emit(WStype_FRAGMENT, "not PCM");
  ws().emit(WStype_FRAGMENT_FIN, "not PCM");
  assert(audioBytes == 1026);
  ws().receivingBinary = true;
  assert(client.isReceivingAudio());
}

static void testMultipleTurnsReuseOneSession() {
  VoicebotClient client;
  unsigned finalTranscripts = 0, replies = 0;
  client.setUserSttCallback([&](const String&, bool isFinal) {
    if (isFinal) ++finalTranscripts;
  });
  client.setBotReplyCallback([&](const String&) { ++replies; });
  connect(client);
  const unsigned beginCount = ws().beginCount;
  const unsigned disconnectCount = ws().disconnectCount;
  assert(beginCount == 1);
  uint8_t pcm[640] = {};
  for (unsigned turn = 0; turn < 3; ++turn) {
    assert(client.sendAudioFrame(pcm, sizeof(pcm)));
    ws().emit(WStype_TEXT, event);
    ws().emit(WStype_BIN, std::string(640, 't'));
    assert(client.sendPlaybackStarted());
    assert(client.sendPlaybackCompleted());
    assert(client.isOpened());
    assert(std::string(client.getSessionId().c_str()) == "session-001");
    assert(ws().beginCount == beginCount && ws().disconnectCount == disconnectCount);
  }
  assert(finalTranscripts == 3 && replies == 3);
}

static void testFailureEndsCallAndExplicitRestartResetsSession() {
  VoicebotClient client;
  unsigned disconnects = 0;
  client.setDisconnectCallback([&](const String&) { disconnects++; });
  connect(client);
  ws().emit(WStype_ERROR, "server URL with secret-key");
  assert(!client.isRunning() && !client.isOpened() && client.getSessionId().length() == 0);
  assert(disconnects == 1);
  ws().emit(WStype_DISCONNECTED);
  assert(disconnects == 1);
  assert(!client.sendPlaybackCompleted());
  connect(client);  // A new physical-button action starts a new server session.
  assert(client.isOpened());
  assert(client.sendPing());
  JsonDocument document;
  assert(!deserializeJson(document, ws().messages.back()));
  assert(document["seq"] == 2);
  ws().emit(WStype_TEXT, R"({"type":"disconnect","parameters":{"info":"session ended"}})");
  assert(!client.isRunning() && !client.isOpened() && !ws().connected && disconnects == 2);
  const unsigned loops = ws().loopCount;
  client.stop();
  client.loop();
  assert(ws().loopCount == loops);
}

static void testOpenedTimeoutAndMillisRollover() {
  VoicebotClient client;
  unsigned disconnects = 0;
  client.setDisconnectCallback([&](const String&) { disconnects++; });
  start(client);
  fakeMillis = UINT32_MAX - 7000;
  ws().emit(WStype_CONNECTED);
  fakeMillis += 14999;
  client.loop();
  assert(disconnects == 0);
  fakeMillis++;
  client.loop();
  assert(disconnects == 1 && !ws().connected && !client.isOpened());
  connect(client);
  fakeMillis += 20000;
  client.loop();
  assert(ws().messages.size() == 1);
  JsonDocument document;
  assert(!deserializeJson(document, ws().messages[0]));
  assert(document["type"] == "ping");
}

static void testPingDefersWhileTransportIsFull() {
  VoicebotClient client;
  unsigned disconnects = 0;
  client.setDisconnectCallback([&](const String&) { disconnects++; });
  connect(client);
  assert(client.canSendNow());
  ws().writable = false;
  assert(!client.canSendNow());
  fakeMillis += 20000;
  client.loop();
  fakeMillis += 20000;
  client.loop();
  assert(client.isOpened() && ws().connected && disconnects == 0);
  assert(ws().messages.empty());
  ws().writable = true;
  fakeMillis++;
  client.loop();
  assert(client.isOpened() && disconnects == 0 && ws().messages.size() == 1);
  JsonDocument document;
  assert(!deserializeJson(document, ws().messages.back()));
  assert(document["type"] == "ping");
  client.loop();
  assert(ws().messages.size() == 1);
}

static void testGracefulUserClose() {
  {
    VoicebotClient client;
    unsigned disconnects = 0;
    client.setDisconnectCallback([&](const String&) { disconnects++; });
    connect(client);
    client.requestClose();
    assert(client.isRunning() && client.isClosing());
    assert(!client.isOpened() && !client.canSendNow());
    uint8_t pcm[640] = {};
    assert(!client.sendAudioFrame(pcm, sizeof(pcm)));
    client.loop();
    assert(ws().messages.size() == 1);
    JsonDocument document;
    assert(!deserializeJson(document, ws().messages.back()));
    assert(document["type"] == "close");
    assert(document["seq"] == 2);
    assert(document["id"] == "session-001");
    assert(document["parameters"]["reason"] == "end");
    ws().emit(WStype_TEXT, R"({"version":"2","type":"closed","id":"session-001"})");
    assert(!client.isRunning() && !client.isClosing() && !ws().connected);
    assert(disconnects == 0);
  }
  {
    VoicebotClient client;
    connect(client);
    const uint32_t closeAt = fakeMillis;
    client.requestClose();
    client.loop();
    fakeMillis = closeAt + 299;
    client.loop();
    assert(client.isRunning() && ws().connected);
    fakeMillis = closeAt + 300;
    client.loop();
    assert(!client.isRunning() && !ws().connected);
  }
  {
    VoicebotClient client;
    connect(client);
    ws().writable = false;
    const uint32_t requestedAt = fakeMillis;
    client.requestClose();
    fakeMillis = requestedAt + 900;
    client.loop();
    assert(client.isRunning() && ws().messages.empty());
    ws().writable = true;
    ws().sendDelayMs = 250;
    client.loop();
    const uint32_t sentAt = fakeMillis;
    assert(ws().messages.size() == 1 && client.isRunning());
    fakeMillis = sentAt + 299;
    client.loop();
    assert(client.isRunning());
    fakeMillis = sentAt + 300;
    client.loop();
    assert(!client.isRunning());
  }
  {
    VoicebotClient client;
    start(client);
    assert(client.isRunning());
    client.requestClose();
    assert(!client.isRunning());
  }
}

static void testUnsupportedOpenedRejected() {
  const std::vector<std::pair<std::string, std::string>> changes = {
    {"16000", "24000"}, {"session-001", ""},
    {"session-001", std::string(129, 's')}, {"session-001", "bad\\\"id"},
    {"\"2\"", "\"3\""}, {"\"startPaused\":false", "\"startPaused\":true"}
  };
  for (const auto& change : changes) {
    VoicebotClient client;
    start(client);
    ws().emit(WStype_CONNECTED);
    std::string invalid = opened;
    invalid.replace(invalid.find(change.first), change.first.size(), change.second);
    ws().emit(WStype_TEXT, invalid);
    assert(!client.isRunning() && !client.isOpened() && !ws().connected);
  }

  std::vector<std::string> invalids;
  for (const auto& change : std::vector<std::pair<std::string, std::string>>{
      {"\"clientseq\":1,", ""},
      {"\"clientseq\":1", "\"clientseq\":\"1\""},
      {"\"clientseq\":1", "\"clientseq\":4294967295"},
      {"\"startPaused\":false,", ""},
      {"\"startPaused\":false", "\"startPaused\":\"false\""},
      {"\"external\"", "\"internal\""},
      {"[\"external\"]", "[]"},
      {"}]}", "},{\"type\":\"audio/L16\",\"sampleRateHz\":16000,\"channels\":[\"external\"]}]}"}}) {
    std::string invalid = opened;
    const size_t at = invalid.find(change.first);
    assert(at != std::string::npos);
    invalid.replace(at, change.first.size(), change.second);
    invalids.push_back(std::move(invalid));
  }
  for (const std::string& invalid : invalids) {
    VoicebotClient client;
    start(client);
    ws().emit(WStype_CONNECTED);
    ws().emit(WStype_TEXT, invalid);
    assert(!client.isRunning() && !client.isOpened() && !ws().connected);
  }
}

static void testSendFailureClosesSession() {
  for (bool binary : {false, true}) {
    VoicebotClient client;
    unsigned disconnects = 0;
    client.setDisconnectCallback([&](const String&) { disconnects++; });
    connect(client);
    ws().sendSucceeds = false;
    uint8_t pcm[640] = {};
    assert(!(binary ? client.sendAudioFrame(pcm, sizeof(pcm)) : client.sendPlaybackStarted()));
    assert(disconnects == 1 && !client.isRunning() && !client.isOpened() &&
           client.getSessionId().length() == 0);
  }
}

static void testBoundedJsonAndArenaReuse() {
  VoicebotClient client;
  unsigned replies = 0, transcripts = 0, barges = 0;
  std::string reply;
  client.setBotReplyCallback([&](const String& text) { reply = text.c_str(); replies++; });
  client.setUserSttCallback([&](const String& text, bool final) {
    assert(std::string(text.c_str()) == "hello" && final); transcripts++;
  });
  client.setBargeInCallback([&] { barges++; });
  connect(client);
  for (unsigned i = 0; i < 2000; ++i) ws().emit(WStype_TEXT, event);
  assert(client.isOpened() && replies == 2000 && transcripts == 2000 && barges == 2000);
  ws().emit(WStype_TEXT, R"({"type":"barge_in"})");
  assert(client.isOpened() && barges == 2001);
  std::string longReply = R"({"type":"event","parameters":{"entities":[{"type":"bot_turn_response","data":{"output":")";
  longReply += std::string(2300, 'z');
  longReply += "\"}}]}}";
  Serial.log.clear();
  ws().emit(WStype_TEXT, longReply);
  assert(client.isOpened() && reply.size() == 2048);
  assert(Serial.log.size() <= 280);

  const std::string thai = "ส";
  std::string unicodeReply = R"({"type":"event","parameters":{"entities":[{"type":"bot_turn_response","data":{"output":")";
  for (unsigned i = 0; i < 800; ++i) unicodeReply += thai;
  unicodeReply += "\"}}]}}";
  Serial.log.clear();
  ws().emit(WStype_TEXT, unicodeReply);
  assert(client.isOpened() && reply.size() == 2046);
  assert(reply.substr(reply.size() - thai.size()) == thai);
  assert(Serial.log.size() <= 280);

  std::string multiReply = R"({"type":"event","parameters":{"entities":[)";
  for (unsigned i = 0; i < 3; ++i) {
    if (i) multiReply += ',';
    multiReply += R"({"type":"bot_turn_response","data":{"output":")";
    multiReply += std::string(700, 'q');
    multiReply += "\"}}";
  }
  multiReply += "]}}";
  client.setBotReplyCallback(nullptr);
  Serial.log.clear();
  ws().emit(WStype_TEXT, multiReply);
  assert(client.isOpened() && Serial.log.size() <= 550);

  std::string manyFields = "{";
  for (unsigned i = 0; i < 600; ++i) manyFields += "\"f" + std::to_string(i) + "\":0,";
  manyFields.back() = '}';
  assert(manyFields.size() < 8192);
  std::string deep = "{\"x\":" + std::string(16, '[') + "0" + std::string(16, ']') + "}";
  for (const std::string& invalid : {std::string("{"), std::string("[]"),
       std::string(8193, ' '), manyFields, deep}) {
    ws().emit(WStype_TEXT, invalid);
    assert(!client.isOpened() && !ws().connected);
    connect(client);
    assert(client.isOpened());
    ws().emit(WStype_TEXT, event);
    assert(client.isOpened());
  }
}

int main() {
  testVerifiedTlsAndCredentialRedaction();
  testSessionAndOutboundPackets();
  testMultipleTurnsReuseOneSession();
  testFailureEndsCallAndExplicitRestartResetsSession();
  testOpenedTimeoutAndMillisRollover();
  testPingDefersWhileTransportIsFull();
  testGracefulUserClose();
  testUnsupportedOpenedRejected();
  testSendFailureClosesSession();
  testBoundedJsonAndArenaReuse();
  std::cout << "Voicebot client: TLS, lifecycle, pacing inputs, bounded JSON, and 2000-message reuse passed\n";
}
