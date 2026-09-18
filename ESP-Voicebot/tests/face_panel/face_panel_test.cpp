// Exercises the real ILI9341 glue against recording Arduino/SPI stubs. Host
// tests cannot prove the panel lights up, but they do pin down the command
// stream, the pixel byte order and which regions actually reach the bus.
#include "../../face_display.h"
#include "../../config.example.h"
#include "../../hardware_pins.h"

#include <cassert>
#include <cstdio>
#include <vector>

using voicebot_face::FaceDisplay;
using voicebot_face::FaceFrame;
using voicebot_face::FaceRenderer;
using voicebot_face::Ili9341;
using voicebot_face::Layout;
using voicebot_face::Palette;
using voicebot_face::Rect;

namespace {

const int8_t kSck = VOICEBOT_DISPLAY_SCK_PIN, kMosi = VOICEBOT_DISPLAY_MOSI_PIN,
             kDc = VOICEBOT_DISPLAY_DC_PIN, kCs = VOICEBOT_DISPLAY_CS_PIN,
             kReset = VOICEBOT_DISPLAY_RESET_PIN;
// Driver backlight tests need a controllable GPIO. The configured board's
// permanently powered backlight is exercised separately below.
const int8_t kBacklight = 41;

Ili9341::Pins configuredWiring() {
  const Ili9341::Pins pins = {VOICEBOT_DISPLAY_SCK_PIN, VOICEBOT_DISPLAY_MOSI_PIN,
      VOICEBOT_DISPLAY_DC_PIN, VOICEBOT_DISPLAY_CS_PIN,
      VOICEBOT_DISPLAY_RESET_PIN, VOICEBOT_DISPLAY_BACKLIGHT_PIN};
  return pins;
}

Ili9341::Pins wiring() {
  const Ili9341::Pins pins = {kSck, kMosi, kDc, kCs, kReset, kBacklight};
  return pins;
}

int projectPinConflict(const Ili9341::Pins& pins) {
  const int assigned[] = {pins.sck, pins.mosi, pins.dc, pins.cs, pins.reset, pins.backlight};
  return voicebot_hardware::displayPinConflict(assigned,
      sizeof(assigned) / sizeof(assigned[0]), VOICEBOT_BUTTON_PIN);
}

// Uses the same production admission helper as startFace(), then exercises
// the real panel path. No copied list of supposedly occupied GPIOs lives here.
bool beginProjectDisplay(FaceDisplay& display, const Ili9341::Pins& pins, SPIClass& bus) {
  if (projectPinConflict(pins) >= 0) return false;
  return display.begin(pins, VOICEBOT_DISPLAY_ROTATION, VOICEBOT_DISPLAY_SPI_HZ, bus,
      Layout(), Palette(), VOICEBOT_DISPLAY_SOFTWARE_ROTATION != 0);
}

// Finds a command byte followed by its parameters in the recorded stream.
bool sent(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& sequence) {
  if (sequence.size() > bytes.size()) return false;
  for (size_t start = 0; start + sequence.size() <= bytes.size(); ++start) {
    bool match = true;
    for (size_t i = 0; i < sequence.size(); ++i) {
      if (bytes[start + i] != sequence[i]) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

int levelAtByte(const SPIClass& bus, size_t index, int pin) {
  assert(index < bus.bytes.size() && bus.byteMarks.size() == bus.bytes.size());
  for (size_t cursor = bus.byteMarks[index]; cursor > 0; --cursor) {
    const auto& event = arduino_stub::writes[cursor - 1];
    if (event.pin == pin) return event.value;
  }
  return -1;
}

size_t commandIndex(const SPIClass& bus, uint8_t command) {
  for (size_t index = 0; index < bus.bytes.size(); ++index) {
    if (bus.bytes[index] == command && levelAtByte(bus, index, kDc) == LOW) return index;
  }
  assert(false && "Expected command was not sent with DC low");
  return bus.bytes.size();
}

void expectCommand(const SPIClass& bus, uint8_t command, const std::vector<uint8_t>& payload) {
  const size_t start = commandIndex(bus, command);
  assert(levelAtByte(bus, start, kCs) == LOW);
  size_t end = start + 1;
  while (end < bus.bytes.size() && levelAtByte(bus, end, kDc) == HIGH) ++end;
  assert(end - start - 1 == payload.size());
  for (size_t index = 0; index < payload.size(); ++index) {
    assert(bus.bytes[start + 1 + index] == payload[index]);
    assert(levelAtByte(bus, start + 1 + index, kCs) == LOW);
  }
}

uint32_t commandTime(const SPIClass& bus, uint8_t command) {
  const size_t start = commandIndex(bus, command);
  for (size_t index = 0; index < bus.transactionByteStarts.size(); ++index) {
    if (bus.transactionByteStarts[index] == start) return bus.transactionTimes[index];
  }
  assert(false && "Expected initialization command to start its own transaction");
  return 0;
}

// A native portrait controller model. It interprets physical CASET/PASET and
// sequential RGB565 writes only; it knows nothing about the driver's rotation
// formulas. MADCTL can have no spatial effect here, as on the reported panel.
class NativeGram {
 public:
  NativeGram() : pixels(240u * 320u, 0x5AA5), writes(240u * 320u, 0) {}

  void consume(const SPIClass& bus) {
    size_t start = 0;
    while (start < bus.bytes.size()) {
      assert(levelAtByte(bus, start, kDc) == LOW);
      assert(levelAtByte(bus, start, kCs) == LOW);
      size_t end = start + 1;
      while (end < bus.bytes.size() && levelAtByte(bus, end, kDc) == HIGH) {
        assert(levelAtByte(bus, end, kCs) == LOW);
        ++end;
      }
      const size_t length = end - start - 1;
      const uint8_t* payload = bus.bytes.data() + start + 1;
      switch (bus.bytes[start]) {
        case 0x36:
          // No mirrored axes or MV swap: receiving or losing this write must
          // leave the same coordinate system (colour interpretation may differ).
          assert(length == 1 && payload[0] == 0x08);
          ++orientationWrites;
          break;
        case 0x2A:
        case 0x2B: {
          assert(length == 4);
          const uint16_t first = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
          const uint16_t last = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
          assert(first <= last);
          if (bus.bytes[start] == 0x2A) {
            assert(last < 240);
            x_ = first;
            lastX_ = last;
          } else {
            assert(last < 320);
            y_ = first;
            lastY_ = last;
          }
          break;
        }
        case 0x2C: {
          const size_t width = lastX_ - x_ + 1;
          const size_t height = lastY_ - y_ + 1;
          assert(length == width * height * 2);
          for (size_t pixel = 0; pixel < width * height; ++pixel) {
            const size_t nativeX = x_ + pixel % width;
            const size_t nativeY = y_ + pixel / width;
            const size_t address = nativeY * 240 + nativeX;
            pixels[address] = static_cast<uint16_t>((payload[pixel * 2] << 8) | payload[pixel * 2 + 1]);
            ++writes[address];
          }
          ++windows;
          break;
        }
        default: break;  // Initialization and display-power commands.
      }
      start = end;
    }
  }

  // View the physical panel in the selected landscape mounting. Inverse
  // coordinates are independent of how the driver splits/reverses rows.
  void expectLandscape(const std::vector<uint16_t>& logical, uint8_t rotation) const {
    assert(logical.size() == 320u * 240u && (rotation == 1 || rotation == 3));
    for (size_t nativeY = 0; nativeY < 320; ++nativeY) {
      for (size_t nativeX = 0; nativeX < 240; ++nativeX) {
        const size_t logicalX = rotation == 1 ? nativeY : 319 - nativeY;
        const size_t logicalY = rotation == 1 ? 239 - nativeX : nativeX;
        assert(pixels[nativeY * 240 + nativeX] == logical[logicalY * 320 + logicalX]);
      }
    }
  }

  void clearWriteCounts() {
    for (size_t i = 0; i < writes.size(); ++i) writes[i] = 0;
  }

  std::vector<uint16_t> pixels;
  std::vector<uint16_t> writes;
  size_t windows = 0;
  size_t orientationWrites = 0;

 private:
  uint16_t x_ = 0, lastX_ = 239, y_ = 0, lastY_ = 319;
};

}  // namespace

static void configured_goouuu_pins_pass_the_project_guard_and_initialize_the_panel() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  const Ili9341::Pins pins = configuredWiring();
  assert(pins.sck == 3 && pins.backlight == -1);
  assert(VOICEBOT_DISPLAY_SPI_HZ == 10000000);
  assert(VOICEBOT_DISPLAY_ROTATION == 3);
  assert(VOICEBOT_DISPLAY_SOFTWARE_ROTATION == 1);
  assert(voicebot_hardware::kMicrophoneBclk == 48);
  assert(VOICEBOT_BUTTON_PIN == 46);
  assert(voicebot_hardware::kStatusLed == 13);
  // GPIO3 used to be the microphone clock. Reserving it after the I2S move
  // disabled the display before SPI initialization, leaving a white screen.
  assert(!voicebot_hardware::audioUsesPin(pins.sck));
  assert(projectPinConflict(pins) == -1);
  assert(beginProjectDisplay(display, pins, bus));
  assert(display.running() && bus.started);
  assert(bus.sck == pins.sck && bus.mosi == pins.mosi);
  assert(sent(bus.bytes, std::vector<uint8_t>{0x11}));  // Sleep out.
  assert(sent(bus.bytes, std::vector<uint8_t>{0x29}));  // Display on.
  expectCommand(bus, 0x36, {0x08});
  bus.bytes.clear();
  display.update(FaceFrame());
  assert(!bus.bytes.empty());
  display.end();
  assert(arduino_stub::levelOf(-1) == -1);
  for (int pin : arduino_stub::modes) assert(pin >= 0);
  for (const auto& event : arduino_stub::writes) assert(event.pin >= 0);
}

static void every_active_audio_button_and_status_pin_is_rejected_before_hardware_use() {
  using namespace voicebot_hardware;
  const int reserved[] = {kMicrophoneBclk, kMicrophoneWs, kMicrophoneData,
      kSpeakerBclk, kSpeakerWs, kSpeakerData, VOICEBOT_BUTTON_PIN, kStatusLed};
  for (int occupied : reserved) {
    for (size_t role = 0; role < 6; ++role) {
      arduino_stub::clear();
      SPIClass bus(HSPI);
      FaceDisplay display;
      Ili9341::Pins pins = configuredWiring();
      int8_t* roles[] = {&pins.sck, &pins.mosi, &pins.dc, &pins.cs, &pins.reset, &pins.backlight};
      *roles[role] = static_cast<int8_t>(occupied);
      assert(projectPinConflict(pins) == occupied);
      assert(!beginProjectDisplay(display, pins, bus));
      assert(!display.running() && bus.beginCalls == 0 && bus.bytes.empty());
      assert(arduino_stub::modes.empty() && arduino_stub::writes.empty());
    }
  }
  // The MAX98357A still receives the shared clocks through GPIO-matrix
  // outputs 38/39, even though the primary I2S clock GPIOs are 48/2.
  assert(audioUsesPin(kSpeakerBclk) && audioUsesPin(kSpeakerWs));
}

static void admission_honours_disabled_optional_pins_and_custom_controls() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  Ili9341::Pins pins = configuredWiring();
  pins.reset = pins.backlight = -1;
  assert(projectPinConflict(pins) == -1);
  assert(beginProjectDisplay(display, pins, bus));
  assert(!bus.bytes.empty() && bus.bytes.front() == 0x01);  // Software reset.
  display.end();
  for (const auto& event : arduino_stub::writes) assert(event.pin >= 0);

  const int optional[] = {-1, -1};
  assert(voicebot_hardware::displayPinConflict(optional, 2, -1, -1) == -1);
  const int customButton[] = {5};
  const int customLed[] = {6};
  assert(voicebot_hardware::displayPinConflict(customButton, 1, 5, 6) == 5);
  assert(voicebot_hardware::displayPinConflict(customLed, 1, 5, 6) == 6);
}

static void bad_wiring_is_refused_and_leaves_the_bus_alone() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  Ili9341::Pins pins = wiring();
  pins.dc = -1;  // DC has no default; the panel cannot be addressed without it.
  assert(!panel.begin(pins, 1, 40000000, bus));
  assert(!panel.running());
  assert(bus.bytes.empty() && !bus.started);
  panel.end();  // Must be safe before a successful begin().
  // Writes on a panel that never started must not touch the bus either.
  const uint16_t pixel = 0x1234;
  panel.beginWindow(0, 0, 1, 1);
  panel.writeRow(&pixel, 1);
  panel.endWindow();
  panel.fill(0, 0, 10, 10, 0);
  assert(bus.bytes.empty());
  assert(bus.depth == 0);
}

static void zero_frequency_is_rejected_before_touching_hardware() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  assert(!panel.begin(wiring(), 1, 0, bus));
  assert(!panel.running() && bus.beginCalls == 0 && bus.bytes.empty());
  assert(arduino_stub::modes.empty() && arduino_stub::writes.empty());
}

static void initialization_is_slow_complete_and_settled_before_fast_pixels() {
  const uint32_t requests[] = {500000, 1000000, 10000000, 40000000};
  for (uint32_t frequency : requests) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    assert(panel.begin(wiring(), 1, frequency, bus));
    assert(bus.bytes.front() == 0x01);  // Software reset follows the hardware pulse too.
    assert(commandTime(bus, 0xEF) - commandTime(bus, 0x01) >= 150);
    assert(commandTime(bus, 0x29) - commandTime(bus, 0x11) >= 150);
    assert(arduino_stub::delayed - commandTime(bus, 0x29) >= 150);
    expectCommand(bus, 0x01, {});
    expectCommand(bus, 0xEF, {0x03, 0x80, 0x02});
    expectCommand(bus, 0xEA, {0x00, 0x00});
    expectCommand(bus, 0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02});
    expectCommand(bus, 0x3A, {0x55});
    expectCommand(bus, 0x36, {0x28});
    expectCommand(bus, 0x11, {});
    expectCommand(bus, 0x29, {});
    const uint32_t expectedInitFrequency = frequency < 1000000 ? frequency : 1000000;
    for (const auto& settings : bus.transactionSettings) {
      assert(settings.frequency == expectedInitFrequency);
      assert(settings.order == MSBFIRST && settings.mode == SPI_MODE0);
    }
    // All commands/parameters use selected CS; command detection above also
    // distinguishes literal parameter bytes from actual command boundaries.
    for (size_t index = 0; index < bus.bytes.size(); ++index) {
      assert(levelAtByte(bus, index, kCs) == LOW);
    }
    const size_t before = bus.transactionSettings.size();
    panel.fill(0, 0, 1, 1, 0xF800);
    assert(bus.transactionSettings.size() == before + 2);
    assert(bus.transactionSettings[before].frequency == expectedInitFrequency);
    assert(bus.transactionSettings.back().frequency == frequency);
    assert(bus.transactionTimes.back() - commandTime(bus, 0x29) >= 150);
    panel.end();
  }
}

static void duplicate_and_invalid_output_pins_are_rejected_before_touching_hardware() {
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    Ili9341::Pins pins = wiring();
    switch (scenario) {
      case 0: pins.dc = pins.sck; break;
      case 1: pins.reset = pins.backlight; break;
      case 2: pins.backlight = pins.cs; break;
      case 3: pins.reset = -2; break;
      case 4: pins.dc = 22; break;  // Not a usable ESP32-S3 GPIO.
      case 5: pins.mosi = 49; break;
      case 6: pins.dc = 127; break;  // Must not cause an oversized GPIO-mask shift.
    }
    assert(!panel.begin(pins, 1, 40000000, bus));
    assert(!panel.running() && bus.beginCalls == 0 && bus.endCalls == 0);
    assert(arduino_stub::modes.empty() && arduino_stub::writes.empty());
  }
}

static void failed_bus_start_is_cleaned_up_and_can_be_retried() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  bus.beginSucceeds = false;
  FaceDisplay display;
  assert(!display.begin(wiring(), 1, 40000000, bus));
  assert(!display.running() && !bus.started);
  assert(bus.beginCalls == 1 && bus.endCalls == 1);
  assert(bus.bytes.empty() && bus.transactions == 0);
  assert(arduino_stub::levelOf(kBacklight) == LOW);
  assert(arduino_stub::levelOf(kCs) == HIGH);
  display.update(FaceFrame());
  display.end();
  assert(bus.bytes.empty() && bus.endCalls == 1);
  bus.beginSucceeds = true;
  assert(display.begin(wiring(), 1, 40000000, bus));
  assert(display.running() && bus.beginCalls == 2);
  display.end();
  assert(bus.endCalls == 2);
}

static void absent_reset_pin_uses_software_reset_and_waits_before_configuration() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  Ili9341::Pins pins = wiring();
  pins.reset = -1;
  pins.backlight = -1;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    bus.bytes.clear();
    bus.transactionTimes.clear();
    assert(panel.begin(pins, 1, 40000000, bus));
    assert(!bus.bytes.empty() && bus.bytes[0] == 0x01);
    assert(bus.transactionTimes.size() >= 2);
    assert(bus.transactionTimes[1] - bus.transactionTimes[0] >= 150);
    panel.end();
  }
  for (size_t index = 0; index < arduino_stub::writes.size(); ++index) {
    assert(arduino_stub::writes[index].pin >= 0);
    assert(arduino_stub::writes[index].pin != kReset);
  }
}

static void begin_resets_the_panel_and_holds_the_backlight_dark() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  assert(panel.begin(wiring(), 1, 40000000, bus));
  assert(panel.running());
  assert(bus.started && bus.sck == kSck && bus.mosi == kMosi);
  // A hardware reset pulse, low then high, before any command.
  bool low = false, pulsed = false;
  uint32_t resetLowAt = 0, resetHighAt = 0;
  for (size_t i = 0; i < bus.byteMarks.front(); ++i) {
    if (arduino_stub::writes[i].pin != kReset) continue;
    if (arduino_stub::writes[i].value == LOW) {
      low = true;
      resetLowAt = arduino_stub::writes[i].atMillis;
    } else if (low) {
      pulsed = true;
      resetHighAt = arduino_stub::writes[i].atMillis;
    }
  }
  assert(pulsed);
  assert(resetHighAt - resetLowAt >= 20);
  assert(bus.bytes.front() == 0x01);
  assert(commandTime(bus, 0x01) - resetHighAt >= 150);
  assert(bus.transactionTimes[1] - bus.transactionTimes[0] >= 150);
  assert(arduino_stub::delayed >= 150);
  // Nothing is shown until a complete face has been drawn.
  assert(arduino_stub::levelOf(kBacklight) == LOW);
  assert(arduino_stub::levelOf(kCs) == HIGH);
  // 16 bits per pixel, sleep out and display on must all be present.
  assert(sent(bus.bytes, std::vector<uint8_t>{0x3A, 0x55}));
  assert(sent(bus.bytes, std::vector<uint8_t>{0x11}));
  assert(sent(bus.bytes, std::vector<uint8_t>{0x29}));
  // Power control A consumes all five parameters before the next command.
  assert(sent(bus.bytes, std::vector<uint8_t>{0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02, 0xF7}));
  panel.end();
  assert(!panel.running() && !bus.started);
  assert(arduino_stub::levelOf(kBacklight) == LOW);
}

static void rotation_selects_the_madctl_value_and_the_axis_order() {
  const uint8_t expected[4] = {0x48, 0x28, 0x88, 0xE8};
  for (uint8_t rotation = 0; rotation < 4; ++rotation) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    assert(panel.begin(wiring(), rotation, 40000000, bus));
    assert(sent(bus.bytes, std::vector<uint8_t>{0x36, expected[rotation]}));
    const bool landscape = (rotation & 1) != 0;
    assert(panel.width() == (landscape ? 320 : 240));
    assert(panel.height() == (landscape ? 240 : 320));
    panel.end();
  }
}

static void window_addresses_recover_when_the_startup_rotation_write_is_lost() {
  const uint8_t expected[4] = {0x48, 0x28, 0x88, 0xE8};
  for (uint8_t rotation = 0; rotation < 4; ++rotation) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    assert(panel.begin(wiring(), rotation, 10000000, bus));
    panel.fill(0, 0, panel.width(), panel.height(), 0xFFFF);
    // Exercise the last physical rows/columns, including x=319 in landscape.
    panel.fill(panel.width() - 2, panel.height() - 2, 2, 2, 0xF800);

    // Replay the address commands as a panel left in its power-on portrait
    // mode after losing the initial MADCTL write. ILI9341 sections 8.2.20/21
    // limit CASET to 239 without MV, or 319 with MV; PASET has the other bound.
    // Reject out-of-range writes instead of assuming the software dimensions
    // prove the controller received the orientation.
    uint8_t madctl = 0;
    unsigned rotationWrites = 0;
    uint16_t x = 0, y = 0, lastX = 239, lastY = 319;
    std::vector<Rect> windows;
    size_t start = 0;
    while (start < bus.bytes.size()) {
      assert(levelAtByte(bus, start, kDc) == LOW);
      size_t end = start + 1;
      while (end < bus.bytes.size() && levelAtByte(bus, end, kDc) == HIGH) ++end;
      const size_t length = end - start - 1;
      const uint8_t command = bus.bytes[start];
      const uint8_t* payload = bus.bytes.data() + start + 1;
      if (command == 0x36) {
        assert(length == 1);
        if (rotationWrites++ != 0) madctl = payload[0];  // Drop startup write.
        // Each retry must retain the conservative clock used at startup.
        bool found = false;
        for (size_t i = 0; i < bus.transactionByteStarts.size(); ++i) {
          if (bus.transactionByteStarts[i] != start) continue;
          assert(bus.transactionSettings[i].frequency == 1000000);
          found = true;
          break;
        }
        assert(found);
      } else if (command == 0x2A || command == 0x2B) {
        assert(length == 4 && madctl == expected[rotation]);
        const uint16_t first = static_cast<uint16_t>((payload[0] << 8) | payload[1]);
        const uint16_t last = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
        const bool landscape = (madctl & 0x20) != 0;
        const uint16_t limit = command == 0x2A ? (landscape ? 319 : 239)
                                                : (landscape ? 239 : 319);
        assert(first <= last && last <= limit);
        if (command == 0x2A) { x = first; lastX = last; }
        else { y = first; lastY = last; }
      } else if (command == 0x2C) {
        const uint16_t width = static_cast<uint16_t>(lastX - x + 1);
        const uint16_t height = static_cast<uint16_t>(lastY - y + 1);
        assert(length == static_cast<size_t>(width) * height * 2);
        windows.push_back({static_cast<int16_t>(x), static_cast<int16_t>(y),
                           static_cast<int16_t>(width), static_cast<int16_t>(height)});
      }
      start = end;
    }
    assert(rotationWrites == 3 && windows.size() == 2);
    assert(windows[0].x == 0 && windows[0].y == 0);
    assert(windows[0].w == panel.width() && windows[0].h == panel.height());
    assert(windows[1].x == panel.width() - 2 && windows[1].y == panel.height() - 2);
    assert(windows[1].w == 2 && windows[1].h == 2);
    panel.end();
  }
}

static void a_window_write_sends_exact_bounds_and_big_endian_pixels() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  assert(panel.begin(wiring(), 1, 40000000, bus));
  bus.bytes.clear();
  const uint32_t transactionsBefore = bus.transactions;
  panel.beginWindow(10, 20, 4, 2);
  const uint16_t row[4] = {0x0000, 0xFFFF, 0xF800, 0x001F};
  panel.writeRow(row, 4);
  panel.writeRow(row, 4);
  panel.endWindow();
  // Restore landscape, CASET 10..13, PASET 20..21, RAMWR, then the pixels.
  const std::vector<uint8_t> expected{0x36, 0x28,
                                      0x2A, 0x00, 0x0A, 0x00, 0x0D,
                                      0x2B, 0x00, 0x14, 0x00, 0x15,
                                      0x2C,
                                      0x00, 0x00, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x1F,
                                      0x00, 0x00, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x1F};
  assert(bus.bytes == expected);
  for (size_t index = 0; index < expected.size(); ++index) {
    assert(levelAtByte(bus, index, kCs) == LOW);
    const bool isCommand = index == 0 || index == 2 || index == 7 || index == 12;
    assert(levelAtByte(bus, index, kDc) == (isCommand ? LOW : HIGH));
  }
  // One safe-clock orientation transaction, then one pixel transaction.
  assert(bus.transactions == transactionsBefore + 2);
  assert(bus.transactionSettings[transactionsBefore].frequency == 1000000);
  assert(bus.transactionSettings[transactionsBefore + 1].frequency == 40000000);
  assert(arduino_stub::levelOf(kCs) == HIGH);
  panel.end();
}

static void odd_and_maximum_rows_use_safe_bulk_storage_without_extra_wire_pixels() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  assert(panel.begin(wiring(), 1, 40000000, bus));
  uint16_t row[Ili9341::kMaxSpanPixels];
  for (size_t index = 0; index < Ili9341::kMaxSpanPixels; ++index) {
    row[index] = static_cast<uint16_t>(0x9000 + index);
  }
  const size_t counts[] = {1, 3, 319, 320};
  for (size_t count : counts) {
    bus.bytes.clear();
    panel.beginWindow(0, 0, static_cast<int16_t>(count), 1);
    panel.writeRow(row, count);
    panel.endWindow();
    assert(bus.bytes.size() == 13 + count * 2);
    for (size_t index = 0; index < count; ++index) {
      assert(bus.bytes[13 + index * 2] == static_cast<uint8_t>(row[index] >> 8));
      assert(bus.bytes[14 + index * 2] == static_cast<uint8_t>(row[index]));
    }
  }
  panel.end();
}

static void fill_covers_every_pixel_of_the_rectangle() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  Ili9341 panel;
  assert(panel.begin(wiring(), 1, 40000000, bus));
  bus.bytes.clear();
  panel.fill(0, 0, 320, 240, 0xFFFF);
  // Address bytes plus 320 * 240 pixels of two bytes each.
  assert(bus.bytes.size() == 13u + 320u * 240u * 2u);
  for (size_t i = 13; i < bus.bytes.size(); ++i) assert(bus.bytes[i] == 0xFF);
  // A zero or negative rectangle is a no-op rather than a runaway window.
  bus.bytes.clear();
  panel.fill(0, 0, 0, 10, 0);
  panel.fill(0, 0, 10, -1, 0);
  assert(bus.bytes.empty());
  panel.end();
}

static void software_landscape_clears_the_entire_native_panel_and_rotates_asymmetric_pixels() {
  const uint8_t rotations[] = {1, 3};
  for (uint8_t rotation : rotations) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    assert(panel.begin(wiring(), rotation, 10000000, bus, true));
    assert(panel.width() == 320 && panel.height() == 240);
    panel.fill(0, 0, 320, 240, 0xFFFF);
    NativeGram gram;
    gram.consume(bus);
    assert(gram.windows == 1 && gram.orientationWrites == 2);
    for (size_t i = 0; i < gram.pixels.size(); ++i) {
      assert(gram.pixels[i] == 0xFFFF && gram.writes[i] == 1);
    }

    // Deliberately asymmetric across both axes, with distinctive corner
    // colours, so transpose, mirror, missing columns and byte swaps disagree.
    std::vector<uint16_t> logical(320u * 240u);
    for (size_t y = 0; y < 240; ++y) {
      for (size_t x = 0; x < 320; ++x) {
        logical[y * 320 + x] = static_cast<uint16_t>(0x1234u + x * 71u + y * 353u);
      }
    }
    logical[0] = 0xF800;
    logical[319] = 0x07E0;
    logical[239u * 320u] = 0x001F;
    logical.back() = 0xABCD;
    bus.bytes.clear();
    gram.clearWriteCounts();
    panel.beginWindow(0, 0, 320, 240);
    for (size_t y = 0; y < 240; ++y) panel.writeRow(logical.data() + y * 320, 320);
    panel.endWindow();
    gram.consume(bus);
    gram.expectLandscape(logical, rotation);
    for (uint16_t count : gram.writes) assert(count == 1);
    assert(bus.depth == 0 && arduino_stub::levelOf(kCs) == HIGH);
    panel.end();
  }
}

static void software_landscape_preserves_partial_streams_corners_and_untouched_pixels() {
  const uint8_t rotations[] = {1, 3};
  for (uint8_t rotation : rotations) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    Ili9341 panel;
    assert(panel.begin(wiring(), rotation, 10000000, bus, true));
    panel.fill(0, 0, 320, 240, 0xFFFF);
    NativeGram gram;
    gram.consume(bus);
    std::vector<uint16_t> logical(320u * 240u, 0xFFFF);
    bus.bytes.clear();
    gram.clearWriteCounts();

    // Solid dirty rectangles exercise the fast fill path at every corner.
    const Rect corners[] = {{0, 0, 2, 3}, {318, 0, 2, 3},
                            {0, 237, 2, 3}, {318, 237, 2, 3}};
    const uint16_t colours[] = {0xF800, 0x07E0, 0x001F, 0xABCD};
    for (size_t i = 0; i < 4; ++i) {
      const Rect rect = corners[i];
      panel.fill(rect.x, rect.y, rect.w, rect.h, colours[i]);
      for (int y = rect.y; y < rect.y + rect.h; ++y) {
        for (int x = rect.x; x < rect.x + rect.w; ++x) logical[y * 320 + x] = colours[i];
      }
    }

    const Rect partial = {17, 23, 7, 3};
    uint16_t source[24];
    for (size_t i = 0; i < 24; ++i) source[i] = static_cast<uint16_t>(0x8100 + i * 37);
    panel.beginWindow(partial.x, partial.y, partial.w, partial.h);
    // Calls start/end midway through rows; the last call extends beyond the
    // window. Rotation 3 must reverse each native segment without reversing
    // the logical stream or writing the final three excess pixels.
    const size_t chunks[] = {2, 8, 1, 9, 4};
    size_t consumed = 0;
    for (size_t count : chunks) {
      panel.writeRow(source + consumed, count);
      consumed += count;
    }
    const size_t completeSize = bus.bytes.size();
    panel.writeRow(source, 1);  // The completed window accepts no more pixels.
    assert(bus.bytes.size() == completeSize);
    panel.endWindow();
    panel.writeRow(source, 1);  // Nor does a closed window.
    assert(bus.bytes.size() == completeSize);
    for (size_t i = 0; i < 21; ++i) {
      logical[(partial.y + i / 7) * 320 + partial.x + i % 7] = source[i];
    }
    gram.consume(bus);
    gram.expectLandscape(logical, rotation);
    size_t touched = 0;
    for (uint16_t count : gram.writes) {
      assert(count <= 1);
      touched += count;
    }
    assert(touched == 4u * 2u * 3u + 21u);

    bus.bytes.clear();
    panel.beginWindow(10, 10, 2, 2);
    const size_t beforeInvalid = bus.bytes.size();
    panel.beginWindow(319, 239, 2, 2);  // Must close the previous valid window.
    panel.writeRow(source, 4);
    panel.endWindow();
    assert(bus.bytes.size() == beforeInvalid);
    assert(bus.depth == 0 && arduino_stub::levelOf(kCs) == HIGH);
    bus.bytes.clear();
    const Rect invalid[] = {{-1, 0, 1, 1}, {0, -1, 1, 1}, {320, 0, 1, 1},
                            {0, 240, 1, 1}, {0, 0, 0, 1}, {0, 0, 1, -1}};
    for (const Rect& rect : invalid) {
      panel.fill(rect.x, rect.y, rect.w, rect.h, 0);
      panel.beginWindow(rect.x, rect.y, rect.w, rect.h);
      panel.writeRow(source, 1);
      panel.endWindow();
    }
    assert(bus.bytes.empty() && bus.depth == 0);
    panel.end();
  }
}

static void software_landscape_face_updates_only_the_dirty_mouth_in_native_gram() {
  const uint8_t rotations[] = {1, 3};
  for (uint8_t rotation : rotations) {
    arduino_stub::clear();
    SPIClass bus(HSPI);
    FaceDisplay display;
    assert(display.begin(wiring(), rotation, 10000000, bus, Layout(), Palette(), true));
    FaceFrame frame;
    frame.gazeX = 7;
    frame.leftEyeOpen = 150;
    display.update(frame);
    NativeGram gram;
    gram.consume(bus);
    std::vector<uint16_t> logical(320u * 240u, 0xFFFF);
    const FaceRenderer& renderer = display.renderer();
    uint16_t row[FaceRenderer::kMaxRegionWidth];
    for (uint8_t region = 0; region < FaceRenderer::kRegionCount; ++region) {
      const Rect rect = renderer.region(region);
      for (int16_t y = 0; y < rect.h; ++y) {
        renderer.renderRow(region, frame, y, row);
        for (int16_t x = 0; x < rect.w; ++x) logical[(rect.y + y) * 320 + rect.x + x] = row[x];
      }
    }
    gram.expectLandscape(logical, rotation);
    bus.bytes.clear();
    display.update(frame);
    assert(bus.bytes.empty());

    gram.clearWriteCounts();
    frame.mouthOpen = 200;
    display.update(frame);
    gram.consume(bus);
    const Rect mouth = renderer.region(FaceRenderer::kMouth);
    for (int16_t y = 0; y < mouth.h; ++y) {
      renderer.renderRow(FaceRenderer::kMouth, frame, y, row);
      for (int16_t x = 0; x < mouth.w; ++x) logical[(mouth.y + y) * 320 + mouth.x + x] = row[x];
    }
    gram.expectLandscape(logical, rotation);
    for (int16_t nativeY = 0; nativeY < 320; ++nativeY) {
      for (int16_t nativeX = 0; nativeX < 240; ++nativeX) {
        const int16_t x = rotation == 1 ? nativeY : 319 - nativeY;
        const int16_t y = rotation == 1 ? 239 - nativeX : nativeX;
        const bool inside = x >= mouth.x && x < mouth.x + mouth.w &&
                            y >= mouth.y && y < mouth.y + mouth.h;
        assert(gram.writes[nativeY * 240 + nativeX] == (inside ? 1 : 0));
      }
    }
    display.end();
  }
}

static void the_first_face_paints_everything_and_lights_the_backlight() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  assert(display.begin(wiring(), 1, 40000000, bus));
  assert(arduino_stub::levelOf(kBacklight) == LOW);  // Still dark after the clear.
  bus.bytes.clear();
  FaceFrame frame;
  display.update(frame);
  assert(arduino_stub::levelOf(kBacklight) == HIGH);
  const FaceRenderer& renderer = display.renderer();
  size_t pixels = 0;
  for (uint8_t index = 0; index < FaceRenderer::kRegionCount; ++index) {
    const Rect rect = renderer.region(index);
    pixels += static_cast<size_t>(rect.w) * static_cast<size_t>(rect.h);
  }
  // Every region, plus orientation and 11 address bytes each.
  assert(bus.bytes.size() == pixels * 2 + FaceRenderer::kRegionCount * 13);
  display.end();
}

static void only_changed_regions_reach_the_bus() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  assert(display.begin(wiring(), 1, 40000000, bus));
  FaceFrame frame;
  display.update(frame);
  const FaceRenderer& renderer = display.renderer();

  // An identical frame must cost nothing at all.
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.empty());

  // A talking mouth must not redraw either eye; this is what keeps 30 frames
  // per second of lip sync affordable next to the audio tasks.
  const Rect mouth = renderer.region(FaceRenderer::kMouth);
  frame.mouthOpen = 200;
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.size() ==
         static_cast<size_t>(mouth.w) * static_cast<size_t>(mouth.h) * 2 + 13);

  // A wink redraws one eye only.
  const Rect right = renderer.region(FaceRenderer::kRightEye);
  frame.rightEyeOpen = 0;
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.size() ==
         static_cast<size_t>(right.w) * static_cast<size_t>(right.h) * 2 + 13);

  // Gaze moves both eyes and leaves the mouth alone.
  const Rect left = renderer.region(FaceRenderer::kLeftEye);
  frame.gazeX = 7;
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.size() ==
         (static_cast<size_t>(left.w) * left.h + static_cast<size_t>(right.w) * right.h) * 2 + 26);
  display.end();
  // Updates after end() must be dropped, not sent to a closed bus.
  bus.bytes.clear();
  frame.mouthOpen = 10;
  display.update(frame);
  assert(bus.bytes.empty());
}

static void a_layout_the_renderer_rejects_never_opens_the_panel() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  Layout broken = Layout();
  broken.mouthCenterY = 130;  // Overlaps the eye windows.
  assert(!display.begin(wiring(), 1, 40000000, bus, broken, Palette()));
  assert(!display.running());
  assert(bus.bytes.empty() && !bus.started);

  // A layout larger than the selected rotation is refused too, rather than
  // silently clipping the face off the bottom of the panel.
  arduino_stub::clear();
  SPIClass portraitBus(HSPI);
  FaceDisplay portrait;
  assert(!portrait.begin(wiring(), 0, 40000000, portraitBus));  // 240 wide, face is 320.
  assert(!portrait.running());
}

static void invalid_reinitialization_closes_the_previous_display() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  assert(display.begin(wiring(), 1, 40000000, bus));
  display.update(FaceFrame());
  Layout broken;
  broken.mouthCenterY = 130;
  assert(!display.begin(wiring(), 1, 40000000, bus, broken));
  assert(!display.running() && !bus.started && bus.endCalls == 1);
  assert(arduino_stub::levelOf(kBacklight) == LOW);
}

int main() {
  configured_goouuu_pins_pass_the_project_guard_and_initialize_the_panel();
  every_active_audio_button_and_status_pin_is_rejected_before_hardware_use();
  admission_honours_disabled_optional_pins_and_custom_controls();
  bad_wiring_is_refused_and_leaves_the_bus_alone();
  zero_frequency_is_rejected_before_touching_hardware();
  initialization_is_slow_complete_and_settled_before_fast_pixels();
  duplicate_and_invalid_output_pins_are_rejected_before_touching_hardware();
  failed_bus_start_is_cleaned_up_and_can_be_retried();
  absent_reset_pin_uses_software_reset_and_waits_before_configuration();
  begin_resets_the_panel_and_holds_the_backlight_dark();
  rotation_selects_the_madctl_value_and_the_axis_order();
  window_addresses_recover_when_the_startup_rotation_write_is_lost();
  a_window_write_sends_exact_bounds_and_big_endian_pixels();
  odd_and_maximum_rows_use_safe_bulk_storage_without_extra_wire_pixels();
  fill_covers_every_pixel_of_the_rectangle();
  software_landscape_clears_the_entire_native_panel_and_rotates_asymmetric_pixels();
  software_landscape_preserves_partial_streams_corners_and_untouched_pixels();
  software_landscape_face_updates_only_the_dirty_mouth_in_native_gram();
  the_first_face_paints_everything_and_lights_the_backlight();
  only_changed_regions_reach_the_bus();
  a_layout_the_renderer_rejects_never_opens_the_panel();
  invalid_reinitialization_closes_the_previous_display();
  printf("test_face_panel: all cases passed\n");
  return 0;
}
