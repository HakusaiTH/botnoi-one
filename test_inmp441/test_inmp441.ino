// Test Sketch for INMP441 Microphone (ESP32-S3)
// Pin Mapping:
// - SCK / BCLK : GPIO 3
// - WS / LRCLK : GPIO 2
// - DOUT / SD  : GPIO 1
// Note: Connect INMP441 L/R pin to GND to select Left Channel.

#include <Arduino.h>
#include <ESP_I2S.h>

constexpr int MIC_SCK = 3;
constexpr int MIC_WS  = 2;
constexpr int MIC_SD  = 1;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLES_PER_READ = 320; // 20ms of audio

I2SClass microphone;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- INMP441 Microphone Test ---");
  Serial.printf("Configuring Pins: SCK=%d, WS=%d, SD=%d\n", MIC_SCK, MIC_WS, MIC_SD);

  microphone.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!microphone.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("[ERROR] Failed to initialize I2S Microphone! Check wiring and pins.");
    while (true) delay(1000);
  }

  Serial.println("[READY] Speak into the microphone. A VU meter bar will show your voice level below:\n");
}

void loop() {
  int32_t rawBuffer[SAMPLES_PER_READ];
  size_t bytesRead = 0;

  esp_err_t err = i2s_channel_read(microphone.rxChan(), rawBuffer, sizeof(rawBuffer), &bytesRead, 100);
  if (err == ESP_OK && bytesRead > 0) {
    size_t samplesCount = bytesRead / sizeof(int32_t);
    int32_t maxSample = 0;
    uint64_t sumSquares = 0;

    for (size_t i = 0; i < samplesCount; ++i) {
      // INMP441 outputs signed 24-bit left aligned in 32-bit slot
      int32_t sample16 = rawBuffer[i] >> 16;
      int32_t absVal = abs(sample16);
      if (absVal > maxSample) maxSample = absVal;
      sumSquares += (uint64_t)(sample16 * sample16);
    }

    uint32_t rms = sqrt(sumSquares / (samplesCount ? samplesCount : 1));

    // Print ASCII VU meter bar
    int barLength = map(constrain(rms, 0, 5000), 0, 5000, 0, 40);
    String bar = "";
    for (int i = 0; i < barLength; ++i) bar += "|";

    Serial.printf("[MIC] Peak: %5d | RMS: %5d | %-40s\n", maxSample, rms, bar.c_str());
  } else if (err != ESP_ERR_TIMEOUT) {
    Serial.printf("[WARN] I2S Read Error: %d\n", err);
  }

  delay(50);
}
