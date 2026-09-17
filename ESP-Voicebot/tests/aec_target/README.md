# Native AEC fixture

This test compiles the production `echo_canceller.h` against ESP-SR 2.4.6
bundled in Arduino ESP32 core 3.3.11. It uses no I2S, Wi-Fi, credentials, or
mock DSP functions. The helper's aligned input/output copies are exercised.

Install Espressif's QEMU `esp_develop_9.2.2_20260417` for your host using the
[IDF 5.5.5 tool manifest](https://github.com/espressif/esp-idf/blob/v5.5.5/tools/tools.json).
Use its documented system dependencies; upstream QEMU lacks this S3 machine.
On macOS these include libgcrypt, pixman, and SDL2. Pass any local dependency
search path through `DYLD_LIBRARY_PATH` when needed.

```sh
python3 ESP-Voicebot/scripts/test_aec_target.py \
  --arduino-cli /path/to/arduino-cli \
  --config-file /path/to/arduino-cli.yaml \
  --qemu /path/to/qemu-system-xtensa

# Execute the real target's missing-PSRAM admission path; no DSP is invoked.
python3 ESP-Voicebot/scripts/test_aec_target.py \
  --arduino-cli /path/to/arduino-cli \
  --config-file /path/to/arduino-cli.yaml \
  --qemu /path/to/qemu-system-xtensa --psram none
```

Each run retains the exact sketch/header, SHA256 manifest, compiler log, UART
log, emulator log, and JSON results in ignored `ESP-Voicebot/build/aec-target-*`.
It stops after 30 seconds without progress, configurable with
`--progress-timeout`; the overall default limit is 600 seconds. `--build-only`
creates the same fixture sketch and binaries for a manual board run and
explicitly does not claim tests executed. UART console baud is 115200.

## Signals and acceptance criteria

Each scenario contains eight seconds of deterministic, independent near/far
broadband signals with filtering and amplitude envelopes. The microphone is
the clean near signal plus a known three-tap echo of the reference. This is
a reproducible synthetic acoustic path, not a recorded room or speech-quality
corpus. Metrics use the final two seconds, after adaptation. Near-speech
metrics compensate measured algorithm latency by correlation within 128ms.

| Scenario | Criterion |
| --- | --- |
| Silence | Peak output at most 2 PCM counts |
| Far only | At least 8dB echo return loss enhancement |
| Near only | Correlation at least 0.7, gain 0.5–1.5, SNR at least 6dB |
| Doubletalk after 3s far-end adaptation | Near correlation at least 0.55, gain 0.35–1.5, error versus known clean near reduced by at least 3dB |
| Echo path changes at 4s, delay 6→40ms | At least 6dB attenuation after readaptation |
| Saturated microphone/reference | Processing completes with finite metrics and nonzero PCM16 output |

All quality cases also require constant free heap during processing, exact
heap recovery after destruction, and allocation-free repeated `begin()`.
Emulated frame timing is logged only for diagnosis, not as a hardware budget.
Passing would not establish real-room doubletalk quality, I2S reference timing,
clock stability, speaker clipping behavior, or successful cloud barge-in.

## Observed emulator limitation (2026-09-18)

The pinned QEMU successfully initializes the real production wrapper with OPI
PSRAM (observed allocation delta: 5,668 internal bytes and 121,512 PSRAM bytes).
The no-PSRAM case passes without an AEC allocation. The default FD_LOW_COST
quality run stalls inside its first zero-input FFT frame, so **DSP quality is
not validated**. CPU1 snapshots retain the same PC and loop counter in
`test_radix2_fft_bf_s16_hp`, at `ee.fft.r2bf.s16.st.incp`; single-threaded TCG
has the same behavior. The instruction does have an implementation in the
[pinned QEMU source](https://github.com/espressif/qemu/blob/esp-develop-9.2.2-20260417/target/xtensa/translate_tie_esp32s3.c).
This is an observed stall, not proof that the opcode is unsupported or that
hardware will behave the same way. Run the unchanged fixture on an ESP32-S3
board to obtain quality and processing-time measurements.
