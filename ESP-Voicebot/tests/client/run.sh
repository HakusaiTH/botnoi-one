#!/usr/bin/env bash
set -euo pipefail
client_test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
arduinojson_dir="${ARDUINOJSON_DIR:-$client_test_dir/../../build/toolchain/user/libraries/ArduinoJson/src}"
if [[ ! -f "$arduinojson_dir/ArduinoJson.h" ]]; then
  echo 'Set ARDUINOJSON_DIR to the ArduinoJson 7.4.3 src directory.' >&2
  exit 1
fi
client_test_binary="$(mktemp "${TMPDIR:-/tmp}/voicebot-client-test.XXXXXX")"
trap 'rm -f "$client_test_binary"' EXIT
# Match ArduinoJson's 32-bit ESP32 pool slot count on a 64-bit host.
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DARDUINOJSON_POOL_CAPACITY=128 \
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0 \
  -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0 \
  -I "$client_test_dir/stubs" -I "$arduinojson_dir" \
  "$client_test_dir/client_protocol_test.cpp" -o "$client_test_binary"
"$client_test_binary"
