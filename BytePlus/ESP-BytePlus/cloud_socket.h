#pragma once

#include <functional>
#include <cstdlib>
#include <cstring>
#include "src/cloud_websockets/WebSocketsClient.h"

// One owner task must perform all calls. Received bytes are valid only while the
// callback is running. Normal audio frames are delivered without another copy.
class CloudSocket : private WebSocketsClient {
 public:
  using BinaryCallback = std::function<void(const uint8_t*, size_t)>;
  static constexpr size_t MAX_MESSAGE_BYTES = 64 * 1024;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 12000;

  CloudSocket() {
    onEvent([this](WStype_t type, uint8_t* bytes, size_t length) {
      handleEvent(type, bytes, length);
    });
  }

  ~CloudSocket() { close(); }
  CloudSocket(const CloudSocket&) = delete;
  CloudSocket& operator=(const CloudSocket&) = delete;

  bool connect(const char* host, const char* path, const String& headers,
               const char* ca) {
    close();
    error_ = nullptr;
    if (!host || !*host || !path || path[0] != '/' || !ca || !*ca) {
      error_ = "invalid cloud connection configuration";
      return false;
    }
    // The client adds its own CRLF after extra headers and then User-Agent.
    // A trailing CRLF here would end HTTP headers before User-Agent is sent.
    size_t headerLength = headers.length();
    while (headerLength && (headers[headerLength - 1] == '\r' || headers[headerLength - 1] == '\n')) {
      --headerLength;
    }
    const String normalizedHeaders(headers.c_str(), static_cast<unsigned int>(headerLength));
    if (normalizedHeaders.length() != headerLength) {
      error_ = "insufficient memory for cloud headers";
      return false;
    }
    beginSslWithCA(host, 443, path, ca, "");
    setExtraHeaders(normalizedHeaders.c_str());
    setReconnectInterval(0);
    const uint32_t start = millis();
    while (!open_ && !error_ && millis() - start < CONNECT_TIMEOUT_MS) {
      // The vendored client also bounds TCP reads to 3 s and TLS to 8 s.
      // Trim a pending HTTP header read to the remaining connection budget.
      if (_client.tcp) {
        const uint32_t remaining = CONNECT_TIMEOUT_MS - (millis() - start);
        _client.tcp->setTimeout(remaining < 3000 ? remaining : 3000);
      }
      WebSocketsClient::loop();
      if (!open_) delay(1);
    }
    if (!open_) {
      if (!error_) error_ = "cloud connection timed out";
      close();
      return false;
    }
    _client.tcp->setTimeout(3000);
    enableHeartbeat(20000, 5000, 2);
    return true;
  }

  bool sendBinary(const uint8_t* bytes, size_t length) {
    if (!connected() || length > MAX_MESSAGE_BYTES || (!bytes && length)) {
      if (!error_) error_ = "cloud connection unavailable for send";
      return false;
    }
    if (!sendBIN(bytes, length)) {
      fail("cloud send failed");
      return false;
    }
    return true;
  }

  void poll(const BinaryCallback& callback) {
    if (!open_) return;
    callback_ = callback;
    WebSocketsClient::loop();
    callback_ = nullptr;
    if (fragmented_ && millis() - fragmentStart_ > CONNECT_TIMEOUT_MS) {
      fail("fragmented cloud message timed out");
    }
  }

  void close() {
    closing_ = true;
    open_ = false;
    _port = 0;  // An explicit owner decision is required before reconnecting.
    WebSocketsClient::disconnect();
    setExtraHeaders("");
    clearFragments();
    closing_ = false;
  }

  bool connected() { return open_ && WebSocketsClient::isConnected(); }
  const char* lastError() const { return error_ ? error_ : "none"; }

 private:
  BinaryCallback callback_;
  uint8_t* fragments_ = nullptr;
  size_t fragmentLength_ = 0;
  uint32_t fragmentStart_ = 0;
  bool fragmented_ = false;
  bool open_ = false;
  bool closing_ = false;
  const char* error_ = nullptr;

  void clearFragments() {
    free(fragments_);
    fragments_ = nullptr;
    fragmentLength_ = 0;
    fragmented_ = false;
  }

  void fail(const char* reason) {
    if (!error_) error_ = reason;
    close();
  }

  bool appendFragment(const uint8_t* bytes, size_t length) {
    if (length > MAX_MESSAGE_BYTES - fragmentLength_) {
      fail("cloud message exceeds 64 KiB");
      return false;
    }
    if (!length) return true;
    void* grown = realloc(fragments_, fragmentLength_ + length);
    if (!grown) {
      fail("insufficient memory for fragmented cloud message");
      return false;
    }
    fragments_ = static_cast<uint8_t*>(grown);
    memcpy(fragments_ + fragmentLength_, bytes, length);
    fragmentLength_ += length;
    return true;
  }

  void handleEvent(WStype_t type, uint8_t* bytes, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        open_ = true;
        break;
      case WStype_DISCONNECTED:
      case WStype_ERROR:
        open_ = false;
        clearFragments();
        if (!closing_ && !error_) error_ = "cloud connection closed or rejected";
        break;
      case WStype_BIN:
        if (fragmented_) {
          fail("unexpected cloud message during fragmentation");
        } else if (length > MAX_MESSAGE_BYTES) {
          fail("cloud message exceeds 64 KiB");
        } else if (callback_) {
          callback_(bytes, length);
        }
        break;
      case WStype_FRAGMENT_BIN_START:
        if (fragmented_) {
          fail("unexpected new fragmented cloud message");
          break;
        }
        fragmented_ = true;
        fragmentStart_ = millis();
        appendFragment(bytes, length);
        break;
      case WStype_FRAGMENT:
      case WStype_FRAGMENT_FIN:
        if (!fragmented_) {
          fail("unexpected cloud continuation frame");
        } else if (appendFragment(bytes, length) && type == WStype_FRAGMENT_FIN) {
          if (callback_) callback_(fragments_, fragmentLength_);
          clearFragments();
        }
        break;
      case WStype_TEXT:
      case WStype_FRAGMENT_TEXT_START:
        fail("unexpected text response from cloud audio service");
        break;
      default:
        break;  // Ping/pong control frames are handled by the client library.
    }
  }
};
