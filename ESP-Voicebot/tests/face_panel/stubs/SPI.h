#pragma once
// Host stub for the subset of SPI used by display_ili9341.h. Records every
// byte so a test can check the ILI9341 command stream and the pixel order.
#include <stddef.h>
#include <stdint.h>
#include <vector>

#define MSBFIRST 1
#define SPI_MODE0 0
#define FSPI 0
#define HSPI 1

class SPISettings {
 public:
  SPISettings() : frequency(0), order(0), mode(0) {}
  SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
      : frequency(clock), order(bitOrder), mode(dataMode) {}
  uint32_t frequency;
  uint8_t order;
  uint8_t mode;
};

class SPIClass {
 public:
  explicit SPIClass(uint8_t bus) : bus(bus) {}
  bool begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);
  void end();
  void beginTransaction(SPISettings settings);
  void endTransaction();
  void write(uint8_t value);
  void writeBytes(const uint8_t* data, size_t length);

  uint8_t bus;
  bool started = false;
  bool beginSucceeds = true;
  uint32_t beginCalls = 0;
  uint32_t endCalls = 0;
  int8_t sck = -1, miso = -1, mosi = -1, ss = -1;
  uint32_t transactions = 0;
  int32_t depth = 0;
  std::vector<uint8_t> bytes;        // Everything sent, in order.
  std::vector<size_t> byteMarks;     // Index in `bytes` of each byte's DC level.
  std::vector<uint32_t> transactionTimes;
};
