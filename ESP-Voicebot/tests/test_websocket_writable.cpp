#include "../src/cloud_websockets/WebSocketsWritable.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

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

    const int closed = sockets[0];
    assert(close(sockets[0]) == 0);
    assert(!webSocketsSocketWritable(closed));
    assert(close(sockets[1]) == 0);
    std::puts("WebSocket writability tests passed (ready, full, drained, invalid, closed).");
}
