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

// GPIO46 preserves the existing wiring. It is an ESP32-S3 boot strapping pin;
// see README before adding an external pull resistor. GPIO4 is an alternative.
#ifndef VOICEBOT_BUTTON_PIN
#define VOICEBOT_BUTTON_PIN 46
#endif

// Default: keep recording intent, but pause mic upload while the bot replies.
// This avoids speaker echo and simultaneous upload/download congestion on S3s
// without PSRAM. Set to 1 only when full duplex/barge-in is required and tested.
#ifndef VOICEBOT_FULL_DUPLEX
#define VOICEBOT_FULL_DUPLEX 0
#endif
