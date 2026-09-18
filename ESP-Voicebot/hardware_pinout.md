# GOOUUU ESP32-S3 Hardware Pinout Specifications

## 1. System Overview

This specification documents the complete hardware pin configuration for the **ESP-Voicebot** firmware running on the **GOOUUU ESP32-S3** board.

The hardware setup includes:
- **Microcontroller**: ESP32-S3
- **Microphone**: INMP441 Digital I2S Microphone
- **Audio Amplifier**: MAX98357A Mono I2S Class-D Amplifier
- **Status Indicator**: External LED (with series current-limiting resistor)
- **Session Control**: Digital Push Button (Active LOW)

---

## 2. Complete Pin Mapping Table

| Component | Signal / Pin | ESP32-S3 Connection | Function / Description |
|---|---|---:|---|
| **INMP441** | SCK / BCLK | **GPIO 48** | I2S Serial Clock |
| **INMP441** | WS / LRCLK | **GPIO 2** | I2S Word Select / Frame Clock |
| **INMP441** | SD / DOUT | **GPIO 1** | I2S Data Output |
| **INMP441** | L/R | **GND** | Left channel selection |
| **INMP441** | VCC | **3.3V** | 3.3V Power Supply |
| **INMP441** | GND | **GND** | System Ground |
| **MAX98357A** | BCLK | **GPIO 38** | I2S Bit Clock (matrix output from I2S0) |
| **MAX98357A** | LRC | **GPIO 39** | I2S Left/Right Clock (matrix output) |
| **MAX98357A** | DIN | **GPIO 40** | I2S Data Input |
| **MAX98357A** | VIN | **5V** | 5V Power Supply |
| **MAX98357A** | GND | **GND** | System Ground |
| **MAX98357A** | GAIN | **GND** | Gain set to +12dB (GND default) |
| **Status LED** | Anode (+) | **GPIO 13** | Status LED output (via series resistor) |
| **Status LED** | Cathode (-) | **GND** | Ground |
| **Push Button** | Signal | **GPIO 46** | Session Start/Stop trigger (INPUT_PULLUP) |
| **Push Button** | Ground | **GND** | Ground |

---

## 3. Connection Details & Signal Behavior

### 3.1 INMP441 I2S Digital Microphone
- **Power**: 3.3V supply and common GND.
- **Channel Slot**: L/R pin tied to GND selects the left channel slot.
- **I2S Signals**:
  - `BCLK`: GPIO 48
  - `WS`: GPIO 2
  - `SD`: GPIO 1

### 3.2 MAX98357A Audio Amplifier
- **Power**: 5V supply for optimal audio output power, with common GND.
- **Gain**: GAIN pin tied to GND (+12dB default gain).
- **I2S Signals**:
  - `BCLK`: GPIO 38
  - `LRC`: GPIO 39
  - `DIN`: GPIO 40

### 3.3 Status LED
- **GPIO 13**: High output (`HIGH`) when session is active / listening, low output (`LOW`) when idle.
- **Wiring**: Anode (+) to GPIO 13 via current-limiting resistor; Cathode (-) to GND.

### 3.4 Push Button (Session Control)
- **GPIO 46**: Configured with internal pull-up (`INPUT_PULLUP`).
- **Signal Logic**:
  - **Released**: `HIGH` (3.3V via internal pull-up)
  - **Pressed**: `LOW` (GND)
