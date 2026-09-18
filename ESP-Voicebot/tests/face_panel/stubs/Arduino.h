#pragma once
// Host stub for the subset of Arduino used by display_ili9341.h.
#include <stddef.h>
#include <stdint.h>
#include <vector>

#define OUTPUT 1
#define HIGH 1
#define LOW 0

namespace arduino_stub {
struct PinEvent {
  int pin;
  int value;
  uint32_t atMillis;
};
extern std::vector<int> modes;
extern std::vector<PinEvent> writes;
extern uint32_t delayed;
int levelOf(int pin);
void clear();
}  // namespace arduino_stub

void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
void delay(uint32_t ms);
