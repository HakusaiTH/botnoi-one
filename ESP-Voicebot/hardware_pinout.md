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

The active firmware uses the TFT and audio wiring below. It does not initialize the camera or optional SD card.

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
```

The backlight is permanently powered, so a white screen only establishes that
it is lit. The firmware must also initialize the controller and draw the face.
MISO is not read by the current display driver.

## 4. Audio and controls used by the firmware

| Device | Signal | GPIO |
|---|---|---:|
| INMP441 | BCLK / SCK | 42 |
| INMP441 | WS / LRCLK | 2 |
| INMP441 | DOUT / SD | 1 |
| MAX98357A | BCLK | 38 |
| MAX98357A | LRC | 39 |
| MAX98357A | DIN | 40 |
| Session button to GND | Signal | 4 |
| External status LED with series resistor | Anode | 48 |

GPIO42 is the microphone clock; GPIO3 belongs to the TFT clock. The audio
driver and display conflict check both use `hardware_pins.h`. GPIO38 and
GPIO39 remain active amplifier clock outputs and cannot be reused by the TFT.

## 5. Updating an existing local configuration

`config.local.h` overrides `config.example.h`. If it was copied from an older
revision, update its display/button definitions to these values while retaining
your existing credentials:

```cpp
#define VOICEBOT_BUTTON_PIN 4
#define VOICEBOT_DISPLAY_SCK_PIN 3
#define VOICEBOT_DISPLAY_MOSI_PIN 45
#define VOICEBOT_DISPLAY_DC_PIN 47
#define VOICEBOT_DISPLAY_CS_PIN 14
#define VOICEBOT_DISPLAY_RESET_PIN 21
#define VOICEBOT_DISPLAY_BACKLIGHT_PIN -1
```

After uploading, the expected initialization log at 115200 baud is:

```text
[FACE] ILI9341 320x240 at 40MHz; SCK=3 MOSI=45 DC=47 CS=14 RESET=21 LED=-1.
```

The older GPIO3 audio reservation caused the display to be skipped before SPI
initialization. The corrected guard allows this pinout and prints the GPIO
number if a local override introduces a real conflict. A memory-reserve skip
or audio startup failure can also prevent the display from starting; inspect
the preceding serial logs if this line is absent.
