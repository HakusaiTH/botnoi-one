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
  return display.begin(pins, VOICEBOT_DISPLAY_ROTATION, VOICEBOT_DISPLAY_SPI_HZ, bus);
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

}  // namespace

static void configured_goouuu_pins_pass_the_project_guard_and_initialize_the_panel() {
  arduino_stub::clear();
  SPIClass bus(HSPI);
  FaceDisplay display;
  const Ili9341::Pins pins = configuredWiring();
  assert(pins.sck == 3 && pins.backlight == -1);
  assert(VOICEBOT_DISPLAY_SPI_HZ == 10000000);
  assert(voicebot_hardware::kMicrophoneBclk == 42);
  // GPIO3 used to be the microphone clock. Reserving it after the I2S move
  // disabled the display before SPI initialization, leaving a white screen.
  assert(!voicebot_hardware::audioUsesPin(pins.sck));
  assert(projectPinConflict(pins) == -1);
  assert(beginProjectDisplay(display, pins, bus));
  assert(display.running() && bus.started);
  assert(bus.sck == pins.sck && bus.mosi == pins.mosi);
  assert(sent(bus.bytes, std::vector<uint8_t>{0x11}));  // Sleep out.
  assert(sent(bus.bytes, std::vector<uint8_t>{0x29}));  // Display on.
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
  // outputs 38/39, even though the primary I2S clock GPIOs are 42/2.
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
    assert(bus.transactionSettings.size() == before + 1);
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
  // CASET 10..13, PASET 20..21, RAMWR, then the pixels.
  const std::vector<uint8_t> expected{0x2A, 0x00, 0x0A, 0x00, 0x0D,
                                      0x2B, 0x00, 0x14, 0x00, 0x15,
                                      0x2C,
                                      0x00, 0x00, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x1F,
                                      0x00, 0x00, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x1F};
  assert(bus.bytes == expected);
  for (size_t index = 0; index < expected.size(); ++index) {
    assert(levelAtByte(bus, index, kCs) == LOW);
    const bool isCommand = index == 0 || index == 5 || index == 10;
    assert(levelAtByte(bus, index, kDc) == (isCommand ? LOW : HIGH));
  }
  // One region costs exactly one transaction and one chip-select assertion.
  assert(bus.transactions == transactionsBefore + 1);
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
    assert(bus.bytes.size() == 11 + count * 2);
    for (size_t index = 0; index < count; ++index) {
      assert(bus.bytes[11 + index * 2] == static_cast<uint8_t>(row[index] >> 8));
      assert(bus.bytes[12 + index * 2] == static_cast<uint8_t>(row[index]));
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
  assert(bus.bytes.size() == 11u + 320u * 240u * 2u);
  for (size_t i = 11; i < bus.bytes.size(); ++i) assert(bus.bytes[i] == 0xFF);
  // A zero or negative rectangle is a no-op rather than a runaway window.
  bus.bytes.clear();
  panel.fill(0, 0, 0, 10, 0);
  panel.fill(0, 0, 10, -1, 0);
  assert(bus.bytes.empty());
  panel.end();
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
  // Every region, plus 11 address bytes each.
  assert(bus.bytes.size() == pixels * 2 + FaceRenderer::kRegionCount * 11);
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
         static_cast<size_t>(mouth.w) * static_cast<size_t>(mouth.h) * 2 + 11);

  // A wink redraws one eye only.
  const Rect right = renderer.region(FaceRenderer::kRightEye);
  frame.rightEyeOpen = 0;
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.size() ==
         static_cast<size_t>(right.w) * static_cast<size_t>(right.h) * 2 + 11);

  // Gaze moves both eyes and leaves the mouth alone.
  const Rect left = renderer.region(FaceRenderer::kLeftEye);
  frame.gazeX = 7;
  bus.bytes.clear();
  display.update(frame);
  assert(bus.bytes.size() ==
         (static_cast<size_t>(left.w) * left.h + static_cast<size_t>(right.w) * right.h) * 2 + 22);
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
  a_window_write_sends_exact_bounds_and_big_endian_pixels();
  odd_and_maximum_rows_use_safe_bulk_storage_without_extra_wire_pixels();
  fill_covers_every_pixel_of_the_rectangle();
  the_first_face_paints_everything_and_lights_the_backlight();
  only_changed_regions_reach_the_bus();
  a_layout_the_renderer_rejects_never_opens_the_panel();
  invalid_reinitialization_closes_the_previous_display();
  printf("test_face_panel: all cases passed\n");
  return 0;
}
