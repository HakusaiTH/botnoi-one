/* Nonblocking TCP readiness for the local LGPL-2.1 WebSocket client fork. */
#ifndef WEBSOCKETS_WRITABLE_H_
#define WEBSOCKETS_WRITABLE_H_

#if defined(ESP32)
#include <lwip/sockets.h>
#else
#include <sys/select.h>
#endif

// Check before dequeuing application audio. A full send buffer is ordinary
// backpressure: leave the frame queued and continue servicing receive/audio.
// Readiness avoids the common congested write, but is not a TLS completion
// guarantee. No connection state is changed on a select error or a closed fd.
inline bool webSocketsSocketWritable(int socketFd) {
#if defined(LWIP_SELECT_MAXNFDS)
    if(socketFd < LWIP_SOCKET_OFFSET || socketFd >= LWIP_SELECT_MAXNFDS) return false;
#else
    if(socketFd < 0 || socketFd >= FD_SETSIZE) return false;
#endif
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socketFd, &writable);
    timeval timeout = { 0, 0 };
    const int result = select(socketFd + 1, nullptr, &writable, nullptr, &timeout);
    return result > 0 && FD_ISSET(socketFd, &writable);
}

#endif
