# ESP-Voicebot: ESP32-S3 Standalone Botnoi Voicebot Client

โปรเจกต์เฟิร์มแวร์สำหรับ **ESP32-S3** ที่เชื่อมต่อไปยัง **Botnoi Voicebot API** ผ่าน WebSocket SSL (`wss://`) ได้โดยตรงแบบ **Standalone** โดยไม่ต้องเปิดโปรแกรม Python Bridge บนคอมพิวเตอร์

---

## 🏗️ โครงสร้างการทำงาน

```text
[INMP441 (Mic)] ──(PCM 16kHz)──> ESP32-S3 ──(WebSocket SSL)──> Botnoi Voicebot API
                                    │                                  │
[MAX98357A (Spk)] <──(PCM 16kHz)────┴──────(TTS Audio PCM Frame)───────┘
```

- **ESP32-S3** รับเสียงจากไมโครโฟน INMP441 และแปลงเป็น PCM 16kHz 16-bit Mono
- ส่ง PCM Frame แบบ Real-time ผ่าน WebSocket SSL ตรงไปยัง Botnoi Voicebot Server
- รับสัญญาณเสียงสังเคราะห์ (TTS Audio Frame) กลับมา และเล่นออกลำโพง MAX98357A ผ่าน I2S
- รองรับสัญญาณ **Barge-in** (หยุดเล่นเสียงทันทีเมื่อผู้ใช้พูดแทรก)

---

## 🔌 การต่อสายฮาร์ดแวร์ (Hardware Pinouts)

| อุปกรณ์ | พินอุปกรณ์ | ESP32-S3 GPIO | หมายเหตุ |
| --- | --- | ---: | --- |
| **INMP441** (Mic) | BCLK / SCK | **GPIO3** | Microphone Bit Clock |
| **INMP441** (Mic) | WS / LRCLK | **GPIO2** | Word Select / Frame Sync |
| **INMP441** (Mic) | DOUT / SD | **GPIO1** | Data Out (ต่อ L/R ลง GND) |
| **MAX98357A** (Spk) | BCLK | **GPIO38** | Speaker Bit Clock |
| **MAX98357A** (Spk) | LRC | **GPIO39** | Left/Right Clock |
| **MAX98357A** (Spk) | DIN | **GPIO40** | Data In |
| **LED ภายนอก** | Anode (+) | **GPIO48** | ต่อผ่าน R 220–1kΩ (Cathode → GND) |
| **Button** | Signal | **GPIO46** | ใช้ `INPUT_PULLUP` (อีกขาต่อ GND) |

---

## 🛠️ ขั้นตอนการตั้งค่าและ Upload ผ่าน Arduino IDE

### 1. ติดตั้ง Board และ Library
1. เพิ่ม URL ใน Arduino IDE: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. ติดตั้ง Board: **esp32 by Espressif Systems** (เวอร์ชัน 3.x)
3. ติดตั้ง Library: **ArduinoJson** (เวอร์ชัน 7.x)

### 2. ตั้งค่า Wi-Fi และ Credentials
1. เปิดไฟล์ `config.example.h` แล้วคัดลอก/เปลี่ยนชื่อเป็น `config.local.h`
2. กรอกชื่อ Wi-Fi, รหัสผ่าน, `BOTNOI_API_KEY` และ `BOTNOI_AGENT_ID` ของคุณ:

```cpp
#define WIFI_SSID "ชื่อไวไฟของคุณ"
#define WIFI_PASS "รหัสผ่านไวไฟ"

#define BOTNOI_API_KEY "ak_xVKcjMpcwS2-F4U_n3KbEps1gMWR_WOC"
#define BOTNOI_AGENT_ID "agt_75d51d8b540d"
```

### 3. ตั้งค่า Board ใน Arduino IDE
- **Board**: `ESP32-S3 Dev Module`
- **Flash Size**: `16 MB (128Mb)`
- **PSRAM**: `OPI PSRAM`
- **USB CDC On Boot**: `Disabled` (หรือ Enabled ตามการใช้งาน Serial Monitor)

### 4. Flash และเปิดใช้งาน
1. เลือกพอร์ต COM ของบอร์ด ESP32-S3 และกด **Upload**
2. เมื่อแฟลชเสร็จ เปิด **Serial Monitor** ที่ความเร็ว **115200 Baud**
3. กดปุ่มที่ **GPIO46** เพื่อเปิด/ปิดการบันทึกเสียงสนทนา
