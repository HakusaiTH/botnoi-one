#!/usr/bin/env bash
set -euo pipefail
task_dir="$(cd -- "$(dirname -- "$0")" && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/voicebot-aec-wrapper.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$task_dir/stubs" "$task_dir/echo_canceller_test.cpp" -o "$test_binary"
"$test_binary"
