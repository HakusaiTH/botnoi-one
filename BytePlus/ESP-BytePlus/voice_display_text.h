#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace voice_display {
namespace text {
constexpr size_t kTextCapacity = 2049;
constexpr size_t kMaxLines = 128;

struct Line { uint16_t start = 0, length = 0; };
struct Layout {
  Line lines[kMaxLines];
  uint16_t count = 0;
  bool truncated = false;
};

namespace detail {
// Return zero for malformed UTF-8. Read continuation bytes only up to NUL.
inline size_t decode(const char* text, uint32_t& codepoint) {
  const uint8_t first = static_cast<uint8_t>(text[0]);
  if (first < 0x80) { codepoint = first; return first ? 1 : 0; }
  size_t length = first >= 0xc2 && first <= 0xdf ? 2 :
                  first >= 0xe0 && first <= 0xef ? 3 :
                  first >= 0xf0 && first <= 0xf4 ? 4 : 0;
  if (!length) return 0;
  codepoint = first & (0x7f >> length);
  for (size_t i = 1; i < length; ++i) {
    const uint8_t byte = static_cast<uint8_t>(text[i]);
    if (byte < 0x80 || byte > 0xbf) return 0;
    codepoint = (codepoint << 6) | (byte & 0x3f);
  }
  if ((length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000) ||
      codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return 0;
  return length;
}

inline bool combining(uint32_t cp) {
  return (cp >= 0x0300 && cp <= 0x036f) || cp == 0x0e31 || cp == 0x0e33 ||
         (cp >= 0x0e34 && cp <= 0x0e3a) || (cp >= 0x0e47 && cp <= 0x0e4e) ||
         (cp >= 0xfe00 && cp <= 0xfe0f);
}

inline size_t clusterLength(const char* value) {
  uint32_t cp = 0;
  size_t length = decode(value, cp);
  if (!length) return *value ? 1 : 0;
  // A Thai leading vowel should stay on the same line as its consonant.
  if (cp >= 0x0e40 && cp <= 0x0e44) {
    uint32_t next = 0;
    const size_t bytes = decode(value + length, next);
    if (bytes && next >= 0x0e01 && next <= 0x0e2e) length += bytes;
  }
  while (value[length]) {
    uint32_t next = 0;
    const size_t bytes = decode(value + length, next);
    if (!bytes || !combining(next)) break;
    length += bytes;
  }
  return length;
}
}  // namespace detail

// Sanitize without heap allocation. The selected U8g2 font renderer addresses
// BMP glyphs, so supplementary codepoints become '?'. Preserve UTF-8 boundaries
// when adding a truncation marker. Source and destination must not overlap.
inline size_t copyUtf8(char* output, size_t capacity, const char* input) {
  if (!output || !capacity) return 0;
  output[0] = '\0';
  if (!input) return 0;
  size_t used = 0, consumed = 0;
  // Do not scan unbounded caller text, even if it contains only control bytes.
  const size_t inputLimit = kTextCapacity * 2;
  while (input[consumed] && consumed < inputLimit) {
    uint32_t cp = 0;
    size_t bytes = detail::decode(input + consumed, cp);
    char substitute = '?';
    const char* source = input + consumed;
    size_t written = bytes;
    if (!bytes || cp > 0xffff) { bytes = bytes ? bytes : 1; written = 1; source = &substitute; }
    else if (cp == '\r') { ++consumed; continue; }
    else if (cp == '\t' || (cp < 32 && cp != '\n')) { substitute = ' '; written = 1; source = &substitute; }
    if (written > capacity - 1 - used) break;
    memcpy(output + used, source, written);
    used += written;
    consumed += bytes;
  }
  output[used] = '\0';
  if (input[consumed] && capacity >= 4) {
    while (used > capacity - 4) {
      --used;
      while (used && (static_cast<uint8_t>(output[used]) & 0xc0) == 0x80) --used;
    }
    memcpy(output + used, "...", 4);
    used += 3;
  }
  return used;
}

// ETL14 Thai is a cell font: even dependent marks have a seven-pixel advance.
// Use the same positioning for measuring and drawing instead of drawUTF8,
// which would move the pen after every mark. The callback receives positions
// relative to the line origin and returns the glyph's encoded advance.
// This handles common Thai mark placement, not contextual OpenType shaping.
inline int16_t glyphRuns(const char* value,
                         int16_t (*glyph)(uint16_t, int16_t, int16_t, void*),
                         void* context) {
  if (!value || !glyph) return 0;
  int16_t cursor = 0, base = 0;
  bool haveBase = false;
  size_t consumed = 0;
  while (consumed < kTextCapacity - 1 && value[consumed]) {
    uint32_t cp = 0;
    size_t bytes = detail::decode(value + consumed, cp);
    if (!bytes || cp > 0xffff) { cp = '?'; bytes = bytes ? bytes : 1; }
    if (bytes > kTextCapacity - 1 - consumed) break;
    consumed += bytes;
    const bool dependent = cp == 0x0e31 || (cp >= 0x0e34 && cp <= 0x0e3a) ||
                           (cp >= 0x0e47 && cp <= 0x0e4e);
    if (dependent && haveBase) {
      glyph(static_cast<uint16_t>(cp), base, 0, context);
      continue;
    }
    if (cp == 0x0e33 && haveBase) {
      // SARA AM contains an above-base NIKHAHIT and a spacing SARA AA.
      // The ETL14 NIKHAHIT bitmap normally sits in the tone-mark row; lower
      // it three pixels here so a preceding tone mark remains visible.
      glyph(0x0e4d, base, 3, context);
      cp = 0x0e32;
    }
    const int16_t advance = glyph(static_cast<uint16_t>(cp), cursor, 0, context);
    base = cursor;
    haveBase = cp >= 0x0e01 && cp <= 0x0e2e;
    if (advance > 0) {
      if (advance > INT16_MAX - cursor) return INT16_MAX;
      cursor = static_cast<int16_t>(cursor + advance);
    }
  }
  return cursor;
}

// Offsets address the sanitized input. English prefers whitespace; Thai wraps
// between grapheme-like clusters, preserving dependent marks. This is not a
// Thai dictionary word breaker or a full Unicode shaping engine.
inline void wrap(const char* value, int16_t maxWidth,
                 int16_t (*measure)(const char*, void*), void* context, Layout& layout) {
  layout.count = 0;
  layout.truncated = false;
  if (!value || !*value || maxWidth <= 0 || !measure) return;
  size_t total = 0;
  while (total < kTextCapacity - 1 && value[total]) ++total;
  char candidate[kTextCapacity];
  size_t start = 0;
  while (start < total && layout.count < kMaxLines) {
    while (start < total && value[start] == ' ') ++start;
    if (start == total) break;
    if (value[start] == '\n') {
      layout.lines[layout.count++] = Line{static_cast<uint16_t>(start), 0};
      ++start;
      continue;
    }
    size_t end = start, lastSpace = start;
    while (end < total && value[end] != '\n') {
      const size_t cluster = detail::clusterLength(value + end);
      if (!cluster || cluster > total - end) break;
      const size_t trial = end + cluster;
      memcpy(candidate, value + start, trial - start);
      candidate[trial - start] = '\0';
      if (measure(candidate, context) > maxWidth && end > start) break;
      if (value[end] == ' ') lastSpace = end;
      end = trial;
      if (measure(candidate, context) > maxWidth) break;  // One oversized cluster; always progress.
    }
    if (end < total && value[end] != '\n' && lastSpace > start) end = lastSpace;
    if (end == start) end += 1;  // Malformed input cannot cause an infinite loop.
    size_t visible = end;
    while (visible > start && value[visible - 1] == ' ') --visible;
    layout.lines[layout.count++] = Line{static_cast<uint16_t>(start), static_cast<uint16_t>(visible - start)};
    start = end;
    if (start < total && value[start] == '\n') ++start;
  }
  layout.truncated = start < total;
}

inline uint16_t pageCount(const Layout& layout, uint8_t linesPerPage) {
  if (!linesPerPage || !layout.count) return 1;
  return static_cast<uint16_t>((layout.count + linesPerPage - 1) / linesPerPage);
}
}  // namespace text
}  // namespace voice_display
