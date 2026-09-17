/**
 * @file WebSockets.cpp
 * @date 20.05.2015
 * @author Markus Sattler
 *
 * Copyright (c) 2015 Markus Sattler. All rights reserved.
 * This file is part of the WebSockets for Arduino.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "WebSockets.h"

#ifdef ESP8266
#include <core_esp8266_features.h>
#endif

extern "C" {
#ifdef CORE_HAS_LIBB64
#include <libb64/cencode.h>
#else
#include "libb64/cencode_inc.h"
#endif
}

#ifdef ESP8266
#include <Hash.h>
#elif defined(ESP32)
#include <esp_system.h>

#if ESP_IDF_VERSION_MAJOR >= 4
#if (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(1, 0, 6))
#include "sha/sha_parallel_engine.h"
#else
#include <esp32/sha.h>
#endif
#else
#include <hwcrypto/sha.h>
#endif

#else

extern "C" {
#include "libsha1/libsha1.h"
}

#endif

/**
 *
 * @param client WSclient_t *  ptr to the client struct
 * @param code uint16_t see RFC
 * @param reason ptr to the disconnect reason message
 * @param reasonLen length of the disconnect reason message
 */
void WebSockets::clientDisconnect(WSclient_t * client, uint16_t code, char * reason, size_t reasonLen) {
    DEBUG_WEBSOCKETS("[WS][%d][handleWebsocket] clientDisconnect code: %u\n", client->num, code);
    // Preserve peer/protocol failure classification through cleanup. A local
    // normal close must not replace the reason already observed from the peer.
    if(code && code != 1000) client->lastCloseCode = code;
    if(client->status == WSC_CONNECTED && code) {
        if(reason) {
            sendFrame(client, WSop_close, (uint8_t *)reason, reasonLen);
        } else {
            uint8_t buffer[2];
            buffer[0] = ((code >> 8) & 0xFF);
            buffer[1] = (code & 0xFF);
            sendFrame(client, WSop_close, &buffer[0], 2);
        }
    }
    clientDisconnect(client);
}

/**
 *
 * @param buf uint8_t *         ptr to the buffer for writing
 * @param opcode WSopcode_t
 * @param length size_t         length of the payload
 * @param mask bool             add dummy mask to the frame (needed for web browser)
 * @param maskkey uint8_t[4]    key used for payload
 * @param fin bool              can be used to send data in more then one frame (set fin on the last frame)
 */
uint8_t WebSockets::createHeader(uint8_t * headerPtr, WSopcode_t opcode, size_t length, bool mask, uint8_t maskKey[4], bool fin) {
    uint8_t headerSize;
    // calculate header Size
    if(length < 126) {
        headerSize = 2;
    } else if(length <= 0xFFFF) {
        headerSize = 4;
    } else {
        headerSize = 10;
    }

    if(mask) {
        headerSize += 4;
    }

    // create header

    // byte 0
    *headerPtr = 0x00;
    if(fin) {
        *headerPtr |= bit(7);    ///< set Fin
    }
    *headerPtr |= opcode;    ///< set opcode
    headerPtr++;

    // byte 1
    *headerPtr = 0x00;
    if(mask) {
        *headerPtr |= bit(7);    ///< set mask
    }

    if(length < 126) {
        *headerPtr |= length;
        headerPtr++;
    } else if(length <= 0xFFFF) {
        *headerPtr |= 126;
        headerPtr++;
        *headerPtr = ((length >> 8) & 0xFF);
        headerPtr++;
        *headerPtr = (length & 0xFF);
        headerPtr++;
    } else {
        // Normally we never get here (to less memory)
        *headerPtr |= 127;
        headerPtr++;
        *headerPtr = 0x00;
        headerPtr++;
        *headerPtr = 0x00;
        headerPtr++;
        *headerPtr = 0x00;
        headerPtr++;
        *headerPtr = 0x00;
        headerPtr++;
        *headerPtr = ((length >> 24) & 0xFF);
        headerPtr++;
        *headerPtr = ((length >> 16) & 0xFF);
        headerPtr++;
        *headerPtr = ((length >> 8) & 0xFF);
        headerPtr++;
        *headerPtr = (length & 0xFF);
        headerPtr++;
    }

    if(mask) {
        *headerPtr = maskKey[0];
        headerPtr++;
        *headerPtr = maskKey[1];
        headerPtr++;
        *headerPtr = maskKey[2];
        headerPtr++;
        *headerPtr = maskKey[3];
        headerPtr++;
    }
    return headerSize;
}

/**
 *
 * @param client WSclient_t *   ptr to the client struct
 * @param opcode WSopcode_t
 * @param payload uint8_t *     ptr to the payload
 * @param length size_t         length of the payload
 * @param fin bool              can be used to send data in more then one frame (set fin on the last frame)
 * @param headerToPayload bool  set true if the payload has reserved 14 Byte at the beginning to dynamically add the Header (payload neet to be in RAM!)
 * @return true if ok
 */
bool WebSockets::sendFrame(WSclient_t * client, WSopcode_t opcode, uint8_t * payload, size_t length, bool fin, bool headerToPayload) {
    if(!client->tcp || !client->tcp->connected() || client->status != WSC_CONNECTED ||
       (!payload && length) || length > WEBSOCKETS_MAX_DATA_SIZE) return false;
    if((opcode & 0x08) && (!fin || length > 125)) return false;

    // One bounded scratch buffer handles masking without modifying the source.
    // The common 640-byte microphone packet and header share one TLS write.
    uint8_t buffer[WebSocketsStream::kChunkSize + WEBSOCKETS_MAX_HEADER_SIZE];
    uint8_t maskKey[4] = { 0 };
    if(client->cIsClient) {
#if defined(ESP32)
        esp_fill_random(maskKey, sizeof(maskKey));
#else
        for(size_t i = 0; i < sizeof(maskKey); ++i) maskKey[i] = random(256);
#endif
    }
    const size_t headerSize = createHeader(buffer, opcode, length, client->cIsClient, maskKey, fin);
    const uint8_t * source = payload ? payload + (headerToPayload ? WEBSOCKETS_MAX_HEADER_SIZE : 0) : nullptr;
    size_t offset = 0;
    size_t prefix = headerSize;
    do {
        const size_t count = std::min(length - offset, static_cast<size_t>(WebSocketsStream::kChunkSize));
        for(size_t i = 0; i < count; ++i) {
            buffer[prefix + i] = source[offset + i] ^ (client->cIsClient ? maskKey[(offset + i) % 4] : 0);
        }
        if(write(client, buffer, prefix + count) != prefix + count) {
            // A partially written frame cannot be retried on this connection.
            // Close TCP directly: sending a close frame here would corrupt it.
            clientDisconnect(client);
            return false;
        }
        offset += count;
        prefix = 0;
    } while(offset < length);
    return true;
}

/**
 * callen when HTTP header is done
 * @param client WSclient_t *  ptr to the client struct
 */
void WebSockets::headerDone(WSclient_t * client) {
    client->status = WSC_CONNECTED;
    client->rx.reset();
    client->rxBackpressured = false;
    client->rxProgress = 0;
    client->lastCloseCode = 0;
    client->pendingPong.reset();
    client->httpLine = "";
    client->httpHeaderBytes = 0;
}

// Read only currently available bytes. Each loop consumes at most one parser
// boundary and <=640 payload bytes, leaving microphone/button work responsive.
void WebSockets::handleWebsocket(WSclient_t * client) {
    if(!client->tcp) return;
    const size_t capacity = binaryReceiveCapacity();
    client->rxBackpressured = client->rx.isReceivingBinary() && capacity == 0;
    const uint32_t now = millis();
    if(client->rx.timedOut(now, client->rxBackpressured)) {
        // Missing input is a transport stall, not proof of malformed framing.
        clientDisconnect(client);
        return;
    }
    const size_t wanted = client->rx.nextReadSize(capacity);
    const int available = client->tcp->available();
    if(!wanted || available <= 0) return;
    uint8_t chunk[WebSocketsStream::kChunkSize];
    const size_t count = std::min(wanted, static_cast<size_t>(available));
    const int read = client->tcp->read(chunk, count);
    if(read <= 0) return;
    ++client->rxProgress;
    WebSocketsStream::Event event;
    const uint16_t error = client->rx.consume(chunk, static_cast<size_t>(read), now, event);
    if(error) {
        clientDisconnect(client, error);
        return;
    }
    // No parser/TCP access after user callbacks: callbacks may disconnect.
    if(event.opcode == WSop_ping) {
        if(!client->pendingPong.queue(event.data, event.length)) {
            clientDisconnect(client, 1002);
            return;
        }
    } else if(event.opcode == WSop_pong) {
        client->pongReceived = true;
    } else if(event.opcode == WSop_close) {
        client->lastCloseCode = event.length >= 2
            ? (uint16_t(event.data[0]) << 8) | event.data[1] : 1005;
        sendFrame(client, WSop_close, event.data, event.length);
        clientDisconnect(client);
        return;
    }
    if(event.opcode) messageReceived(client, static_cast<WSopcode_t>(event.opcode), event.data, event.length, true);
}

/**
 * generate the key for Sec-WebSocket-Accept
 * @param clientKey String
 * @return String Accept Key
 */
String WebSockets::acceptKey(String & clientKey) {
    uint8_t sha1HashBin[20] = { 0 };
#ifdef ESP8266
    sha1(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", &sha1HashBin[0]);
#elif defined(ESP32)
    String data = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    esp_sha(SHA1, (unsigned char *)data.c_str(), data.length(), &sha1HashBin[0]);
#else
    clientKey += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const unsigned char *)clientKey.c_str(), clientKey.length());
    SHA1Final(&sha1HashBin[0], &ctx);
#endif

    String key = base64_encode(sha1HashBin, 20);
    key.trim();

    return key;
}

/**
 * base64_encode
 * @param data uint8_t *
 * @param length size_t
 * @return base64 encoded String
 */
String WebSockets::base64_encode(uint8_t * data, size_t length) {
    size_t size   = ((length * 1.6f) + 1);
    size          = std::max(size, (size_t)5);    // minimum buffer size
    char * buffer = (char *)malloc(size);
    if(buffer) {
        base64_encodestate _state;
        base64_init_encodestate(&_state);
        int len = base64_encode_block((const char *)&data[0], length, &buffer[0], &_state);
        len     = base64_encode_blockend((buffer + len), &_state);

        String base64 = String(buffer);
        free(buffer);
        return base64;
    }
    return String("-FAIL-");
}

/**
 * write x byte to tcp or get timeout
 * @param client WSclient_t *
 * @param out  uint8_t * data buffer
 * @param n size_t byte count
 * @return bytes send
 */
size_t WebSockets::write(WSclient_t * client, uint8_t * out, size_t n) {
    if(out == NULL)
        return 0;
    if(client == NULL)
        return 0;
    unsigned long t = millis();
    const unsigned long timeout = client->status == WSC_CONNECTED
                                      ? WEBSOCKETS_IO_TIMEOUT
                                      : WEBSOCKETS_TCP_TIMEOUT;
    size_t len      = 0;
    size_t total    = 0;
    DEBUG_WEBSOCKETS("[write] n: %zu t: %lu\n", n, t);
    while(n > 0) {
        if(client->tcp == NULL) {
            DEBUG_WEBSOCKETS("[write] tcp is null!\n");
            break;
        }

        if(!client->tcp->connected()) {
            DEBUG_WEBSOCKETS("[write] not connected!\n");
            break;
        }

        if((millis() - t) >= timeout) {
            DEBUG_WEBSOCKETS("[write] write TIMEOUT! %lu\n", (millis() - t));
            break;
        }

        len = client->tcp->write((const uint8_t *)out, n);
        if(len) {
            out += len;
            n -= len;
            total += len;
            // DEBUG_WEBSOCKETS("write %d left %d!\n", len, n);
        } else {
            DEBUG_WEBSOCKETS("WS write %d failed left %d!\n", len, n);
        }
        if(n > 0) {
            WEBSOCKETS_YIELD();
        }
    }
    WEBSOCKETS_YIELD();
    return total;
}

size_t WebSockets::write(WSclient_t * client, const char * out) {
    if(client == NULL)
        return 0;
    if(out == NULL)
        return 0;
    return write(client, (uint8_t *)out, strlen(out));
}

/**
 * enable ping/pong heartbeat process
 * @param client WSclient_t *
 * @param pingInterval uint32_t how often ping will be sent
 * @param pongTimeout uint32_t millis after which pong should timout if not received
 * @param disconnectTimeoutCount uint8_t how many timeouts before disconnect, 0=> do not disconnect
 */
void WebSockets::enableHeartbeat(WSclient_t * client, uint32_t pingInterval, uint32_t pongTimeout, uint8_t disconnectTimeoutCount) {
    if(client == NULL)
        return;
    client->pingInterval           = pingInterval;
    client->pongTimeout            = pongTimeout;
    client->disconnectTimeoutCount = disconnectTimeoutCount;
    client->pongReceived           = false;
}

/**
 * handle ping/pong heartbeat timeout process
 * @param client WSclient_t *
 */
void WebSockets::handleHBTimeout(WSclient_t * client) {
    if(client->pingInterval) {    // if heartbeat is enabled
        uint32_t pi = millis() - client->lastPing;

        if(client->pongReceived) {
            client->pongTimeoutCount = 0;
        } else {
            if(pi > client->pongTimeout) {    // pong not received in time
                client->pongTimeoutCount++;
                client->lastPing = millis() - client->pingInterval - 500;    // force ping on the next run

                DEBUG_WEBSOCKETS("[HBtimeout] pong TIMEOUT! lp=%d millis=%lu pi=%d count=%d\n", client->lastPing, millis(), pi, client->pongTimeoutCount);

                if(client->disconnectTimeoutCount && client->pongTimeoutCount >= client->disconnectTimeoutCount) {
                    DEBUG_WEBSOCKETS("[HBtimeout] count=%d, DISCONNECTING\n", client->pongTimeoutCount);
                    clientDisconnect(client);
                }
            }
        }
    }
}
