This directory contains the client-only subset of
[Links2004/arduinoWebSockets 2.7.2](https://github.com/Links2004/arduinoWebSockets/tree/2.7.2),
under LGPL-2.1, with the upstream license and libb64 notices retained. Original
file SHA-256 hashes are recorded in `upstream-sha256.json`. Source line endings
were normalized to LF.

The local changes are intentionally limited:

- Raise the ESP32 receive-frame limit from 15 KiB to 64 KiB. Real BytePlus
  streaming TTS packets exceed the upstream limit.
- Bound TCP reads/writes to 3 seconds and ESP32 TLS negotiation to 8 seconds.
- Disable library debug logging so authentication headers cannot be printed.
- Use a random ESP32 hardware-generated mask for every outgoing client frame,
  including payloads above the upstream 1,400-byte fast path; allocation or
  write failures are returned to the caller.

`../../cloud_socket.h` owns the connection and reassembles fragmented binary
messages with a 64 KiB total limit and a 12-second deadline. Ordinary complete
frames are delivered directly from the library allocation; they are not copied
into a second receive buffer. Fragmented messages need their partial assembly
plus the current frame. All socket operations belong to one task.

No installed WebSockets library is required for this sketch. Arduino recursively
compiles this `src` directory. The server and Socket.IO sources are omitted.
