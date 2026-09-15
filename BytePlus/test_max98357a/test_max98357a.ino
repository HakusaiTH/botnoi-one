#include <Arduino.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#if __has_include("../ESP-BytePlus/config.local.h")
#include "../ESP-BytePlus/config.local.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "Pho"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "Fujiwara23121911"
#endif

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 0, 0)
#error "Please use Arduino ESP32 Core v3.0.0 or higher..."
#endif

#define LED_PIN  (48)   // Status LED pin (GPIO48)
#define LED_ON   (1)    // Active-high LED 
#define LED_OFF  (!LED_ON)

// MAX98357A I2S Audio Pins for ESP32-S3
// - BCLK : GPIO 38
// - LRC  : GPIO 39
// - DIN  : GPIO 40
// IMPORTANT: Connect MAX98357A SD pin to 3.3V!
#define BCLK_PIN (38)
#define LRC_PIN  (39)
#define DIN_PIN  (40)

const int queue_size  = 8;
const int SAMPLE_RATE = 16000;  // sample rate in Hz
const int NUM_SAMPLES = 512;
const int SERVER_PORT = 9000;

QueueHandle_t sample_queue;
WiFiUDP  udp;
I2SClass i2s_out;

typedef struct {
  int16_t samples[NUM_SAMPLES];
} audio_chunk_t;

void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi: ");
  Serial.print(WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    Serial.print(".");
  }
  digitalWrite(LED_PIN, LED_ON);
  Serial.println();
  Serial.print("Connected! ESP32 IP Address: ");
  Serial.println(WiFi.localIP());
}

void task_udp_receive(void *parameter) {
  for (;;) {
    int packetSize = udp.parsePacket();
    if (packetSize == (NUM_SAMPLES * sizeof(int16_t))) {
      audio_chunk_t chunk;
      int len = udp.read((char*)chunk.samples, sizeof(chunk.samples));
      if (len == sizeof(chunk.samples)) {
        if (xQueueSend(sample_queue, &chunk, 100) != pdTRUE) {
          Serial.println("[UDP] Queue full, dropping audio packet!");
        }
        else {
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write((const uint8_t *)"ACK", 3);
          udp.endPacket();
        }
      }
    }
    delay(1);
  }
}

// FreeRTOS Task for Audio I2S Output (Stereo Framing for MAX98357A)
void task_i2s_write(void *parameter) {
  audio_chunk_t chunk;
  int16_t stereoBuffer[NUM_SAMPLES * 2]; // Dual channel (Left + Right)
  uint32_t packetsPlayed = 0;

  for (;;) {
    if (xQueueReceive(sample_queue, &chunk, portMAX_DELAY) == pdTRUE) {
      float gain = 1.5f; // Amplification
      for (int i = 0; i < NUM_SAMPLES; i++) {
        int32_t temp = (int32_t)(chunk.samples[i] * gain);
        if (temp > 32767)  temp = 32767;
        if (temp < -32768) temp = -32768;
        int16_t sample16 = (int16_t)temp;
        
        // Duplicate Mono to Stereo Left & Right for MAX98357A I2S framing
        stereoBuffer[2 * i]     = sample16; // Left
        stereoBuffer[2 * i + 1] = sample16; // Right
      }

      // Write Stereo PCM to MAX98357A
      i2s_out.write((const uint8_t *)stereoBuffer, sizeof(stereoBuffer));
      
      packetsPlayed++;
      if (packetsPlayed % 50 == 0) {
        Serial.printf("[I2S] Playing audio stream... (%u packets received & output)\n", packetsPlayed);
      }
    }
    delay(1);
  }
}

void init_i2s_out() {
  // Config I2S Pins: BCLK=38, LRC=39, DIN=40
  i2s_out.setPins(BCLK_PIN, LRC_PIN, DIN_PIN, -1, -1);

  // MAX98357A requires I2S STEREO framing (I2S_SLOT_MODE_STEREO / I2S_STD_SLOT_BOTH)
  bool success = i2s_out.begin(I2S_MODE_STD, SAMPLE_RATE,
                               I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  if (!success) {
    Serial.println("[ERROR] Failed to initialize I2S! Check board & pin definitions.");
    while (true) delay(1000);
  }
  Serial.println("[I2S] Max98357A I2S Output Initialized in STEREO Mode.");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  Serial.println("\n==================================================");
  Serial.println(" ESP32-S3 MAX98357A Audio Test (BCLK=38,LRC=39,DIN=40) ");
  Serial.println("==================================================");

  // Initialize I2S
  init_i2s_out();

  // Connect WiFi
  connect_wifi();

  // Start UDP server
  udp.begin(SERVER_PORT);
  Serial.printf("Listening for UDP Audio Stream on Port %d...\n\n", SERVER_PORT);

  sample_queue = xQueueCreate(queue_size, sizeof(audio_chunk_t));
  if (sample_queue == NULL) {
    Serial.println("[ERROR] Failed to create Audio FreeRTOS Queue!");
    while (true) delay(1000);
  }

  xTaskCreatePinnedToCore(task_udp_receive, "UDP Receive", 4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(task_i2s_write, "I2S Write", 8192, NULL, 2, NULL, 1);
}

void loop() {
}
