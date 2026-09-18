#pragma once
// ESP32-S3 output mask: GPIO0..48 except the unbonded GPIO22..25.
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) \
  ((pin) >= 0 && (pin) <= 48 && ((pin) < 22 || (pin) > 25))
