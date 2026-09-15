#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// SeedASR v3 binary WebSocket protocol. Request/audio payloads are uncompressed;
// live BytePlus sessions return uncompressed JSON with this configuration.
// Reference: https://docs.byteplus.com/en/docs/byteplusvoice/asrunidirect
namespace byteplus {
namespace asr {

struct Frame {
  const uint8_t* payload = nullptr;
  size_t payloadLength = 0;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint8_t serialization = 0;
  uint8_t compression = 0;
  int32_t sequence = 0;
  int32_t event = 0;
  uint32_t errorCode = 0;
  bool final = false;
};

inline uint32_t readUint32(const uint8_t* input) {
  return (uint32_t(input[0]) << 24) | (uint32_t(input[1]) << 16) |
         (uint32_t(input[2]) << 8) | uint32_t(input[3]);
}

inline int32_t readInt32(const uint8_t* input) {
  const uint32_t value = readUint32(input);
  return (value & 0x80000000u) ? -1 - int32_t(~value) : int32_t(value);
}

inline void writeUint32(uint8_t* output, uint32_t value) {
  output[0] = uint8_t(value >> 24);
  output[1] = uint8_t(value >> 16);
  output[2] = uint8_t(value >> 8);
  output[3] = uint8_t(value);
}

// sequence is the positive counter (starting at 1 for the JSON request).
// A final audio request carries its negated sequence and the final flag.
// Returns 0 on invalid arguments or insufficient output capacity.
inline size_t buildRequest(uint8_t* output, size_t capacity,
                           const uint8_t* payload, size_t length,
                           int32_t sequence, bool json, bool final) {
  if (!output || capacity < 12 || length > capacity - 12 ||
      length > UINT32_MAX ||
      (length && !payload) || sequence <= 0 || (json && final)) {
    return 0;
  }
  // memmove permits the caller to place PCM directly in the output buffer.
  if (length) std::memmove(output + 12, payload, length);
  output[0] = 0x11;  // Version 1, four-byte header.
  output[1] = uint8_t((json ? 0x10 : 0x20) | (final ? 0x03 : 0x01));
  output[2] = json ? 0x10 : 0x00;  // JSON or raw, compression disabled.
  output[3] = 0;
  writeUint32(output + 4, uint32_t(final ? -sequence : sequence));
  writeUint32(output + 8, uint32_t(length));
  return 12 + length;
}

// Parse one complete server WebSocket message without allocating or inflating.
// payload remains valid only while the caller retains data. Check compression
// before parsing JSON; compression 1 denotes gzip and needs decompression.
// Final responses can carry a POSITIVE sequence: flags, not sign, mark the end.
inline bool parse(const uint8_t* data, size_t length, Frame& frame) {
  frame = Frame();
  if (!data || length < 4 || (data[0] >> 4) != 1) return false;
  const size_t headerLength = size_t(data[0] & 0x0f) * 4;
  if (headerLength < 4 || headerLength > length) return false;

  Frame parsed;
  parsed.type = data[1] >> 4;
  parsed.flags = data[1] & 0x0f;
  parsed.serialization = data[2] >> 4;
  parsed.compression = data[2] & 0x0f;
  if ((parsed.type != 9 && parsed.type != 15) || parsed.flags > 7 ||
      parsed.serialization > 1 || parsed.compression > 1) {
    return false;
  }

  size_t offset = headerLength;
  if (parsed.flags & 1) {
    if (length - offset < 4) return false;
    parsed.sequence = readInt32(data + offset);
    offset += 4;
  }
  parsed.final = (parsed.flags & 2) != 0;
  if (parsed.flags & 4) {
    if (length - offset < 4) return false;
    parsed.event = readInt32(data + offset);
    offset += 4;
  }
  if (parsed.type == 15) {
    if (length - offset < 4) return false;
    parsed.errorCode = readUint32(data + offset);
    offset += 4;
  }
  if (length - offset < 4) return false;
  const uint32_t payloadLength = readUint32(data + offset);
  offset += 4;
  // No truncated payloads or silently ignored trailing data.
  if (payloadLength != length - offset) return false;
  parsed.payload = data + offset;
  parsed.payloadLength = payloadLength;
  frame = parsed;
  return true;
}

}  // namespace asr
}  // namespace byteplus
