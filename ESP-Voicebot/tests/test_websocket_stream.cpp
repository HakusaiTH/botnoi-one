#include "../src/cloud_websockets/WebSocketsStream.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

static Bytes frame(uint8_t opcode, bool fin, const Bytes & payload) {
    Bytes wire = { static_cast<uint8_t>(opcode | (fin ? 0x80 : 0)) };
    const uint64_t size = payload.size();
    if(size < 126) {
        wire.push_back(static_cast<uint8_t>(size));
    } else if(size <= 65535) {
        wire.push_back(126);
        wire.push_back(static_cast<uint8_t>(size >> 8));
        wire.push_back(static_cast<uint8_t>(size));
    } else {
        wire.push_back(127);
        for(int shift = 56; shift >= 0; shift -= 8) wire.push_back(static_cast<uint8_t>(size >> shift));
    }
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

struct Capture {
    Bytes binary;
    std::vector<std::string> text;
    std::vector<uint8_t> control;
    size_t largestChunk = 0;

    void accept(const WebSocketsStream::Event & event) {
        if(event.opcode == 2) {
            largestChunk = std::max(largestChunk, event.length);
            binary.insert(binary.end(), event.data, event.data + event.length);
        } else if(event.opcode == 1) {
            assert(event.data[event.length] == 0);
            text.emplace_back(reinterpret_cast<const char *>(event.data), event.length);
        } else if(event.opcode) {
            control.push_back(event.opcode);
        }
    }
};

static uint16_t feed(WebSocketsStream & parser, Bytes wire, Capture & capture,
                     size_t networkChunk = 1024, size_t capacity = SIZE_MAX) {
    size_t offset = 0;
    while(offset < wire.size()) {
        const size_t amount = std::min({parser.nextReadSize(capacity), wire.size() - offset, networkChunk});
        assert(amount > 0);
        WebSocketsStream::Event event;
        const uint16_t error = parser.consume(wire.data() + offset, amount, 100, event);
        if(error) return error;
        offset += amount;
        capture.accept(event);
    }
    return 0;
}

static void large_frame_and_backpressure() {
    WebSocketsStream parser;
    Capture capture;
    Bytes pcm(768 * 1024 + 1);
    for(size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<uint8_t>((i * 37) ^ (i >> 8));
    Bytes wire = frame(2, true, pcm);

    // First two bytes and extended length arrive in separate TCP segments.
    WebSocketsStream::Event event;
    assert(parser.consume(wire.data(), 1, 1, event) == 0);
    assert(parser.consume(wire.data() + 1, 1, 2, event) == 0);
    assert(parser.consume(wire.data() + 2, 8, 3, event) == 0);
    assert(parser.isReceivingBinary());
    assert(parser.nextReadSize(0) == 0);
    assert(parser.nextReadSize(17) == 17);
    assert(!parser.timedOut(90000, true));
    assert(!parser.timedOut(90001, false));
    assert(parser.timedOut(105001, false));

    assert(feed(parser, Bytes(wire.begin() + 10, wire.end()), capture, 1037, 503) == 0);
    assert(capture.binary == pcm);
    assert(capture.largestChunk == 503);
    assert(!parser.isReceivingBinary());
    assert(!parser.inProgress());
    assert(!parser.timedOut(200000, false));
    static_assert(sizeof(WebSocketsStream) < 8704, "Decoder RAM must stay independent of frame size");
}

static void fragmented_binary_with_controls() {
    WebSocketsStream parser;
    Capture capture;
    const Bytes first = { 1, 2, 3 }, middle = { 4, 5 }, last = { 6, 7, 8, 9 };
    assert(feed(parser, frame(2, false, first), capture, 1) == 0);
    assert(parser.isReceivingBinary());
    assert(feed(parser, frame(9, true, { 4, 2 }), capture, 1, 0) == 0);
    assert(parser.isReceivingBinary());
    assert(feed(parser, frame(0, false, middle), capture, 1) == 0);
    assert(feed(parser, frame(10, true, {}), capture, 1, 0) == 0);
    assert(feed(parser, frame(0, true, last), capture, 1) == 0);
    assert(!parser.isReceivingBinary());
    assert(capture.binary == Bytes({ 1, 2, 3, 4, 5, 6, 7, 8, 9 }));
    assert(capture.control == Bytes({ 9, 10 }));
}

static void fragmented_text_and_limits() {
    WebSocketsStream parser;
    Capture capture;
    assert(feed(parser, frame(1, false, { '{', '"', 't', '"', ':' }), capture, 2) == 0);
    assert(capture.text.empty());
    assert(feed(parser, frame(9, true, { 'p' }), capture) == 0);
    assert(feed(parser, frame(0, true, { '1', '}' }), capture, 1) == 0);
    assert(capture.text == std::vector<std::string>({ "{\"t\":1}" }));
    assert(capture.binary.empty());
    assert(feed(parser, frame(1, true, Bytes(8192, 'a')), capture, 7) == 0);
    assert(capture.text.back().size() == 8192);
    assert(feed(parser, frame(1, true, Bytes(8193, 'a')), capture) == 1009);
    parser.reset();
    assert(feed(parser, frame(1, false, Bytes(8192, 'a')), capture) == 0);
    assert(feed(parser, frame(0, true, { 'b' }), capture) == 1009);
    parser.reset();
    // UTF-8 sequences may themselves be split across WebSocket fragments.
    assert(feed(parser, frame(1, false, { 0xe0, 0xb8 }), capture) == 0);
    assert(feed(parser, frame(0, true, { 0x81 }), capture) == 0);
    assert(capture.text.back() == "\xe0\xb8\x81");
    assert(feed(parser, frame(1, true, { 0xc0, 0x80 }), capture) == 1007);
}

static void invalid_frames_and_reset() {
    const std::vector<Bytes> invalid = {
        { 0x80, 0 },                     // continuation without an open message
        { 0x83, 0 },                     // reserved opcode
        { 0xc2, 0 },                     // unsupported RSV/compression
        { 0x82, 0x80 },                  // server masking is forbidden
        { 0x09, 0 },                     // fragmented control frame
        { 0x89, 126, 0, 126 },           // oversized control frame
        { 0x88, 1 },                     // close with half a status code
        { 0x82, 126, 0, 125 },           // nonminimal 16-bit length
        { 0x82, 127, 0, 0, 0, 0, 0, 0, 255, 255 }, // nonminimal 64-bit length
        { 0x82, 127, 128, 0, 0, 0, 0, 0, 0, 0 }   // forbidden sign bit
    };
    Capture capture;
    for(const Bytes & wire : invalid) {
        WebSocketsStream parser;
        assert(feed(parser, wire, capture, 1) == 1002);
        parser.reset();
        assert(!parser.inProgress());
        assert(feed(parser, frame(2, true, { 42 }), capture) == 0);
    }
    WebSocketsStream parser;
    assert(feed(parser, { 0x82, 127, 0, 0, 0, 1, 0, 0, 0, 0 }, capture) == 1009);
    parser.reset();
    assert(feed(parser, frame(2, false, { 1 }), capture) == 0);
    assert(feed(parser, frame(1, true, {}), capture) == 1002);
    parser.reset();
    assert(!parser.isReceivingBinary());
    assert(feed(parser, frame(1, true, {}), capture) == 0);
    assert(capture.text.back().empty());
    assert(feed(parser, frame(8, true, { 0x03, 0xed }), capture) == 1002); // forbidden 1005 status
    parser.reset();
    assert(feed(parser, frame(8, true, { 0x03, 0xe8, 0xed, 0xa0, 0x80 }), capture) == 1007); // surrogate UTF-8
}

static void time_wrap_and_truncation() {
    WebSocketsStream parser;
    WebSocketsStream::Event event;
    uint8_t byte = 0x82;
    assert(parser.consume(&byte, 1, UINT32_MAX - 500, event) == 0);
    assert(!parser.timedOut(100, false));
    assert(parser.timedOut(16000, false));
    parser.reset();
    Capture capture;
    assert(feed(parser, frame(2, false, {}), capture) == 0);
    assert(parser.isReceivingBinary());
    assert(!parser.timedOut(999999, true));
    assert(!parser.timedOut(1000000, false));
    assert(feed(parser, frame(0, true, {}), capture) == 0);
    assert(!parser.isReceivingBinary());
}

int main() {
    large_frame_and_backpressure();
    fragmented_binary_with_controls();
    fragmented_text_and_limits();
    invalid_frames_and_reset();
    time_wrap_and_truncation();
    std::puts("WebSocket stream tests passed (large frames, backpressure, fragments, controls, limits, reset, timeout).");
}
