#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

// Only the Arduino surface used by voicebot_client.h. ArduinoJson itself is
// compiled from the pinned, real library rather than mocked.
class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  String(const char* s, size_t length) : value_(s, length) {}
  String& operator=(const char* s) { value_ = s; return *this; }
  String& operator+=(const char* s) { value_ += s; return *this; }
  String& operator+=(char c) { value_ += c; return *this; }
  bool reserve(size_t size) { value_.reserve(size); return true; }
  size_t length() const { return value_.length(); }
  const char* c_str() const { return value_.c_str(); }
 private:
  std::string value_;
};

inline uint32_t fakeMillis = 0;
inline uint32_t millis() { return fakeMillis; }
struct TestSerial {
  std::string log;
  void println(const char* message) { log += message; log += '\n'; }
  void printf(const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    log += buffer;
  }
};
inline TestSerial Serial;
