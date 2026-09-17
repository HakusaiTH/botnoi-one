# รายงานการตรวจสอบความละเอียดสูง: Botnoi Voicebot API Protocol Inspection

เอกสารฉบับนี้สรุปผลการวิเคราะห์และตรวจสอบการทำงานของ **Botnoi Voicebot API** ผ่านสัญญาณการสื่อสารจริง (Live WebSocket Inspection) เพื่อใช้อ้างอิงในการพัฒนาเฟิร์มแวร์ ESP32-S3 Standalone

---

## 1. ข้อมูลการเชื่อมต่อ (Endpoint Specification)

| รายการ | รายละเอียด |
| --- | --- |
| **Host** | `voicebot-stg.botnoigroup.com` |
| **Port** | `443` (TLS / SSL) |
| **Scheme** | `wss://` |
| **Path & Query** | `/v1/preview_call?api_key={BOTNOI_API_KEY}&agent_id={BOTNOI_AGENT_ID}` |
| **Reverse Proxy / CDN** | Cloudflare CDN (รองรับ TLS 1.2 / TLS 1.3) |

---

## 2. การสถาปนาการเชื่อมต่อ (HTTP 101 Handshake)

เมื่อ Client ส่งคำขอ Upgrade HTTP/1.1 เป็น WebSocket:

```http
GET /v1/preview_call?api_key=YOUR_BOTNOI_API_KEY&agent_id=YOUR_BOTNOI_AGENT_ID HTTP/1.1
Host: voicebot-stg.botnoigroup.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)
Origin: https://voicebot-stg.botnoigroup.com
```

### Server Response Headers:
```http
HTTP/1.1 101 Switching Protocols
Date: Wed, 16 Sep 2026 14:02:11 GMT
Connection: upgrade
Server: cloudflare
Upgrade: websocket
Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
cf-cache-status: DYNAMIC
```

---

## 3. ลำดับเหตุการณ์และข้อความ (Protocol Sequence & Lifecycle)

เมื่อเชื่อมต่อ WebSocket สำเร็จ ลำดับข้อความที่ Server และ Client ส่งหากันมีดังนี้:

```text
Client                                             Botnoi Server
  │                                                      │
  ├─────── HTTP 101 WebSocket Upgrade Request ──────────>│
  │<────── HTTP 101 Switching Protocols ────────────────┤
  │                                                      │
  │<────── JSON: type="opened" (Session ID Created) ─────┤
  │<────── JSON: type="event" (Bot Greeting Text 1) ─────┤
  │<────── JSON: type="event" (Bot Greeting Text 2) ─────┤
  │<────── Binary Frames (PCM16 16kHz Greeting TTS) ──────┤ (437,912 Bytes ~ 13.68s)
  │                                                      │
  ├─────── JSON: type="ping" (Every 20s Keep-alive) ────>│
  │<────── JSON: type="pong" ────────────────────────────┤
  │                                                      │
  ├─────── Binary Frames (User Mic PCM16 20ms/640B) ────>│ (While User Talking)
  ├─────── Binary Frames (500ms Silence Padding) ───────>│ (User Stops Talking)
  │                                                      │
  │<────── JSON: type="event" (user_turn_response) ──────┤
  │<────── JSON: type="event" (bot_turn_response) ───────┤
  │<────── Binary Frames (TTS PCM Audio Stream) ─────────┤
```

---

## 4. รายละเอียดโครงสร้าง JSON Messages

### 4.1. สัญญาณเปิด Session (`type: "opened"`)
ส่งทันทีเมื่อ WebSocket สถาปนาการเชื่อมต่อสำเร็จ:
```json
{
  "version": "2",
  "type": "opened",
  "seq": 1,
  "clientseq": 1,
  "id": "4ceb333b-acd9-4ab0-a402-3aaa02b9fe8e",
  "parameters": {
    "startPaused": false,
    "media": [
      {
        "type": "audio/L16",
        "sampleRateHz": 16000,
        "channels": ["external"]
      }
    ]
  }
}
```

### 4.2. ข้อความตอบกลับจาก บอท (`type: "event"`, entity: `"bot_turn_response"`)
Server ส่งข้อความทักทายอัตโนมัติทันที 2 ข้อความ:
```json
{
  "version": "2",
  "type": "event",
  "seq": 2,
  "clientseq": 1,
  "id": "4ceb333b-acd9-4ab0-a402-3aaa02b9fe8e",
  "parameters": {
    "entities": [
      {
        "type": "bot_turn_response",
        "data": {
          "output": "สวัสดีครับ ยินดีต้อนรับสู่บูธของบอทน้อยครับ!\n\n",
          "action": "None",
          "response_type": "text",
          "phone_transfer": null
        }
      }
    ]
  }
}
```

### 4.3. ข้อความตรวจจับเสียงพูดผู้ใช้ (`type: "event"`, entity: `"user_turn_response"`)
ส่งเมื่อ Server ถอดความเสียงพูดของผู้ใช้ (STT) สำเร็จ:
```json
{
  "version": "2",
  "type": "event",
  "seq": 5,
  "clientseq": 2,
  "id": "4ceb333b-acd9-4ab0-a402-3aaa02b9fe8e",
  "parameters": {
    "entities": [
      {
        "type": "user_turn_response",
        "data": {
          "transcript": {
            "result": {
              "text": "สวัสดีครับ"
            }
          },
          "is_final": true
        }
      }
    ]
  }
}
```

### 4.4. สัญญาณผู้ใช้พูดแทรก (`type: "event"`, entity: `"barge_in"`)
ส่งเมื่อผู้ใช้เริ่มพูดแทรกขณะที่บอทกำลังเล่นเสียงตอบกลับ:
```json
{
  "version": "2",
  "type": "event",
  "seq": 6,
  "clientseq": 3,
  "id": "4ceb333b-acd9-4ab0-a402-3aaa02b9fe8e",
  "parameters": {
    "entities": [
      {
        "type": "barge_in",
        "data": {}
      }
    ]
  }
}
```

---

## 5. การรับส่งข้อมูลเสียง (Audio Payload & Format Specifications)

| พารามิเตอร์ | ค่ากำหนด |
| --- | --- |
| **Codec / Format** | Raw PCM (Signed 16-bit Little-Endian) |
| **Sample Rate** | 16,000 Hz (16 kHz) |
| **Channels** | 1 Channel (Mono) |
| **Bit Rate** | 256 kbit/s (32,000 Bytes/sec) |
| **Frame Duration (Mic)** | 20 ms per frame = **640 Bytes** |
| **TTS Chunk Size (Bot)** | ทยอยส่งเป็นก้อน Binary Frame ละประมาณ **64,000 Bytes** (~2 วินาทีต่อก้อน) |
| **Greeting Audio Size** | รวม **437,912 Bytes** (~13.68 วินาที) |

---

## 6. Current firmware guidance (2026-09-17)

The wire examples above describe the historical inspection. The old advice to
allocate complete 256 KiB messages, fall back to internal malloc, disable TLS
verification, or ignore speaker queue overflow has been superseded.

The current firmware streams binary PCM in fixed 640-byte chunks, applies TCP
backpressure when its bounded queue is full, and validates the server using the
real GTS Root R4 certificate. It handles text/binary fragmentation separately,
uses one task for all socket operations, and sends ordered, paced trailing
silence. See [README.md](README.md) for current setup, RAM budgets and validation
limits. The 437,912-byte greeting documented here is also a host PCM regression
fixture size; that host test does not establish on-device audio success.
