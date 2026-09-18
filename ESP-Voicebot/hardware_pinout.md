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

The 2.8-inch TFT display uses an SPI interface. This signal map matches the
[GOOUUU expansion-board reference](https://github.com/profharris/GOOUUU-Tech-ESP32-S3-CAM-Expansion-Board#lcd-28in-240320-spi-tft-display-ili9341).
That community guide does not establish the exact revision of an individual board or panel.

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

The driver initializes the controller at **1 MHz**, including a software reset,
then uses **10 MHz** for pixel writes by default. The ILI9341 specification requires
a minimum 100 ns serial write-clock period, corresponding to 10 MHz; the old
40 MHz setting exceeded that published timing. See the
[ILI9341 datasheet, section 18.3.4, page 238](https://www.displayfuture.com/Display/datasheet/controller/ILI9341.pdf#page=238).

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

## 5. Expansion-board peripherals sharing these pins

On the [referenced expansion board](https://github.com/profharris/GOOUUU-Tech-ESP32-S3-CAM-Expansion-Board#misc-pin-connectionsconfigurations):

- Open/remove **P7** (potentiometer on GPIO1) and **P8** (DHT11 on GPIO2) before using the microphone.
- Do not use an OLED on GPIO42 or the onboard SD socket on GPIO38–40 simultaneously with this audio wiring.
- The session button on **GPIO4 is external**. The built-in **KEY/BOOT button uses GPIO0**. GPIO4 also connects to camera SCCB/SIOD, so camera operation and this external button cannot be enabled together.

The touch controller's T_CS, T_DIN and T_CLK inputs connect to GPIO1, GPIO2 and
GPIO42. These are input connections, so the overlap alone does not prove output
contention. Touch is unsupported with this audio pin map. During troubleshooting,
isolate unused touch signals where the wiring permits; do not drive GPIO1 high
to disable touch, because it carries microphone data.

## 6. Updating an existing local configuration

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
#define VOICEBOT_DISPLAY_SPI_HZ 10000000
```

In particular, replace any old `VOICEBOT_DISPLAY_SPI_HZ 40000000` override;
updating `config.example.h` does not override an existing local definition.

After uploading, the expected initialization log at 115200 baud is:

```text
[FACE] ILI9341 320x240 at 10MHz; SCK=3 MOSI=45 DC=47 CS=14 RESET=21 LED=-1.
```

The older GPIO3 audio reservation caused the display to be skipped before SPI
initialization. The corrected guard allows this pinout and prints the GPIO
number if a local override introduces a real conflict. A memory-reserve skip
or audio startup failure can also prevent the display from starting; inspect
the preceding serial logs if this line is absent.

## 7. Isolate a blank or noisy display

A photo of snowy/random pixels does not identify the cause. It cannot distinguish
SPI timing, wiring, reset, power or a different controller. The write-only startup
log confirms that commands were sent, not that the panel accepted them.

Build the standalone pattern test at 1 MHz from the repository root:

```sh
python3 ESP-Voicebot/scripts/display_check.py --frequency 1000000
```

This builds a sketch without audio, Wi-Fi or credentials and prints its path;
it does not upload automatically. Follow the [display-test instructions](tests/display_target/README.md)
to upload it and check the solid colours and patterns. If those are still corrupt,
check the labelled LCD connections, common ground, 5 V VCC, 3.3 V backlight and
reset before testing alongside audio again. A clean standalone test narrows the
problem but does not establish long-running integrated stability.
