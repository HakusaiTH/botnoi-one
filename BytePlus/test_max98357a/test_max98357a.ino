// Test Sketch for MAX98357A Audio Amplifier & Speaker (ESP32-S3)
// Pin Mapping:
// - BCLK : GPIO 38
// - LRC  : GPIO 39
// - DIN  : GPIO 40

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

constexpr int SPK_BCLK = 38;
constexpr int SPK_LRC  = 39;
constexpr int SPK_DIN  = 40;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr float TONE_FREQ = 440.0; // 440 Hz (A4 Note)
constexpr size_t BUFFER_SAMPLES = 320;

I2SClass speaker;
int16_t sineBuffer[BUFFER_SAMPLES * 2]; // Stereo (L+R)

void generateSineWave() {
  const float amplitude = 8000.0; // Moderate volume
  const float phaseInc = 2.0 * M_PI * TONE_FREQ / SAMPLE_RATE;
  static float phase = 0.0;

  for (size_t i = 0; i < BUFFER_SAMPLES; ++i) {
    int16_t sample = (int16_t)(amplitude * sin(phase));
    sineBuffer[i * 2]     = sample; // Left channel
    sineBuffer[i * 2 + 1] = sample; // Right channel
    phase += phaseInc;
    if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- MAX98357A Speaker Test ---");
  Serial.printf("Configuring Pins: BCLK=%d, LRC=%d, DIN=%d\n", SPK_BCLK, SPK_LRC, SPK_DIN);

  speaker.setPins(SPK_BCLK, SPK_LRC, SPK_DIN, -1);
  if (!speaker.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[ERROR] Failed to initialize I2S Speaker! Check wiring and pins.");
    while (true) delay(1000);
  }

  Serial.println("[READY] Playing 440 Hz test tone audio cycle (1s ON, 1s OFF)...");
}

void loop() {
  Serial.println("[SPK] Playing 440 Hz Beep Tone (ON)...");
  uint32_t startTime = millis();
  
  // Play 440 Hz tone for 1 second
  while (millis() - startTime < 1000) {
    generateSineWave();
    size_t bytesWritten = 0;
    i2s_channel_write(speaker.txChan(), sineBuffer, sizeof(sineBuffer), &bytesWritten, 100);
  }

  Serial.println("[SPK] Silence (OFF)...");
  delay(1000);
}
