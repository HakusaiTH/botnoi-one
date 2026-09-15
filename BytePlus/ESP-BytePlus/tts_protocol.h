#ifndef BYTEPLUS_ESP_TTS_PROTOCOL_H
#define BYTEPLUS_ESP_TTS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// BytePlus v3 bidirectional TTS binary framing. Payload and ID views borrow the
// received WebSocket buffer; consume them before returning from its callback.
// https://docs.byteplus.com/en/docs/byteplusvoice/streaming_tts
// Live-verified sequence (JSON requests, raw PCM16 mono at 16000 Hz):
// 1 {} -> 50; 100 config -> 150; 200 config+text; 102 {};
// receive 350, 352 PCM chunks, 351, then 152; send 2 {} -> receive 52.
// Include event, namespace="BidirectionalTTS" and req_params in session JSON.
// Configure req_params.audio_params={"format":"pcm","sample_rate":16000}.
// Thai voice th_female_bv568_neutral_uranus_bigtts needs resource seed-tts-2.0.
// Errors can arrive as type 15, event 51/153, or a WebSocket text message.
// Audio frames observed exceed 24 KiB including framing; the transport must
// accommodate them. A sentence-end event (351) does not finish the session.
namespace byteplus {
namespace tts {

enum Event : uint32_t {
  StartConnection = 1,
  FinishConnection = 2,
  ConnectionStarted = 50,
  ConnectionFailed = 51,
  ConnectionFinished = 52,
  StartSession = 100,
  CancelSession = 101,
  FinishSession = 102,
  SessionStarted = 150,
  SessionCanceled = 151,
  SessionFinished = 152,
  SessionFailed = 153,
  TaskRequest = 200,
  SentenceStart = 350,
  SentenceEnd = 351,
  AudioResponse = 352,
};

struct Frame {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint8_t serialization = 0;
  uint8_t compression = 0;
  uint32_t event = 0;
  uint32_t errorCode = 0;
  int32_t sequence = 0;
  const uint8_t* sessionId = nullptr;
  size_t sessionIdLength = 0;
  const uint8_t* connectionId = nullptr;
  size_t connectionIdLength = 0;
  const uint8_t* payload = nullptr;
  size_t payloadLength = 0;
};

namespace detail {
inline void writeU32(uint8_t* out, uint32_t value) {
  out[0] = uint8_t(value >> 24);
  out[1] = uint8_t(value >> 16);
  out[2] = uint8_t(value >> 8);
  out[3] = uint8_t(value);
}

struct Cursor {
  const uint8_t* bytes;
  size_t length;
  size_t offset;

  bool u32(uint32_t& value) {
    if (length - offset < 4) return false;
    const uint8_t* p = bytes + offset;
    value = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    offset += 4;
    return true;
  }

  bool blob(const uint8_t*& value, size_t& size) {
    uint32_t declared = 0;
    if (!u32(declared) || declared > length - offset) return false;
    value = bytes + offset;
    size = declared;
    offset += declared;
    return true;
  }
};
}  // namespace detail

// Returns zero without writing on invalid input or insufficient capacity.
// Control events 1/2 have no ID; all session events require a nonempty ID.
// Requests are JSON with compression=0, so firmware needs no gzip dependency.
inline size_t buildEvent(uint8_t* out, size_t capacity, uint32_t event,
                         const char* sessionId, const uint8_t* payload,
                         size_t payloadLength) {
  if (!out || capacity < 12 || event == 0 || payloadLength > UINT32_MAX ||
      payloadLength > capacity || (!payload && payloadLength != 0)) return 0;
  const bool connectionEvent = event == StartConnection || event == FinishConnection;
  size_t idLength = 0;
  if (!connectionEvent) {
    if (!sessionId) return 0;
    while (idLength < capacity && sessionId[idLength] != '\0') ++idLength;
    if (idLength == 0 || idLength == capacity || idLength > UINT32_MAX) return 0;
  }
  size_t required = 12;
  if (!connectionEvent) {
    if (capacity - required < 4 || idLength > capacity - required - 4) return 0;
    required += 4 + idLength;
  }
  if (payloadLength > capacity - required) return 0;
  required += payloadLength;

  out[0] = 0x11;  // Version 1, four-byte header.
  out[1] = 0x14;  // Full-client request, WithEvent flag.
  out[2] = 0x10;  // JSON, no compression.
  out[3] = 0;
  detail::writeU32(out + 4, event);
  size_t offset = 8;
  if (!connectionEvent) {
    detail::writeU32(out + offset, uint32_t(idLength));
    offset += 4;
    memcpy(out + offset, sessionId, idLength);
    offset += idLength;
  }
  detail::writeU32(out + offset, uint32_t(payloadLength));
  offset += 4;
  if (payloadLength != 0) memcpy(out + offset, payload, payloadLength);
  return required;
}

// One complete binary WebSocket message is required. Gzip is identified but
// never decoded here; callers must reject it before treating payload as PCM.
// Strict length checks reject truncation, overflow lengths and trailing bytes.
inline bool parse(const uint8_t* data, size_t length, Frame& frame) {
  frame = Frame{};
  if (!data || length < 8 || data[0] != 0x11 || data[3] != 0) return false;
  Frame result;
  result.type = data[1] >> 4;
  result.flags = data[1] & 15;
  result.serialization = data[2] >> 4;
  result.compression = data[2] & 15;
  if (result.flags > 4 || result.serialization > 1 || result.compression > 1) return false;
  if (result.type != 1 && result.type != 2 && result.type != 9 &&
      result.type != 11 && result.type != 12 && result.type != 15) return false;

  detail::Cursor cursor{data, length, 4};
  if (result.type == 15) {
    if (result.flags != 0 || !cursor.u32(result.errorCode)) return false;
  } else if (result.flags == 1 || result.flags == 3) {
    uint32_t sequence = 0;
    if (!cursor.u32(sequence)) return false;
    // Avoid relying on implementation-defined conversion of uint32_t > INT_MAX.
    result.sequence = sequence <= INT32_MAX ? int32_t(sequence) :
                      -1 - int32_t(UINT32_MAX - sequence);
  } else if (result.flags == 4) {
    if (!cursor.u32(result.event) || result.event == 0) return false;
    if (result.event >= ConnectionStarted && result.event <= ConnectionFinished) {
      if (!cursor.blob(result.connectionId, result.connectionIdLength)) return false;
    } else if (result.event != StartConnection && result.event != FinishConnection) {
      if (!cursor.blob(result.sessionId, result.sessionIdLength)) return false;
    }
  }
  if (!cursor.blob(result.payload, result.payloadLength) || cursor.offset != length) return false;
  frame = result;
  return true;
}

}  // namespace tts
}  // namespace byteplus
#endif
