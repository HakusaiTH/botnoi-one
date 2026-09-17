#include "../src/cloud_websockets/WebSocketsWritable.h"
#include "../src/cloud_websockets/WebSocketsControl.h"
#include "../src/cloud_websockets/WebSocketsStream.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static void receivePing(WebSocketsStream & parser, WebSocketsPendingPong & pending,
                        uint8_t * payload, size_t length) {
    assert(length <= 125);
    uint8_t header[2] = { 0x89, static_cast<uint8_t>(length) };
    WebSocketsStream::Event event;
    assert(parser.consume(header, sizeof(header), 1, event) == 0);
    if(length) assert(parser.consume(payload, length, 2, event) == 0);
    assert(event.opcode == 9);
    assert(pending.queue(event.data, event.length));
}

int main() {
    assert(!webSocketsSocketWritable(-1));
    assert(!webSocketsSocketWritable(FD_SETSIZE));

    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    for(int fd : sockets) assert(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
    const int bufferBytes = 4096;
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes)) == 0);
    assert(webSocketsSocketWritable(sockets[0]));

    // Stop the peer from consuming data, as happens during a congested TTS
    // response. The readiness check must defer instead of entering a write.
    const char payload[640] = {};
    size_t sent = 0;
    while(true) {
        const ssize_t result = send(sockets[0], payload, sizeof(payload), 0);
        if(result < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        assert(result > 0);
        sent += static_cast<size_t>(result);
    }
    assert(sent > 0);
    assert(!webSocketsSocketWritable(sockets[0]));

    WebSocketsStream parser;
    WebSocketsPendingPong pending;
    size_t writes = 0;
    auto writePong = [&](uint8_t * bytes, size_t length) {
        ++writes;
        return send(sockets[0], bytes, length, 0) == static_cast<ssize_t>(length);
    };
    uint8_t firstPing[3] = { 1, 0, 2 };
    receivePing(parser, pending, firstPing, sizeof(firstPing));
    assert(pending.flush(webSocketsSocketWritable(sockets[0]), writePong));
    assert(pending.pending() && writes == 0);
    uint8_t latestPing[125];
    for(size_t i = 0; i < sizeof(latestPing); ++i) latestPing[i] = static_cast<uint8_t>(i);
    receivePing(parser, pending, latestPing, sizeof(latestPing));
    // A later ping replaces the unsent one, preserving the exact binary payload.
    assert(pending.flush(webSocketsSocketWritable(sockets[0]), writePong));
    assert(pending.pending() && writes == 0);
    assert(!pending.queue(latestPing, 126));

    char received[1024];
    size_t drained = 0;
    while(true) {
        const ssize_t result = recv(sockets[1], received, sizeof(received), 0);
        if(result < 0) { assert(errno == EAGAIN || errno == EWOULDBLOCK); break; }
        assert(result > 0);
        drained += static_cast<size_t>(result);
    }
    assert(drained == sent);
    assert(webSocketsSocketWritable(sockets[0]));
    assert(pending.flush(webSocketsSocketWritable(sockets[0]), writePong));
    assert(!pending.pending() && writes == 1);
    assert(recv(sockets[1], received, sizeof(received), 0) == sizeof(latestPing));
    assert(memcmp(received, latestPing, sizeof(latestPing)) == 0);
    assert(pending.flush(true, writePong));
    assert(writes == 1); // A flushed pong must not be sent twice.

    receivePing(parser, pending, nullptr, 0);
    assert(pending.pending());
    assert(pending.flush(true, [&](uint8_t *, size_t length) { ++writes; return length == 0; }));
    assert(!pending.pending() && writes == 2);
    receivePing(parser, pending, firstPing, sizeof(firstPing));
    assert(!pending.flush(true, [](uint8_t *, size_t) { return false; }));
    pending.reset(); // Production disconnect resets the pending control state.
    assert(!pending.pending());
    static_assert(sizeof(WebSocketsPendingPong) <= 128, "Pong storage must remain bounded");

    const int closed = sockets[0];
    assert(close(sockets[0]) == 0);
    assert(!webSocketsSocketWritable(closed));
    assert(close(sockets[1]) == 0);
    std::puts("WebSocket control tests passed (blocked ping, latest replacement, drain/flush, empty ping, reset, socket readiness).");
}
