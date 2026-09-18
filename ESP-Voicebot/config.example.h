#pragma once

// Wi-Fi Credentials
#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASS
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#endif

// Botnoi Voicebot API Credentials
#ifndef BOTNOI_API_KEY
#define BOTNOI_API_KEY "YOUR_BOTNOI_API_KEY"
#endif

#ifndef BOTNOI_AGENT_ID
#define BOTNOI_AGENT_ID "YOUR_BOTNOI_AGENT_ID"
#endif

#ifndef BOTNOI_WS_HOST
#define BOTNOI_WS_HOST "voicebot-stg.botnoigroup.com"
#endif

#ifndef BOTNOI_WS_PATH
#define BOTNOI_WS_PATH "/v1/preview_call"
#endif

#ifndef BOTNOI_WS_PORT
#define BOTNOI_WS_PORT 443
#endif

// External session button to GND, using INPUT_PULLUP. Leave TFT SDO/MISO
// disconnected: GPIO46 is reserved for this button. KEY/BOOT remains GPIO0.
#ifndef VOICEBOT_BUTTON_PIN
#define VOICEBOT_BUTTON_PIN 46
#endif

// Native ESP-SR full-duplex echo cancellation. Requires ESP32-S3 with working
// PSRAM (OPI for N16R8). Missing memory is detected before native allocation;
// the device reports a half-duplex fallback instead of uploading speaker echo.
#ifndef VOICEBOT_AEC_ENABLED
#define VOICEBOT_AEC_ENABLED 1
#endif

// Optional raw full-duplex override, used ONLY when AEC is explicitly disabled.
// Requires external echo control/acoustic isolation. With AEC enabled, full
// duplex starts automatically only after successful echo-canceller initialization.
#ifndef VOICEBOT_FULL_DUPLEX
#define VOICEBOT_FULL_DUPLEX 0
#endif

// 2.8" 320x240 ILI9341 SPI panel showing the animated robot face. Set
// VOICEBOT_DISPLAY_ENABLED 0 to build audio-only firmware. The touch
// controller on the module is not used and needs no wiring.
#ifndef VOICEBOT_DISPLAY_ENABLED
#define VOICEBOT_DISPLAY_ENABLED 1
#endif

#ifndef VOICEBOT_DISPLAY_SCK_PIN
#define VOICEBOT_DISPLAY_SCK_PIN 3
#endif

#ifndef VOICEBOT_DISPLAY_MOSI_PIN
#define VOICEBOT_DISPLAY_MOSI_PIN 45
#endif

#ifndef VOICEBOT_DISPLAY_DC_PIN
#define VOICEBOT_DISPLAY_DC_PIN 47
#endif

#ifndef VOICEBOT_DISPLAY_CS_PIN
#define VOICEBOT_DISPLAY_CS_PIN 14
#endif

// Set to -1 when the panel's RESET is tied to the board's own reset line.
#ifndef VOICEBOT_DISPLAY_RESET_PIN
#define VOICEBOT_DISPLAY_RESET_PIN 21
#endif

// Set to -1 when LED is wired permanently on. A GPIO cannot safely source the
// backlight current of every module; see README before driving it directly.
#ifndef VOICEBOT_DISPLAY_BACKLIGHT_PIN
#define VOICEBOT_DISPLAY_BACKLIGHT_PIN -1
#endif

// The face needs 320x240 landscape: 1 is the default, 3 flips it by 180 degrees.
// Update any old config.local.h override too; portrait (0/2) cannot fit the face.
#ifndef VOICEBOT_DISPLAY_ROTATION
#define VOICEBOT_DISPLAY_ROTATION 1
#endif

// SPI3 keeps the panel off the bus the global Arduino SPI object claims.
#ifndef VOICEBOT_DISPLAY_SPI_BUS
#define VOICEBOT_DISPLAY_SPI_BUS HSPI
#endif

// Conservative write clock: ILI9341 specifies a minimum 100 ns write cycle.
// Initialization is capped at 1 MHz. Faster writes need board validation;
// try 1 MHz first if jumper-wire connections show noise or missing pixels.
#ifndef VOICEBOT_DISPLAY_SPI_HZ
#define VOICEBOT_DISPLAY_SPI_HZ 10000000
#endif

// Frame interval of the face task. 33ms is about 30 frames per second, which
// is enough for the mouth to track syllables without starving the audio tasks.
#ifndef VOICEBOT_DISPLAY_FRAME_MS
#define VOICEBOT_DISPLAY_FRAME_MS 33
#endif
