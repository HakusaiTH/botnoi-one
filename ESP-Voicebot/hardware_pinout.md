# GOOUUU ESP32-S3-CAM V1.5
## Hardware Specifications & Pin Configuration

## 1. Board Overview

The system is based on the **GOOUUU ESP32-S3-CAM V1.5** with an **OV3660 camera** and a **2.8-inch 240×320 TFT display**.

The hardware configuration includes:

- ESP32-S3 microcontroller
- OV3660 camera
- 2.8-inch 240×320 TFT display
- INMP441 I2S digital microphone
- MAX98357A I2S audio amplifier
- External LED
- Push button
- Optional SD card

The pin configuration is designed to avoid conflicts between the camera, TFT display, microphone, and audio amplifier.

---

# 2. Main Components

| Component | Specification |
|---|---|
| Microcontroller | ESP32-S3 |
| Development Board | GOOUUU ESP32-S3-CAM V1.5 |
| Camera | OV3660 |
| TFT Display | 2.8-inch |
| TFT Resolution | 240 × 320 pixels |
| TFT Controller | ILI9341 |
| Microphone | INMP441 |
| Microphone Interface | I2S |
| Audio Amplifier | MAX98357A |
| Audio Interface | I2S |
| External LED | Standard LED |
| Push Button | Digital input |
| Logic Voltage | 3.3V |
| Audio Amplifier Supply | 5V |

---

# 3. TFT Display Pinout

The 2.8-inch TFT display uses an SPI interface.

| TFT Pin | ESP32-S3 GPIO | Function |
|---|---:|---|
| VCC | 5V | Power |
| GND | GND | Ground |
| SCK | GPIO3 | SPI Clock |
| SDI / MOSI | GPIO45 | SPI Data |
| SDO / MISO | GPIO46 | SPI Data |
| DC | GPIO47 | Data / Command |
| RESET | GPIO21 | Display Reset |
| CS | GPIO14 | Chip Select |
| LED | 3.3V | Backlight |

### TFT Interface

```text
TFT SCK   -> GPIO3
TFT MOSI  -> GPIO45
TFT MISO  -> GPIO46
TFT DC    -> GPIO47
TFT RESET -> GPIO21
TFT CS    -> GPIO14
TFT LED   -> 3.3V