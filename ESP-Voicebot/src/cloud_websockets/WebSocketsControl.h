/* Bounded control-message state for the local LGPL-2.1 client fork. */
#ifndef WEBSOCKETS_CONTROL_H_
#define WEBSOCKETS_CONTROL_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// RFC 6455 section 5.5.3 permits replying only to the latest outstanding ping.
// Retain its payload while TCP is congested instead of blocking the receive
// callback. All access belongs to the same task as WebSocketsClient::loop().
class WebSocketsPendingPong {
 public:
    bool queue(const uint8_t * payload, size_t length) {
        if(length > sizeof(payload_) || (!payload && length)) return false;
        if(length) memcpy(payload_, payload, length);
        length_ = static_cast<uint8_t>(length);
        pending_ = true;
        return true;
    }
    bool pending() const { return pending_; }
    void reset() { pending_ = false; length_ = 0; }

    // Exactly one write attempt, only when writable. A failed write must close
    // the transport; retrying a partially sent WebSocket frame is unsafe.
    template <typename Write>
    bool flush(bool writable, Write write) {
        if(!pending_ || !writable) return true;
        if(!write(payload_, static_cast<size_t>(length_))) return false;
        reset();
        return true;
    }

 private:
    uint8_t payload_[125];
    uint8_t length_ = 0;
    bool pending_ = false;
};

#endif
