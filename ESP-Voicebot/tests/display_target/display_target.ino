// Standalone panel test. scripts/display_check.py stages this fixture with the
// actual production driver and sanitized default configuration for Arduino IDE.
#include <Arduino.h>
#include <SPI.h>
#include "config.example.h"
#include "display_check_settings.h"
#include "display_ili9341.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This diagnostic uses the ESP32-S3 pin map."
#endif

voicebot_face::Ili9341 panel;
SPIClass displaySpi(VOICEBOT_DISPLAY_SPI_BUS);
alignas(4) uint16_t checkerRow[voicebot_face::Ili9341::kMaxSpanPixels];
bool displayReady = false;
uint32_t cycleNumber = 0;

void showSolid(const char* name, uint16_t colour) {
  Serial.printf("[DISPLAY CHECK] %s\n", name);
  panel.fill(0, 0, panel.width(), panel.height(), colour);
  panel.backlight(true);
  delay(2000);
}

void showBars() {
  Serial.println("[DISPLAY CHECK] RGB BARS: left=RED middle=GREEN right=BLUE");
  const uint16_t colours[] = {0xF800, 0x07E0, 0x001F};
  for (int stripe = 0; stripe < 3; ++stripe) {
    const int16_t left = panel.width() * stripe / 3;
    const int16_t right = panel.width() * (stripe + 1) / 3;
    panel.fill(left, 0, right - left, panel.height(), colours[stripe]);
    delay(1);
  }
  delay(3000);
}

void showChecker() {
  Serial.println("[DISPLAY CHECK] CHECKER: 16x16 black/white tiles");
  // One row of pixels is reused for the entire screen. Close the transaction
  // before yielding so no SPI lock remains held during a delay.
  for (int16_t y = 0; y < panel.height(); ++y) {
    for (int16_t x = 0; x < panel.width(); ++x) {
      checkerRow[x] = ((x / 16 + y / 16) & 1) ? 0xFFFF : 0x0000;
    }
    panel.beginWindow(0, y, panel.width(), 1);
    panel.writeRow(checkerRow, static_cast<size_t>(panel.width()));
    panel.endWindow();
    if ((y & 15) == 15) delay(1);
  }
  delay(3000);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[DISPLAY CHECK] ESP32-S3 ILI9341 test; no Wi-Fi, audio or credentials");
  Serial.printf("[DISPLAY CHECK] SPI=%lu Hz rotation=%d SCK=%d MOSI=%d DC=%d CS=%d RESET=%d BACKLIGHT=%d\n",
      static_cast<unsigned long>(DISPLAY_CHECK_SPI_HZ), VOICEBOT_DISPLAY_ROTATION,
      VOICEBOT_DISPLAY_SCK_PIN, VOICEBOT_DISPLAY_MOSI_PIN, VOICEBOT_DISPLAY_DC_PIN,
      VOICEBOT_DISPLAY_CS_PIN, VOICEBOT_DISPLAY_RESET_PIN, VOICEBOT_DISPLAY_BACKLIGHT_PIN);
  const voicebot_face::Ili9341::Pins pins = {
      VOICEBOT_DISPLAY_SCK_PIN, VOICEBOT_DISPLAY_MOSI_PIN, VOICEBOT_DISPLAY_DC_PIN,
      VOICEBOT_DISPLAY_CS_PIN, VOICEBOT_DISPLAY_RESET_PIN, VOICEBOT_DISPLAY_BACKLIGHT_PIN};
  displayReady = panel.begin(pins, VOICEBOT_DISPLAY_ROTATION, DISPLAY_CHECK_SPI_HZ, displaySpi);
  if (!displayReady) {
    Serial.println("[DISPLAY CHECK] FAILED: pin validation or SPI initialization. No pixels will be sent.");
    return;
  }
  Serial.println("[DISPLAY CHECK] SPI initialized; write-only wiring cannot detect or identify the panel.");
  Serial.printf("[DISPLAY CHECK] Expected size: %d x %d. Watch the panel while stages are printed.\n",
      int(panel.width()), int(panel.height()));
}

void loop() {
  if (!displayReady) {
    delay(1000);
    return;
  }
  Serial.printf("[DISPLAY CHECK] CYCLE %lu\n", static_cast<unsigned long>(++cycleNumber));
  showSolid("BLACK", 0x0000);
  showSolid("RED", 0xF800);
  showSolid("GREEN", 0x07E0);
  showSolid("BLUE", 0x001F);
  showSolid("WHITE", 0xFFFF);
  showBars();
  showChecker();
}
