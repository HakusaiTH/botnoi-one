/* Bounded, incremental server-to-client framing for ESP-Voicebot.
 * Part of the local LGPL-2.1 arduinoWebSockets client fork. */
#ifndef WEBSOCKETS_STREAM_H_
#define WEBSOCKETS_STREAM_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class WebSocketsStream {
 public:
    // 640 bytes = 20 ms of 16 kHz mono PCM16, matching the speaker queue slot.
    enum : size_t { kChunkSize = 640, kTextLimit = 8192 };
    enum : uint32_t { kBinaryMessageLimit = 16UL * 1024 * 1024, kIdleTimeoutMs = 15000 };

    struct Event {
        uint8_t opcode = 0;
        uint8_t * data = nullptr;
        size_t length = 0;
    };

    void reset() {
        headerSize_ = 0;
        headerNeeded_ = 2;
        payload_ = false;
        opcode_ = 0;
        effectiveOpcode_ = 0;
        fragmentOpcode_ = 0;
        remaining_ = 0;
        messageLength_ = 0;
        textLength_ = 0;
        controlLength_ = 0;
        lastProgress_ = 0;
    }

    // A read must not cross the next parser boundary. In particular, ask the
    // application for space BEFORE removing binary payload bytes from TCP/TLS.
    size_t nextReadSize(size_t binaryCapacity) const {
        if(!payload_) return headerNeeded_ - headerSize_;
        size_t count = remaining_ < kChunkSize ? remaining_ : kChunkSize;
        if(effectiveOpcode_ == 2 && binaryCapacity < count) count = binaryCapacity;
        return count;
    }

    bool isReceivingBinary() const {
        return fragmentOpcode_ == 2 || (payload_ && effectiveOpcode_ == 2) ||
               (!payload_ && headerSize_ && (header_[0] & 0x0f) == 2);
    }

    bool inProgress() const { return headerSize_ || payload_ || fragmentOpcode_; }

    bool timedOut(uint32_t now, bool applicationPaused) {
        if(applicationPaused || !inProgress()) lastProgress_ = now;
        return uint32_t(now - lastProgress_) > kIdleTimeoutMs;
    }

    // Returns a WebSocket close code on invalid/oversized input, otherwise 0.
    // At most one event is produced. Its data is valid until the next read.
    uint16_t consume(uint8_t * bytes, size_t length, uint32_t now, Event & event) {
        event = Event();
        if(!length || length > nextReadSize(SIZE_MAX)) return 1002;
        lastProgress_ = now;
        if(!payload_) {
            memcpy(header_ + headerSize_, bytes, length);
            headerSize_ += length;
            if(headerSize_ < headerNeeded_) return 0;
            if(headerNeeded_ == 2) {
                const uint8_t len = header_[1] & 0x7f;
                // This client negotiates no extensions; servers must not mask.
                if((header_[0] & 0x70) || (header_[1] & 0x80)) return 1002;
                headerNeeded_ = len == 126 ? 4 : (len == 127 ? 10 : 2);
                if(headerSize_ < headerNeeded_) return 0;
            }
            const uint16_t error = decodeHeader();
            if(error) return error;
            if(!remaining_) return finish(event);
            return 0;
        }

        if(effectiveOpcode_ == 2) {
            event.opcode = 2;
            event.data = bytes;
            event.length = length;
        } else if(effectiveOpcode_ == 1) {
            memcpy(text_ + textLength_, bytes, length);
            textLength_ += length;
        } else {
            memcpy(control_ + controlLength_, bytes, length);
            controlLength_ += length;
        }
        remaining_ -= length;
        if(!remaining_) return finish(event);
        return 0;
    }

 private:
    uint16_t decodeHeader() {
        fin_ = (header_[0] & 0x80) != 0;
        opcode_ = header_[0] & 0x0f;
        const uint8_t shortLength = header_[1] & 0x7f;
        uint64_t length = shortLength;
        if(shortLength >= 126) {
            length = 0;
            for(size_t i = 2; i < headerNeeded_; ++i) length = (length << 8) | header_[i];
            if((shortLength == 126 && length < 126) ||
               (shortLength == 127 && (length < 65536 || (header_[2] & 0x80)))) return 1002;
        }
        if(opcode_ == 8 || opcode_ == 9 || opcode_ == 10) {
            if(!fin_ || length > 125 || (opcode_ == 8 && length == 1)) return 1002;
            effectiveOpcode_ = opcode_;
            controlLength_ = 0;
        } else if(opcode_ == 0 || opcode_ == 1 || opcode_ == 2) {
            if(opcode_ == 0) {
                if(!fragmentOpcode_) return 1002;
                effectiveOpcode_ = fragmentOpcode_;
            } else {
                if(fragmentOpcode_) return 1002;
                effectiveOpcode_ = opcode_;
                textLength_ = 0;
                messageLength_ = 0;
                if(!fin_) fragmentOpcode_ = opcode_;
            }
            const uint32_t limit = effectiveOpcode_ == 1 ? static_cast<uint32_t>(kTextLimit) : static_cast<uint32_t>(kBinaryMessageLimit);
            if(length > limit || messageLength_ > limit - length) return 1009;
            messageLength_ += static_cast<uint32_t>(length);
        } else {
            return 1002;
        }
        remaining_ = static_cast<size_t>(length);
        payload_ = true;
        return 0;
    }

    static bool validUtf8(const uint8_t * text, size_t length) {
        size_t i = 0;
        while(i < length) {
            const uint8_t lead = text[i++];
            if(lead < 0x80) continue;
            size_t rest;
            uint32_t value;
            uint32_t minimum;
            if(lead >= 0xc2 && lead <= 0xdf) { rest = 1; value = lead & 0x1f; minimum = 0x80; }
            else if(lead >= 0xe0 && lead <= 0xef) { rest = 2; value = lead & 0x0f; minimum = 0x800; }
            else if(lead >= 0xf0 && lead <= 0xf4) { rest = 3; value = lead & 0x07; minimum = 0x10000; }
            else return false;
            if(rest > length - i) return false;
            while(rest--) {
                const uint8_t next = text[i++];
                if((next & 0xc0) != 0x80) return false;
                value = (value << 6) | (next & 0x3f);
            }
            if(value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
        }
        return true;
    }

    uint16_t finish(Event & event) {
        if(effectiveOpcode_ == 1 && fin_) {
            if(!validUtf8(text_, textLength_)) return 1007;
            text_[textLength_] = 0;
            event.opcode = 1;
            event.data = text_;
            event.length = textLength_;
        } else if(effectiveOpcode_ >= 8) {
            if(effectiveOpcode_ == 8 && controlLength_ >= 2) {
                const uint16_t code = (uint16_t(control_[0]) << 8) | control_[1];
                if(code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015) return 1002;
                if(!validUtf8(control_ + 2, controlLength_ - 2)) return 1007;
            }
            event.opcode = effectiveOpcode_;
            event.data = control_;
            event.length = controlLength_;
        }
        if(effectiveOpcode_ <= 2 && fin_) fragmentOpcode_ = 0;
        headerSize_ = 0;
        headerNeeded_ = 2;
        payload_ = false;
        return 0;
    }

    uint8_t header_[10];
    uint8_t text_[kTextLimit + 1];
    uint8_t control_[125];
    size_t headerSize_ = 0;
    size_t headerNeeded_ = 2;
    size_t remaining_ = 0;
    size_t textLength_ = 0;
    size_t controlLength_ = 0;
    uint32_t messageLength_ = 0;
    uint32_t lastProgress_ = 0;
    uint8_t opcode_ = 0;
    uint8_t effectiveOpcode_ = 0;
    uint8_t fragmentOpcode_ = 0;
    bool payload_ = false;
    bool fin_ = false;
};

#endif
