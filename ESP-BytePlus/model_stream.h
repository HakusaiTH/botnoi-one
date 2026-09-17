#ifndef BYTEPLUS_ESP_MODEL_STREAM_H
#define BYTEPLUS_ESP_MODEL_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Allocation-free streaming adapters. Feed only currently available bytes;
// service audio/WebSocket work and check the turn generation between reads.
// JSON stays with the caller: SSE emits complete data fields, including [DONE].
// https://docs.byteplus.com/en/docs/ModelArk/1494384
// https://docs.byteplus.com/en/docs/byteplusvoice/streaming_tts
// https://html.spec.whatwg.org/multipage/server-sent-events.html
// https://www.rfc-editor.org/rfc/rfc9112.html#section-7.1
namespace byteplus {
namespace stream {

enum class Error : uint8_t { None, Invalid, Overflow, Truncated, Stopped };
using ByteSink = bool (*)(const uint8_t*, size_t, void*);
using TextSink = bool (*)(const char*, size_t, void*);
enum class BodyMode : uint8_t { UntilClose, ContentLength, Chunked };

// Decodes an HTTP response body after the caller has checked status/headers.
// Reject conflicting framing headers and unsupported Content-Encoding before
// reset(). Chunk metadata is bounded too, so small chunks cannot hide an
// unbounded extension/trailer. No body data is delayed to fill a network read.
class HttpBodyDecoder {
 public:
  bool reset(BodyMode mode, uint32_t contentLength = 0, uint32_t maxBytes = 262144) {
    mode_ = mode;
    remaining_ = mode == BodyMode::ContentLength ? contentLength : 0;
    maxBytes_ = maxBytes;
    total_ = metadata_ = 0;
    lineLength_ = 0;
    error_ = Error::None;
    state_ = mode == BodyMode::Chunked ? State::Size : State::Data;
    done_ = mode == BodyMode::ContentLength && contentLength == 0;
    if (!maxBytes || (mode == BodyMode::ContentLength && contentLength > maxBytes))
      return fail(Error::Overflow);
    return true;
  }

  bool feed(const uint8_t* bytes, size_t length, ByteSink sink, void* context) {
    if (error_ != Error::None) return false;
    if ((!bytes && length) || !sink || (done_ && length)) return fail(Error::Invalid);
    size_t offset = 0;
    while (offset < length) {
      if (done_) return fail(Error::Invalid);
      if (state_ == State::Data) {
        size_t count = length - offset;
        if (mode_ != BodyMode::UntilClose && count > remaining_) count = remaining_;
        if (count > maxBytes_ - total_) return fail(Error::Overflow);
        if (count && !sink(bytes + offset, count, context)) return fail(Error::Stopped);
        offset += count;
        total_ += static_cast<uint32_t>(count);
        if (mode_ != BodyMode::UntilClose) {
          remaining_ -= static_cast<uint32_t>(count);
          if (!remaining_) {
            if (mode_ == BodyMode::ContentLength) done_ = true;
            else state_ = State::DataCR;
          }
        }
        continue;
      }
      const uint8_t value = bytes[offset++];
      if (++metadata_ > 65536) return fail(Error::Overflow);
      if (state_ == State::DataCR) {
        if (value != '\r') return fail(Error::Invalid);
        state_ = State::DataLF;
      } else if (state_ == State::DataLF) {
        if (value != '\n') return fail(Error::Invalid);
        state_ = State::Size;
      } else if (state_ == State::SizeLF || state_ == State::TrailerLF) {
        if (value != '\n') return fail(Error::Invalid);
        const bool trailer = state_ == State::TrailerLF;
        if (trailer) {
          if (!lineLength_) done_ = true;
          else state_ = State::Trailer;
        } else if (!parseSize()) return false;
        lineLength_ = 0;
      } else if (value == '\r') {
        state_ = state_ == State::Size ? State::SizeLF : State::TrailerLF;
      } else {
        if (value == '\n' || (value < 32 && value != '\t') || value == 127)
          return fail(Error::Invalid);
        const size_t limit = state_ == State::Size ? 96 : sizeof(line_);
        if (lineLength_ == limit) return fail(Error::Overflow);
        line_[lineLength_++] = static_cast<char>(value);
      }
    }
    return true;
  }

  bool finish() {
    if (error_ != Error::None) return false;
    if (mode_ == BodyMode::UntilClose) done_ = true;
    return done_ || fail(Error::Truncated);
  }
  bool done() const { return done_; }
  Error error() const { return error_; }
  uint32_t totalBytes() const { return total_; }

 private:
  enum class State : uint8_t { Size, SizeLF, Data, DataCR, DataLF, Trailer, TrailerLF };
  bool fail(Error value) { error_ = value; return false; }
  bool parseSize() {
    uint32_t value = 0;
    size_t index = 0;
    for (; index < lineLength_; ++index) {
      const char character = line_[index];
      unsigned digit;
      if (character >= '0' && character <= '9') digit = unsigned(character - '0');
      else if (character >= 'a' && character <= 'f') digit = unsigned(character - 'a' + 10);
      else if (character >= 'A' && character <= 'F') digit = unsigned(character - 'A' + 10);
      else break;
      if (value > (UINT32_MAX - digit) / 16) return fail(Error::Overflow);
      value = value * 16 + digit;
    }
    if (!index) return fail(Error::Invalid);
    while (index < lineLength_ && (line_[index] == ' ' || line_[index] == '\t')) ++index;
    if (index < lineLength_ && line_[index] != ';') return fail(Error::Invalid);
    if (value > maxBytes_ - total_) return fail(Error::Overflow);
    remaining_ = value;
    state_ = value ? State::Data : State::Trailer;
    return true;
  }
  BodyMode mode_ = BodyMode::UntilClose;
  State state_ = State::Data;
  Error error_ = Error::None;
  uint32_t remaining_ = 0, maxBytes_ = 262144, total_ = 0, metadata_ = 0;
  char line_[256]{};
  size_t lineLength_ = 0;
  bool done_ = false;
};

// One fixed event buffer, independent of TCP/TLS/HTTP chunk boundaries.
// Handles CR, LF, CRLF, initial UTF-8 BOM, comments and multiline data fields.
// EOF does not dispatch an unterminated event; the caller must require [DONE].
template <size_t Capacity = 4096>
class SseParser {
 public:
  void reset() {
    length_ = lineLength_ = fieldLength_ = 0;
    colon_ = dataField_ = skipSpace_ = skipLF_ = ended_ = false;
    bom_ = 0;
    error_ = Error::None;
  }
  bool feed(const uint8_t* bytes, size_t length, TextSink sink, void* context) {
    if (error_ != Error::None) return false;
    if ((!bytes && length) || !sink || ended_) return fail(Error::Invalid);
    for (size_t index = 0; index < length; ++index) {
      const uint8_t value = bytes[index];
      if (bom_ < 3) {
        if (bom_ == 0 && value != 0xEF) bom_ = 3;
        else {
          const uint8_t expected[] = {0xEF, 0xBB, 0xBF};
          if (value != expected[bom_++]) return fail(Error::Invalid);
          continue;
        }
      }
      if (skipLF_) { skipLF_ = false; if (value == '\n') continue; }
      if (value == '\r' || value == '\n') {
        skipLF_ = value == '\r';
        if (!lineEnd(sink, context)) return false;
        continue;
      }
      if (++lineLength_ > Capacity + 16) return fail(Error::Overflow);
      if (!colon_) {
        if (value == ':') {
          colon_ = true;
          dataField_ = fieldLength_ == 4 && memcmp(field_, "data", 4) == 0;
          skipSpace_ = true;
        } else {
          if (fieldLength_ < sizeof(field_)) field_[fieldLength_] = static_cast<char>(value);
          if (fieldLength_ < sizeof(field_) + 1) ++fieldLength_;
        }
      } else if (dataField_) {
        if (skipSpace_) { skipSpace_ = false; if (value == ' ') continue; }
        if (!append(static_cast<char>(value))) return false;
      }
    }
    return true;
  }
  bool finish() {
    if (error_ != Error::None) return false;
    if ((bom_ > 0 && bom_ < 3) || lineLength_ || length_) return fail(Error::Truncated);
    ended_ = true;
    return true;
  }
  Error error() const { return error_; }

 private:
  bool fail(Error value) { error_ = value; return false; }
  bool append(char value) {
    if (length_ == Capacity + 1) return fail(Error::Overflow);
    data_[length_++] = value;
    return true;
  }
  bool lineEnd(TextSink sink, void* context) {
    if (!lineLength_) {
      if (length_) {
        data_[--length_] = '\0';  // Remove the last data field's newline.
        if (!sink(data_, length_, context)) return fail(Error::Stopped);
        length_ = 0;
      }
    } else if (dataField_ || (!colon_ && fieldLength_ == 4 && memcmp(field_, "data", 4) == 0)) {
      if (!append('\n')) return false;
    }
    lineLength_ = fieldLength_ = 0;
    colon_ = dataField_ = skipSpace_ = false;
    return true;
  }
  static_assert(Capacity > 0, "SSE event capacity must be positive");
  char data_[Capacity + 2]{}, field_[5]{};
  size_t length_ = 0, lineLength_ = 0, fieldLength_ = 0;
  uint8_t bom_ = 0;
  Error error_ = Error::None;
  bool colon_ = false, dataField_ = false, skipSpace_ = false;
  bool skipLF_ = false, ended_ = false;
};

namespace detail {
inline bool whitespace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n'; }
inline bool punctuation(uint32_t cp) {
  return cp == '!' || cp == '?' || cp == '\n' || cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F;
}
inline bool joinsPrevious(uint32_t cp, uint32_t previous) {
  return (cp >= 0x0E30 && cp <= 0x0E3A) || cp == 0x0E45 ||
      (cp >= 0x0E47 && cp <= 0x0E4E) || (previous >= 0x0E40 && previous <= 0x0E44) ||
      (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
      (cp >= 0x1F3FB && cp <= 0x1F3FF) || cp == 0x200D || previous == 0x200D;
}
}  // namespace detail

// Incremental UTF-8 text grouping, without a Thai dictionary or heap allocation.
// Sentence/space boundaries are preferred; very long words use a bounded safe
// codepoint-cluster boundary. Thai dependent marks and leading vowels remain
// attached. This is not a full Unicode grapheme/Thai word segmentation engine.
// flush() supports a latency timer: it retains the last open cluster so a mark
// in the next delta cannot be orphaned. finish() flushes everything at [DONE].
template <size_t Capacity = 512>
class TextChunker {
 public:
  void reset() {
    length_ = clusterStart_ = clusters_ = preferred_ = preferredClusters_ = 0;
    have_ = need_ = 0;
    last_ = 0;
    sentence_ = ended_ = false;
    error_ = Error::None;
  }
  bool feed(const char* text, size_t length, TextSink sink, void* context) {
    if (error_ != Error::None) return false;
    if ((!text && length) || !sink || ended_) return fail(Error::Invalid);
    for (size_t index = 0; index < length; ++index) {
      const uint8_t value = static_cast<uint8_t>(text[index]);
      if (!have_) {
        if (value < 0x80) need_ = 1;
        else if (value >= 0xC2 && value <= 0xDF) need_ = 2;
        else if (value >= 0xE0 && value <= 0xEF) need_ = 3;
        else if (value >= 0xF0 && value <= 0xF4) need_ = 4;
        else return fail(Error::Invalid);
      } else if ((value & 0xC0) != 0x80) return fail(Error::Invalid);
      codepoint_[have_++] = value;
      if (have_ != need_) continue;
      uint32_t cp = decode(codepoint_, need_);
      if ((need_ == 2 && cp < 0x80) || (need_ == 3 && cp < 0x800) ||
          (need_ == 4 && cp < 0x10000) || cp > 0x10FFFF ||
          (cp >= 0xD800 && cp <= 0xDFFF) || (cp < 32 && !detail::whitespace(cp)))
        return fail(Error::Invalid);
      if (!append(cp, sink, context)) return false;
      have_ = need_ = 0;
    }
    return true;
  }
  bool flush(TextSink sink, void* context) {
    if (error_ != Error::None) return false;
    if (!sink || ended_) return fail(Error::Invalid);
    const size_t safe = (detail::whitespace(last_) || detail::punctuation(last_) || last_ == '.')
        ? length_ : clusterStart_;
    return !safe || emit(safe, sink, context);
  }
  bool finish(TextSink sink, void* context) {
    if (error_ != Error::None) return false;
    if (!sink || ended_) return fail(Error::Invalid);
    if (have_) return fail(Error::Truncated);
    if (length_ && !emit(length_, sink, context)) return false;
    ended_ = true;
    return true;
  }
  size_t pendingBytes() const { return length_ + have_; }
  Error error() const { return error_; }

 private:
  static uint32_t decode(const uint8_t* bytes, uint8_t count) {
    uint32_t cp = count == 1 ? bytes[0] : bytes[0] & (0x7F >> count);
    for (uint8_t index = 1; index < count; ++index) cp = (cp << 6) | (bytes[index] & 0x3F);
    return cp;
  }
  bool fail(Error value) { error_ = value; return false; }
  void remember(uint32_t cp, size_t start, size_t end) {
    if (!start || !detail::joinsPrevious(cp, last_)) { clusterStart_ = start; ++clusters_; }
    if (detail::whitespace(cp)) { preferred_ = end; preferredClusters_ = clusters_; }
    sentence_ = detail::punctuation(cp) || (detail::whitespace(cp) && (sentence_ || last_ == '.'));
    last_ = cp;
  }
  bool append(uint32_t cp, TextSink sink, void* context) {
    const bool independent = !detail::joinsPrevious(cp, last_);
    if (length_ && independent && !detail::whitespace(cp) && (sentence_ || clusters_ >= 48)) {
      const size_t cut = sentence_ || preferredClusters_ < 12 ? length_ : preferred_;
      if (!emit(cut, sink, context)) return false;
    }
    if (length_ + need_ > Capacity) {
      if (!clusterStart_) return fail(Error::Overflow);
      if (!emit(clusterStart_, sink, context)) return false;
      if (length_ + need_ > Capacity) return fail(Error::Overflow);
    }
    const size_t start = length_;
    memcpy(pending_ + length_, codepoint_, need_);
    length_ += need_;
    remember(cp, start, length_);
    return true;
  }
  bool emit(size_t count, TextSink sink, void* context) {
    const char saved = pending_[count];
    pending_[count] = '\0';
    const bool accepted = sink(pending_, count, context);
    pending_[count] = saved;
    if (!accepted) return fail(Error::Stopped);
    memmove(pending_, pending_ + count, length_ - count);
    length_ -= count;
    clusterStart_ = clusters_ = preferred_ = preferredClusters_ = 0;
    sentence_ = false;
    last_ = 0;
    for (size_t index = 0; index < length_;) {
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pending_ + index);
      const uint8_t size = bytes[0] < 0x80 ? 1 : bytes[0] < 0xE0 ? 2 : bytes[0] < 0xF0 ? 3 : 4;
      remember(decode(bytes, size), index, index + size);
      index += size;
    }
    return true;
  }
  static_assert(Capacity >= 8, "Text capacity must fit a UTF-8 cluster");
  char pending_[Capacity + 1]{};
  uint8_t codepoint_[4]{}, have_ = 0, need_ = 0;
  size_t length_ = 0, clusterStart_ = 0, clusters_ = 0, preferred_ = 0, preferredClusters_ = 0;
  uint32_t last_ = 0;
  Error error_ = Error::None;
  bool sentence_ = false, ended_ = false;
};

}  // namespace stream
}  // namespace byteplus
#endif
