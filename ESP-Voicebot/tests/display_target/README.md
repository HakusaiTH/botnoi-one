# Standalone ILI9341 display check

This test uses the production `display_ili9341.h` driver and the pins in
`config.example.h`. It cycles black, red, green, blue and white screens, then
three RGB bars and a black/white checkerboard. Each stage is labeled on Serial
at 115200 baud. There is no Wi-Fi, audio, voicebot session or framebuffer;
pixels use fixed row buffers. Arduino and SPI still allocate their normal
runtime control state.

The current defaults use rotation `3` and software rotation: pixels are mapped
into native 240×320 address windows while the pattern remains logically
320×240. Rotation `1` supports the opposite mounting direction. To compare
hardware rotation, set `VOICEBOT_DISPLAY_SOFTWARE_ROTATION` to `0` in the
generated configuration and rebuild.

From the repository root, using an existing Arduino CLI and ESP32 core 3.3.11:

```sh
python3 ESP-Voicebot/scripts/display_check.py
```

For a toolchain outside your normal CLI configuration:

```sh
python3 ESP-Voicebot/scripts/display_check.py \
  --arduino-cli /path/to/arduino-cli \
  --config-file /path/to/arduino-cli.yaml
```

The default SPI write frequency is **1 MHz**. To compare signal stability at
another speed, add `--frequency 5000000`; the accepted range is 1–10 MHz.
The script downloads nothing and never uploads. It compiles for ESP32-S3,
16 MB flash and PSRAM disabled; the display test does not need PSRAM.

The printed artifact directory, `ESP-Voicebot/build/display-check-*/`, contains:

- `DisplayCheck/DisplayCheck.ino`: the standalone sketch to open in Arduino IDE.
- `DisplayCheck/config.example.h`: an editable copy of the default wiring.
- `DisplayCheck/display_check_settings.h`: the selected diagnostic SPI speed.
- `DisplayCheck/display_ili9341.h`: the production driver used by this build.
- `compile.log`, `output/` and `summary.json`: compiler output, firmware binaries,
  memory use, warnings and SHA-256 hashes of staged source and binaries.

The script never reads `config.local.h`. Credential macros in the staged example
configuration become unused placeholders. No credential setup is required.
Edit pins only in the generated configuration if your wiring differs, then
compile/upload that generated sketch yourself. Rerunning the script creates a
fresh directory using the current repository defaults. If you edit a generated
file, its original manifest no longer describes your edited build.

In Arduino IDE, select ESP32-S3 Dev Module with flash size 16 MB, the
3 MB APP/9 MB FATFS partition scheme and PSRAM Disabled. The script build uses
USB CDC On Boot Disabled, so Serial is the board's UART0 USB-to-serial port;
choose that port and 115200 baud. If your board uses native USB instead, select
the corresponding USB CDC setting and port when compiling in Arduino IDE.
Uploading the diagnostic replaces the running voicebot firmware; upload the
main `ESP-Voicebot.ino` again after the display check.

## Reading the result

| Observation | What it establishes / next check |
|---|---|
| All solid colors, RGB bars and checker tiles appear correctly | The driver, selected pins and display link work at this SPI speed. This does not validate audio or the full voicebot. |
| Serial reports pin/SPI initialization failure | Check duplicate/invalid pins and SPI bus setup. No pixels were sent. |
| Serial cycles through stages but the panel stays white | SPI initialization alone does not detect the panel. Check panel power/common ground, the display controller model, and the SCK/MOSI/DC/CS/RESET connections against the printed pins. |
| Panel is dark during every stage | Check panel power and its backlight connection. `BACKLIGHT=-1` means this test does not drive a backlight GPIO. |
| Colors or shapes change but are wrong | Check controller identity, rotation/color order and wiring; compare again at 1 MHz. |
| 1 MHz is reliable but a higher speed corrupts pixels | Investigate wiring length, ground and signal quality before raising the main firmware speed. |
| No stage labels appear | Check the selected Serial port/USB mode, baud rate, upload and board reset. |

The test has no panel readback. `SPI initialized` means the MCU accepted the bus
setup; it is not a panel detection result. A compile success is not a physical
display test. Record both the serial output and what you actually see.
