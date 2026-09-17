// Direct Hardware Audio Test for MAX98357A (No WiFi/UDP needed!)
// Plays a loud 1000 Hz Beep Tone continuously to verify hardware wiring.

#include <Arduino.h>
#include <ESP_I2S.h>
#include <math.h>

// ESP32-S3 MAX98357A I2S Pins:
// - BCLK : GPIO 38
// - LRC  : GPIO 39
// - DIN  : GPIO 40
// - SD   : Connect to 3.3V! (Crucial: if SD is not on 3.3V, MAX98357A is MUTED!)
#define BCLK_PIN (38)
#define LRC_PIN  (39)
#define DIN_PIN  (40)

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t SAMPLES_COUNT = 320; // 20ms buffer

I2SClass speaker;
int16_t stereoBuffer[SAMPLES_COUNT * 2]; // Left + Right

void generateLoudTone(float freq, float amplitude) {
  static float phase = 0.0;
  float phaseInc = 2.0 * M_PI * freq / SAMPLE_RATE;

  for (size_t i = 0; i < SAMPLES_COUNT; ++i) {
    int16_t val = (int16_t)(amplitude * sin(phase));
    stereoBuffer[2 * i]     = val; // Left channel
    stereoBuffer[2 * i + 1] = val; // Right channel
    phase += phaseInc;
    if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==================================================");
  Serial.println("   MAX98357A Direct Loud Beep Hardware Test       ");
  Serial.println("==================================================");
  Serial.printf("Pins: BCLK=%d, LRC=%d, DIN=%d\n", BCLK_PIN, LRC_PIN, DIN_PIN);

  speaker.setPins(BCLK_PIN, LRC_PIN, DIN_PIN, -1, -1);
  if (!speaker.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[ERROR] Failed to initialize I2S Speaker!");
    while (true) delay(1000);
  }

  Serial.println("[READY] Outputting 1000 Hz LOUD BEEP TONE...");
  Serial.println("--------------------------------------------------");
  Serial.println("CHECKLIST IF NO SOUND:");
  Serial.println("1. SD (Shutdown) pin MUST be connected to 3.3V!");
  Serial.println("2. VIN connected to 5V (or 3.3V), GND to GND.");
  Serial.println("3. Check speaker wires connected to MAX98357A (+/-)");
  Serial.println("--------------------------------------------------\n");
}

void loop() {
  // Generate loud 1000 Hz tone (Amplitude 25000 = Loud)
  generateLoudTone(1000.0, 25000.0);
  
  size_t written = 0;
  i2s_channel_write(speaker.txChan(), (uint8_t*)stereoBuffer, sizeof(stereoBuffer), &written, 100);
}
