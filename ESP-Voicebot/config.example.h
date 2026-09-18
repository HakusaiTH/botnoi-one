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

// GPIO46 active-low session button (Released -> HIGH, Pressed -> LOW).
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
