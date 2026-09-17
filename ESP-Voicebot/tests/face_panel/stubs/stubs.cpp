#include "Arduino.h"
#include "SPI.h"

#include <cassert>
#include <map>

namespace arduino_stub {
std::vector<int> modes;
std::vector<PinEvent> writes;
uint32_t delayed = 0;
static std::map<int, int> levels;

int levelOf(int pin) {
  const std::map<int, int>::const_iterator found = levels.find(pin);
  return found == levels.end() ? -1 : found->second;
}

void clear() {
  modes.clear();
  writes.clear();
  levels.clear();
  delayed = 0;
}
}  // namespace arduino_stub

void pinMode(int pin, int mode) {
  (void)mode;
  arduino_stub::modes.push_back(pin);
}

void digitalWrite(int pin, int value) {
  arduino_stub::PinEvent event = {pin, value};
  arduino_stub::writes.push_back(event);
  arduino_stub::levels[pin] = value;
}

void delay(uint32_t ms) { arduino_stub::delayed += ms; }

void SPIClass::begin(int8_t sckPin, int8_t misoPin, int8_t mosiPin, int8_t ssPin) {
  started = true;
  sck = sckPin;
  miso = misoPin;
  mosi = mosiPin;
  ss = ssPin;
}

void SPIClass::end() { started = false; }

void SPIClass::beginTransaction(SPISettings settings) {
  (void)settings;
  ++transactions;
  ++depth;
  assert(depth == 1);  // Nested transactions would deadlock on hardware.
}

void SPIClass::endTransaction() {
  --depth;
  assert(depth == 0);
}

void SPIClass::write(uint8_t value) {
  assert(depth == 1);  // Never send outside a transaction.
  bytes.push_back(value);
}

void SPIClass::writeBytes(const uint8_t* data, size_t length) {
  assert(depth == 1);
  for (size_t i = 0; i < length; ++i) bytes.push_back(data[i]);
}
