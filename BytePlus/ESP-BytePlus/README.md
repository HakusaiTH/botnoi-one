# ESP-BytePlus: ESP32 voicebot แบบ standalone

**หลังแฟลชแล้ว ให้จ่ายไฟจากอะแดปเตอร์ USB หรือ power bank แล้วกดปุ่มพูดได้เลย** ESP32 รับเสียงจากไมโครโฟน เรียก BytePlus และเล่นคำตอบผ่านลำโพงเอง ต้องมี **Wi-Fi ที่เชื่อมต่ออินเทอร์เน็ต** ค่าเริ่มต้นใช้ภาษาไทยและเสียง Mildred

ขณะใช้งานไม่ต้องต่อคอมพิวเตอร์ เปิด Python หรือเปิดเบราว์เซอร์ คอมพิวเตอร์ใช้สำหรับแก้เฟิร์มแวร์และแฟลชครั้งแรกเท่านั้น การตั้งค่าหลักด้านล่างทำผ่าน Arduino IDE ได้โดยไม่ต้องรันสคริปต์ Python ของโปรเจกต์

```text
INMP441 → ESP32 → SeedASR (WebSocket) → ข้อความผู้ใช้
                     ↓
                ModelArk (HTTPS / SSE)
                     ↓ ข้อความคำตอบที่ทยอยสร้าง
              BytePlus TTS (WebSocket)
                     ↓ เสียงที่ทยอยสังเคราะห์
                 ESP32 → MAX98357A
```

ESP32 เป็นผู้เรียกทั้งสามบริการผ่าน Wi-Fi โดยตรง เสียงไม่ได้ส่งผ่าน Python หรือสาย serial ของคอมพิวเตอร์ Serial Monitor ใช้อ่านข้อความวินิจฉัยเท่านั้น

คำตอบใช้ streaming จากโมเดลถึง TTS: ESP32 เปิด session สังเคราะห์เสียง แล้วส่งข้อความที่ ModelArk ทยอยสร้างให้ TTS พร้อมรับเสียงกลับไปเล่น จึงเริ่มรับเสียงได้ก่อนโมเดลสร้างคำตอบครบ ทั้งนี้เวลารอจริงขึ้นกับการตรวจจบประโยค เครือข่าย และบริการ cloud

## ใช้งานด้วยไฟเลี้ยงอย่างเดียว

1. ถอดสาย USB ที่ต่อกับคอมพิวเตอร์ทั้งหมด
2. จ่ายไฟให้บอร์ดจากอะแดปเตอร์ USB หรือ power bank ผ่านพอร์ตจ่ายไฟ/USB-to-UART ของบอร์ด โดยใช้ Wi-Fi ที่ตั้งค่าไว้ในเฟิร์มแวร์
3. ปล่อยปุ่มระหว่างเริ่มต้น รอประมาณ 30 วินาทีให้บอร์ดเชื่อม Wi-Fi และซิงก์เวลา เครือข่ายช้าอาจใช้เวลานานกว่านี้ จากนั้น **กดปุ่มหนึ่งครั้งเพื่อเริ่มสนทนา** ไม่ต้องกดค้าง
4. พูดตามปกติแล้วเว้นจังหวะสั้นๆ บอร์ดจะประมวลผลและตอบกลับ จากนั้นเปิดฟังประโยคถัดไปอัตโนมัติ
5. **กดปุ่มเดิมอีกครั้งเพื่อหยุด** ได้ทุกช่วง รวมถึงขณะเล่นคำตอบ หากไม่มีเสียงพูดที่ตรวจพบระหว่างเปิดฟังครบ **30 วินาที** ระบบหยุด session อัตโนมัติ ต้องกดปุ่มอีกครั้งเพื่อเริ่มใหม่

**ผู้ใช้ยืนยันแล้วว่าบอร์ดทำงานแบบ standalone โดยไม่ต้องต่อคอมพิวเตอร์** การทดสอบก่อนหน้านี้ยืนยันเส้นทางเสียงบนบอร์ดถึง BytePlus ขณะต่อ USB เพื่ออ่าน log รายละเอียดผลอยู่ในหัวข้อการตรวจสอบ

## ฮาร์ดแวร์

บอร์ดที่ตรวจพบและแฟลชสำเร็จเป็น **ESP32-S3 revision 0.2, flash 16 MB และ PSRAM 8 MB** (N16R8) ใช้ quad flash และ OPI PSRAM 3.3 V หากใช้บอร์ดอื่นให้ตรวจรุ่น ขา และการตั้งค่า flash/PSRAM ให้ตรงกับฮาร์ดแวร์

| อุปกรณ์ | ขาอุปกรณ์ | ESP32-S3 GPIO | หมายเหตุ |
| --- | --- | ---: | --- |
| ST7735 TFT | SCK | **GPIO42** | Hardware SPI Clock |
| ST7735 TFT | MOSI | **GPIO41** | Hardware SPI Data |
| ST7735 TFT | DC | **GPIO45** | Data / Command |
| ST7735 TFT | CS | **GPIO47** | Chip Select |
| ST7735 TFT | RESET | **GPIO14** | Reset |
| ST7735 TFT | LED/BL | **GPIO21** | Backlight |
| INMP441 | BCLK / SCK | **GPIO3** | Microphone Bit Clock |
| INMP441 | WS / LRCLK | **GPIO2** | Word Select / Frame Sync |
| INMP441 | DOUT / SD | **GPIO1** | Data Out |
| MAX98357A | BCLK | **GPIO38** | Speaker Bit Clock |
| MAX98357A | LRC | **GPIO39** | Left/Right Clock |
| MAX98357A | DIN | **GPIO40** | Data In |
| LED ภายนอก | Anode (+) | **GPIO48** | ต่อผ่าน R 220–1kΩ (Cathode → GND) |
| Button | Signal / ขาหนึ่ง | **GPIO46** | Input only (`INPUT_PULLUP`, อีกขา → GND) |

ต่อ GND ร่วมกัน ใช้ไฟเลี้ยงตามข้อกำหนดของโมดูล และต่อ INMP441 **L/R ลง GND** เพื่อเลือกช่องซ้าย เฟิร์มแวร์อ่านไมโครโฟน I2S 32-bit แล้วแปลงเป็น PCM16 ส่วนลำโพงส่งเสียงเดียวกันทั้งสองช่อง I2S stereo ด้วย gain 60%

## ตั้งค่าด้วย Arduino IDE

1. เปิด `ESP-BytePlus.ino` ใน Arduino IDE
2. คัดลอก [config.example.h](config.example.h) เป็น `config.local.h` ในโฟลเดอร์เดียวกับสเก็ตช์
3. ใส่ค่า Wi-Fi และ BytePlus ของคุณแทนช่องว่างใน `config.local.h`
4. เลือกบอร์ดและ upload ตามหัวข้อถัดไป หลังแก้ Wi-Fi หรือ credentials ต้อง build และแฟลชใหม่

ค่าที่ต้องกรอกคือ `WIFI_SSID`, `WIFI_PASS` (เว้นว่างได้สำหรับเครือข่ายเปิด), `BYTEPLUS_ASR_APP_ID`, `BYTEPLUS_ASR_ACCESS_TOKEN`, `BYTEPLUS_ARK_ENDPOINT_ID`, `BYTEPLUS_ARK_API_KEY`, `BYTEPLUS_TTS_APP_ID` และ `BYTEPLUS_TTS_TOKEN` ไฟล์ `config.local.h` ถูก git-ignore เก็บไฟล์นี้ไว้เป็นส่วนตัวและไม่ commit

ค่าเริ่มต้นใน template เป็นภาษาไทย ใช้เสียง **Mildred** (`th_female_bv568_neutral_uranus_bigtts`) กับ resource `seed-tts-2.0` คู่นี้ผ่านการเรียกบริการจริงแล้ว หากต้องการภาษาอังกฤษ ให้เปลี่ยนสามบรรทัดใน `config.local.h` เป็น:

```cpp
#define BYTEPLUS_LANGUAGE "en"
#define BYTEPLUS_TTS_VOICE "en_female_anna_mars_bigtts"
#define BYTEPLUS_TTS_RESOURCE_ID "volc.service_type.1000009"
```

หากเลือกเสียงอื่น ต้องใช้ resource ให้ตรงกับโมเดลและสิทธิ์ของเสียงนั้น ดู [รายชื่อเสียง BytePlus](https://docs.byteplus.com/en/docs/byteplusvoice/tts-voice-list)

## Build และ upload ด้วย Arduino IDE

1. เพิ่ม `https://espressif.github.io/arduino-esp32/package_esp32_index.json` ใน Additional Boards Manager URLs แล้วติดตั้ง **esp32 by Espressif Systems 3.3.11** จาก Boards Manager
2. ติดตั้ง **ArduinoJson 7.4.3**, **Adafruit ST7735 and ST7789 Library 1.11.0**, **Adafruit GFX Library 1.12.6**, **Adafruit BusIO 1.17.4** และ **U8g2_for_Adafruit_GFX 1.8.0** จาก Library Manager
3. เลือก **ESP32-S3 Dev Module** สำหรับบอร์ด N16R8 ที่ทดสอบให้เลือก **Flash Size: 16 MB**, **PSRAM: OPI PSRAM**, **USB CDC On Boot: Disabled** และใช้ partition scheme เริ่มต้น
4. ต่อพอร์ต USB-to-UART เลือก serial port ของบอร์ด แล้วกด **Upload**
5. เมื่อต้องการอ่าน log ให้เปิด Serial Monitor ที่ **115200 baud** หลังแฟลชเสร็จสามารถถอดคอมพิวเตอร์แล้วเปลี่ยนเป็นอะแดปเตอร์ USB หรือ power bank ได้

ซอร์ส arduinoWebSockets **2.7.2** อยู่ใน `src/cloud_websockets/` แล้ว จึงไม่ต้องติดตั้ง WebSockets เพิ่ม สำเนานี้ปรับขอบเขตข้อความเป็น **64 KiB** และจำกัดเวลารอ เพราะ TTS ที่ทดสอบจริงส่ง audio chunk ใหญ่กว่า 15 KiB ดูที่มา ใบอนุญาต และรายละเอียด patch ใน [vendored client](src/cloud_websockets/README.md)

## จอ TFT ST7735

จอขนาด **128×160 แนวตั้ง** แสดงสถานะเชื่อมต่อ Wi-Fi, ซิงก์เวลา, พร้อมพูด, กำลังฟัง, รู้จำเสียง, คิดคำตอบ และเล่นเสียง พร้อมข้อความสนทนาภาษาไทยหรืออังกฤษ ข้อความยาวเปลี่ยนหน้าอัตโนมัติ และคำตอบล่าสุดยังอยู่บนจอเมื่อจบรอบ

จอใช้ hardware SPI ตามขาในตาราง และ task ลำดับความสำคัญต่ำแยกจากงานเสียง การส่งสถานะจากงานเสียงไม่รอให้วาดจอเสร็จ ค่าเริ่มต้นใช้ ST7735 black-tab, rotation 0 และ SPI 16 MHz ปรับชนิด panel, rotation และขาได้ใน [voice_display.h](voice_display.h) ส่วนไฟ backlight ใช้การต่อเดิมของโมดูล ไม่มีการกำหนด GPIO ควบคุม backlight

## พฤติกรรมและการแก้ปัญหา

ปุ่มเป็นแบบ toggle: กดครั้งแรกเปิด session กดอีกครั้งปิด การกดค้างไม่สลับซ้ำ เมื่อปิดหรือหมดเวลา จอและบอร์ดยังมีไฟ พร้อมให้กดเริ่ม session ใหม่

ตัวจับเวลา **30 วินาที** นับเฉพาะช่วงที่เปิดฟังผู้ใช้ และพักระหว่างประมวลผลหรือเล่นคำตอบ การตรวจเสียงจากไมโครโฟนพักระหว่างเล่นเสียง พร้อมเว้น 250 ms หลัง playback เพื่อหลีกเลี่ยงการฟังเสียงตอบของตัวเอง หากกดเริ่มใหม่ทันทีขณะคำขอที่ยกเลิกยังทำ cleanup จอแสดง Starting session จนพร้อมฟัง

[WebRTC VAD C core](src/voice_activity/README.md) ทำงานบน ESP32 โดยตรง ยืนยันเสียงพูดต่อเนื่อง 100 ms เก็บเสียงก่อนเริ่ม 240 ms เพื่อรักษาพยางค์ต้น และจบประโยคหลัง detector ไม่พบเสียงพูดต่อเนื่อง **400 ms** จำกัดการบันทึกครั้งละ **20 วินาที** ไม่ส่งเสียงขึ้น cloud ขณะปิด session หรือรอเสียงพูด เสียงพูดจากทีวีหรือบุคคลอื่นอาจกระตุ้น detector ได้

ASR เปิด `enable_nonstream`, `show_utterances`, `end_window_size=400` และ `force_to_speech_time=100` เพื่อรับสัญญาณจบประโยคจาก BytePlus เพิ่มเติม หากข้อความล่าสุดมี `definite=true` บอร์ดจะจบการบันทึกทันที แม้เสียงรบกวนยังทำให้ detector บนบอร์ดคิดว่ามีเสียงพูด การแปลงไมโครโฟนใช้ `raw >> 16` เพื่อรักษาระดับ PCM แทน gain 4 เท่าที่ทำให้ clipping ในบิลด์เดิม

ทดสอบ cloud ด้วยเสียงไทยสังเคราะห์พบสัญญาณจบประโยคประมาณ **425 ms หลังเสียงจบ** บน endpoint เดิม ผลนี้เป็นการทดสอบ API บน host ไม่ใช่เวลาตอบรวมบน ESP32 ส่วน `bigmodel_async` ในการทดสอบเดียวกันถอดเสียงไทยเป็นภาษาจีน จึงคง endpoint ที่รองรับภาษาไทยไว้

แต่ละ session เก็บบริบทสนทนา **4 รอบล่าสุด** เมื่อปิดหรือหมดเวลาจะล้างบริบท หากเครือข่ายขาด บัฟเฟอร์เต็ม หรือบริการปฏิเสธคำขอ ระบบปิด session และแสดงข้อความบนจอ ให้กดเริ่มใหม่เมื่อพร้อม

การเชื่อมต่อ cloud ตรวจสอบ TLS certificate ด้วย CA ใน `tls_roots.h` และรอซิงก์เวลาด้วย NTP ก่อนเริ่มใช้งาน ต้องให้เครือข่ายเข้าถึงทั้งบริการ BytePlus และบริการซิงก์เวลาได้

สำหรับการวินิจฉัยระหว่างพัฒนา สามารถต่อ Serial Monitor เพื่อดู `[READY]`, `[ASR]`, `[LLM]`, `[TTS]` และ `[LATENCY]` ได้ บรรทัด `[ASR]` แสดงจำนวนไบต์เสียง, peak, sample ที่ไม่เป็นศูนย์, sample ที่ clipping และสถานะคำตอบสุดท้าย โดยไม่พิมพ์ credentials หาก peak เป็นศูนย์ให้ตรวจไฟเลี้ยงไมโครโฟน ขา SD และการต่อ L/R ลง GND ส่วน `[LATENCY]` แยกเวลาที่ได้ transcript สุดท้าย ข้อความแรกจากโมเดล และ PCM ชุดแรกจาก TTS เพื่อระบุว่ารออยู่ขั้นตอนไหน

เฟิร์มแวร์ standalone นี้ใช้ HTTPS และ WebSocket โดยไม่ได้ฝัง BytePlus RTC SDK เสียงรับและส่ง cloud ใช้ **16 kHz, mono, signed PCM16 little-endian** บน WebSocket อัตราข้อมูลเสียงดิบอยู่ที่ 256 kbit/s ต่อทิศทางที่กำลังทำงาน ก่อนรวม overhead

### Streaming implementation

ModelArk uses `stream: true` and returns SSE events. The ESP32 decodes HTTP chunked transfer as bytes arrive, reads `choices[0].delta.content`, and sends incremental text as TTS `TaskRequest` events. It polls the TTS socket between model reads and during model pauses, while the separate playback task drains PCM into I2S. It sends `FinishSession` after the model's `[DONE]` event and the final text fragment. This follows the [ModelArk Chat API](https://docs.byteplus.com/en/docs/ModelArk/1494384) and [BytePlus bidirectional TTS flow](https://docs.byteplus.com/en/docs/byteplusvoice/streaming_tts).

[model_stream.h](model_stream.h) uses fixed buffers for HTTP framing, SSE events and text chunks. Text is sent at punctuation or bounded text boundaries, with a 250 ms flush timer for pending fragments. UTF-8 characters, Thai dependent vowels and tone marks stay attached; this is not Thai dictionary word segmentation. The complete reply is kept separately for the display and the existing four-turn history. Cancellation checks the current turn before sending more text or queuing audio.

Streaming changes when speech can begin; it does not compress PCM or guarantee a fixed response time. Compare the device's `[LATENCY]` lines after a real utterance to measure the improvement.

## ผลการตรวจสอบ

แฟลชบิลด์ N16R8 ลงบอร์ดจริงผ่าน **WCH USB-to-UART** สำเร็จ พร้อมตรวจสอบ hash ของข้อมูลที่เขียน หลัง reset พบ startup log และ **READY** ยืนยันการบูต Wi-Fi และ NTP การกดปุ่มและพูดจริงถูก ASR รู้จำเป็น “สวัสดีครับ.” จากนั้น ModelArk ตอบภาษาไทย และบอร์ดรับเสียง TTS **101,572 ไบต์** ส่งเข้า I2S สำเร็จ

การทดสอบนี้เกิดขณะต่อ USB กับคอมพิวเตอร์เพื่ออ่าน log โดย ESP32 เรียกบริการ cloud เอง **ผู้ใช้ยืนยันเพิ่มเติมแล้วว่าอุปกรณ์ทำงานแบบ standalone โดยไม่ต้องต่อคอมพิวเตอร์**

บิลด์ N16R8 พร้อม TFT และ streaming ผ่านการ compile แล้ว ใช้ flash **1,215,403 ไบต์** (92% ของ app partition เริ่มต้น) และ global RAM **80,612 ไบต์** ตัวเลข RAM นี้ยังไม่รวม audio queues, task stacks, TLS, ประวัติสนทนา และ canvas จอ **40 KiB** ที่จัดสรรระหว่างทำงาน โดย canvas ใช้ PSRAM ก่อน

บิลด์ streaming แฟลชลงบอร์ดและตรวจ hash สำเร็จ หลัง reset พบ `[DISPLAY]` และ `[READY]` เวลาเริ่มตอบหลังผู้ใช้หยุดพูดบนบอร์ดจริงยังต้องวัดจากการทดลองพูด

## เครื่องมือสำหรับนักพัฒนา — ใช้เมื่อจำเป็นเท่านั้น

เครื่องมือในหัวข้อนี้ใช้เตรียมไฟล์หรือทดสอบแยกบนคอมพิวเตอร์ **ไม่ต้องรันระหว่างใช้งานอุปกรณ์** วิธีตั้งค่าและแฟลชผ่าน Arduino IDE ด้านบนไม่ต้องใช้เครื่องมือเหล่านี้

### สร้าง config จากไฟล์ environment ที่มีอยู่

หากต้องการใช้ค่าจากไฟล์ `.env` เดิมแทนการกรอก `config.local.h` เอง สามารถใช้ helper นี้จากราก repository:

```sh
python3 BytePlus/ESP-BytePlus/configure.py
python3 BytePlus/ESP-BytePlus/configure.py --language en
```

เลือกใช้คำสั่งเดียวตามภาษา สคริปต์สร้าง `config.local.h` โดยตั้งสิทธิ์ `0600` และไม่พิมพ์ credentials ค่า resolve ตามลำดับ process environment → `.env` ที่ราก → `BytePlus/.env` → `VoiceBot/.env` → `~/github/rtcbot/.env.local` หรือไฟล์ใน `BYTEPLUS_ENV_FILE` เขียนเฉพาะค่าที่ standalone ต้องใช้ลง header

เลือกเสียงผ่าน `--voice` หรือ `BYTEPLUS_TTS_VOICE_TH` / `BYTEPLUS_TTS_VOICE_EN` และ override resource ด้วย `BYTEPLUS_TTS_RESOURCE_ID` ได้

### ใช้ Arduino CLI แทน IDE

```sh
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install ArduinoJson@7.4.3
arduino-cli lib install "Adafruit ST7735 and ST7789 Library@1.11.0" "Adafruit GFX Library@1.12.6" "Adafruit BusIO@1.17.4" "U8g2_for_Adafruit_GFX@1.8.0"
arduino-cli board list
```

สำหรับ N16R8 ที่ตรวจพบ ใช้พอร์ตจาก `board list` แทน `YOUR_SERIAL_PORT`:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi BytePlus/ESP-BytePlus
arduino-cli upload --fqbn esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi --port YOUR_SERIAL_PORT BytePlus/ESP-BytePlus
```

ค่า generic สำหรับ compile ESP32-S3 คือ `esp32:esp32:esp32s3` เลือกค่า flash/PSRAM ให้ตรงกับบอร์ดที่จะ upload

### ทดสอบซอฟต์แวร์บน host

ทดสอบ offline จากราก repository:

```sh
python -m unittest discover -s tests -v
```

ทดสอบบริการจริงด้วยไฟล์เสียงพูด **WAV mono PCM16, 16 kHz** ใช้ Python environment ที่ติดตั้ง `requirements.txt` แล้ว:

```sh
python scripts/smoke_esp_byteplus.py --live --mic-wav /path/to/thai.wav --language th --voice th_female_bv568_neutral_uranus_bigtts --output-wav /path/to/reply.wav
python scripts/smoke_esp_byteplus.py --live --mic-wav /path/to/english.wav --language en
```

คำสั่งนี้ทดสอบ ASR → ModelArk → TTS บน host ด้วยไฟล์เสียง ไม่ได้ทำหน้าที่ relay ให้อุปกรณ์ `--output-wav` บันทึกคำตอบลงไฟล์ใหม่โดยไม่เขียนทับไฟล์เดิม `--live` เรียกบริการที่ตั้งค่าไว้และคิดค่าบริการตามปกติ

การทดสอบ host ผ่านทั้งภาษาไทยและอังกฤษครบเส้นทางด้วยเสียงสังเคราะห์ ได้ transcript คำตอบจากโมเดล และ PCM16 16 kHz ที่ไม่มี gzip ชุดทดสอบ offline ครอบคลุม protocol ด้วย C++, ข้อความ TTS ขนาดใหญ่, fragmented WebSocket, audio queue backpressure, การยกเลิกเสียง, timeout, ข้อความ TFT, การจัดสรร canvas และ session แบบ hands-free พร้อมตรวจ native helpers และโค้ด capture/pipeline/display ด้วย AddressSanitizer/UndefinedBehaviorSanitizer

ตัวช่วย streaming ผ่าน host tests ที่แบ่งข้อมูล HTTP/SSE ณ ตำแหน่งไบต์ต่างๆ รวมถึงกลางอักขระไทย ตรวจ chunk extensions/trailers, SSE หลายบรรทัด, ข้อมูลขาดหรือเกินขอบเขต, UTF-8 ผิดรูปแบบ และการยกเลิก callback พร้อม AddressSanitizer/UndefinedBehaviorSanitizer การทดสอบนี้ตรวจ parser และการแบ่งข้อความ ไม่ใช่ผลจับเวลาบนบอร์ดจริง

ชุดทดสอบรวม **158 tests ผ่านทั้งหมด** รวมการดึงโค้ด `TtsStream` และ `streamReply` จริงมาทดสอบกับ network doubles ยืนยันว่าเล่น PCM ได้ก่อนโมเดลส่ง `[DONE]`, ยังรับ TTS ขณะโมเดลเว้นช่วง และไม่ส่งข้อความหรือเสียงต่อหลังยกเลิก

การตรวจ TFT บน host ครอบคลุม UTF-8 ที่ไม่สมบูรณ์, การจำกัดข้อความ, ตำแหน่งสระและวรรณยุกต์ไทย, การตัดบรรทัด และการแบ่งหน้า โดยตรวจภาพจากฟอนต์จริงประกอบด้วย ฟอนต์รองรับไทยและอังกฤษทั่วไป อักขระที่ไม่มีในฟอนต์แสดงเป็น `?`

แฟลชบิลด์ TFT ลงบอร์ด N16R8 พร้อมตรวจ hash ของข้อมูลที่เขียนสำเร็จแล้ว หลัง reset พบ `[DISPLAY] ST7735 128x160 ready` พร้อมขา CS10/DC11/RST12/MOSI13/SCLK14 และ Wi-Fi READY การแสดงผลบนจอจริงยังรอผู้ใช้ยืนยัน

บิลด์ hands-free แฟลชและตรวจ hash สำเร็จแล้ว หลัง reset พบข้อความให้ tap เริ่มสนทนา การทดสอบอัตโนมัติครอบคลุมปุ่ม toggle, pre-roll, การจบประโยค, การหยุดขณะ cloud ทำงาน, ประวัติ 4 รอบ, timeout 30 วินาที และการป้องกันสถานะเก่าบนจอ การทดลองพูดสองรอบต่อเนื่อง กดหยุด และปล่อยเงียบ 30 วินาทีบนบอร์ดจริงยังรอผู้ใช้ยืนยัน

ระหว่างทดสอบบอร์ดพบและแก้ CRLF ส่วนเกินใน WebSocket HTTP handshake โดยให้ `CloudSocket` ลบเฉพาะ CR/LF ท้าย extra headers พร้อม regression tests

## API ที่ ESP32 เรียกโดยตรง

| บริการ | Endpoint | เอกสาร |
| --- | --- | --- |
| SeedASR | `wss://voice.ap-southeast-1.bytepluses.com/api/v3/sauc/bigmodel_nostream` | [ASR unidirectional streaming](https://docs.byteplus.com/en/docs/byteplusvoice/asrunidirect) |
| ModelArk | `https://ark.ap-southeast.bytepluses.com/api/v3/chat/completions` | [Chat API / SSE](https://docs.byteplus.com/en/docs/ModelArk/1494384) |
| TTS | `wss://voice.ap-southeast-1.bytepluses.com/api/v3/tts/bidirection` | [TTS bidirectional streaming](https://docs.byteplus.com/en/docs/byteplusvoice/streaming_tts) |
