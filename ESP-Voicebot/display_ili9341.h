#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <stddef.h>
#include <stdint.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <driver/gpio.h>
#endif

namespace voicebot_face {

// Minimal ILI9341 driver: only a rectangle fill and repeated window writes from
// a caller-owned row buffer, which is all the animated face needs. No fonts, no
// glyph tables, no touch controller and no framebuffer. Pixel storage is fixed;
// the underlying SPI core still allocates its own control state.
// Pixels are byte-swapped here rather than relying on a core-specific
// writePixels() byte order.
class Ili9341 {
 public:
  struct Pins {
    int8_t sck;
    int8_t mosi;
    int8_t dc;
    int8_t cs;
    int8_t reset;      // -1 when the panel's RESET is tied to the board's.
    int8_t backlight;  // -1 when LED is wired permanently on.
  };

  // Widest span a single write may cover; also sizes the byte-swap buffer.
  static const size_t kMaxSpanPixels = 320;

  bool begin(const Pins& pins, uint8_t rotation, uint32_t frequency, SPIClass& bus,
             bool softwareLandscape = false) {
    end();  // Reinitialization is supported only from the same owning task.
    if (!frequency || !validPins(pins)) return false;
    pins_ = pins;
    bus_ = &bus;
    // Keep controller setup conservative even when pixels use a faster clock.
    // A missed COLMOD/MADCTL parameter can corrupt every later frame.
    controlSettings_ = SPISettings(frequency < 1000000 ? frequency : 1000000, MSBFIRST, SPI_MODE0);
    settings_ = SPISettings(frequency, MSBFIRST, SPI_MODE0);
    pinMode(pins_.dc, OUTPUT);
    pinMode(pins_.cs, OUTPUT);
    digitalWrite(pins_.dc, HIGH);
    digitalWrite(pins_.cs, HIGH);
    if (pins_.backlight >= 0) {
      pinMode(pins_.backlight, OUTPUT);
      digitalWrite(pins_.backlight, LOW);  // Stay dark until the face is drawn.
    }
    if (pins_.reset >= 0) {
      pinMode(pins_.reset, OUTPUT);
      digitalWrite(pins_.reset, HIGH);
      delay(5);
      digitalWrite(pins_.reset, LOW);
      delay(20);
      digitalWrite(pins_.reset, HIGH);
      delay(150);
    }
    if (!bus_->begin(pins_.sck, -1, pins_.mosi, -1)) {
      // SPIClass can retain a started bus after a pin-attachment failure.
      bus_->end();
      bus_ = nullptr;
      return false;
    }
    // Also reset over SPI when RESET is configured: the module can remain
    // powered across MCU-only restarts, and its reset wire may be absent.
    send(0x01, nullptr, 0);  // SWRESET
    delay(150);
    sendInitSequence();
    setRotation(rotation, softwareLandscape);
    running_ = true;
    return true;
  }

  void end() {
    if (!running_) return;
    endWindow();
    running_ = false;
    backlight(false);
    send(0x28, nullptr, 0);  // DISPOFF
    bus_->end();
    bus_ = nullptr;
  }

  bool running() const { return running_; }
  int16_t width() const { return width_; }
  int16_t height() const { return height_; }

  void backlight(bool on) {
    if (pins_.backlight >= 0) digitalWrite(pins_.backlight, on ? HIGH : LOW);
  }

  void fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t colour) {
    if (!running_ || w <= 0 || h <= 0) return;
    if (softwareLandscape_) {
      if (!validLogicalWindow(x, y, w, h)) return;
      // A solid rectangle needs no pixel transpose. Transform its bounds and
      // stream it directly in the panel's native 240 x 320 address space.
      const int16_t nativeX = rotation_ == 1 ? static_cast<int16_t>(240 - y - h) : y;
      const int16_t nativeY = rotation_ == 1 ? x : static_cast<int16_t>(320 - x - w);
      const int16_t nativeWidth = h;
      h = w;
      w = nativeWidth;
      x = nativeX;
      y = nativeY;
    }
    size_t span = static_cast<size_t>(w);
    if (span > kMaxSpanPixels) span = kMaxSpanPixels;
    uint16_t row[kMaxSpanPixels];
    for (size_t i = 0; i < span; ++i) row[i] = colour;
    for (int16_t column = 0; column < w; column += static_cast<int16_t>(span)) {
      const int16_t chunk = w - column < static_cast<int16_t>(span) ? static_cast<int16_t>(w - column)
                                                                   : static_cast<int16_t>(span);
      beginPixelTransaction();
      writeWindow(static_cast<int16_t>(x + column), y, chunk, h);
      for (int16_t line = 0; line < h; ++line) writePixels(row, static_cast<size_t>(chunk), false);
      endWindow();
    }
  }

  // Restore orientation at the safe control clock, then hold chip select for
  // the window and all writeRow() calls in one pixel transaction.
  void beginWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!running_) return;
    // A rejected replacement must not leave writes targeting the old window.
    endWindow();
    if (w <= 0 || h <= 0) return;
    if (softwareLandscape_ && !validLogicalWindow(x, y, w, h)) return;
    beginPixelTransaction();
    if (softwareLandscape_) {
      windowX_ = x;
      windowY_ = y;
      windowWidth_ = w;
      windowHeight_ = h;
      windowColumn_ = 0;
      windowRow_ = 0;
    } else {
      writeWindow(x, y, w, h);
    }
  }

  void writeRow(const uint16_t* pixels, size_t count) {
    if (!running_ || !windowOpen_ || !pixels || !count) return;
    if (count > kMaxSpanPixels) count = kMaxSpanPixels;
    if (!softwareLandscape_) {
      writePixels(pixels, count, false);
      return;
    }
    // Keep MADCTL in portrait mode. A landscape row becomes a native column,
    // so no framebuffer or transposed tile storage is needed. Splitting at a
    // logical row boundary also preserves partial-row streaming semantics.
    while (count && windowRow_ < windowHeight_) {
      const size_t remaining = static_cast<size_t>(windowWidth_ - windowColumn_);
      const size_t chunk = count < remaining ? count : remaining;
      const int16_t x = static_cast<int16_t>(windowX_ + windowColumn_);
      const int16_t y = static_cast<int16_t>(windowY_ + windowRow_);
      const bool reverse = rotation_ == 3;
      const int16_t nativeX = reverse ? y : static_cast<int16_t>(239 - y);
      const int16_t nativeY = reverse ? static_cast<int16_t>(320 - x - chunk) : x;
      writeWindow(nativeX, nativeY, 1, static_cast<int16_t>(chunk));
      writePixels(pixels, chunk, reverse);
      pixels += chunk;
      count -= chunk;
      windowColumn_ = static_cast<int16_t>(windowColumn_ + chunk);
      if (windowColumn_ == windowWidth_) {
        windowColumn_ = 0;
        ++windowRow_;
      }
    }
  }

  void endWindow() {
    if (!windowOpen_) return;
    digitalWrite(pins_.cs, HIGH);
    bus_->endTransaction();
    windowOpen_ = false;
  }

 private:
  bool validLogicalWindow(int16_t x, int16_t y, int16_t w, int16_t h) const {
    return x >= 0 && y >= 0 && w > 0 && h > 0 &&
           static_cast<int32_t>(x) + w <= width_ && static_cast<int32_t>(y) + h <= height_;
  }

  void beginPixelTransaction() {
    endWindow();
    // With no MISO readback a missed startup MADCTL write is undetectable.
    // Reassert the selected addressing mode before CASET/PASET at a safe clock.
    send(0x36, &madctl_, 1);
    bus_->beginTransaction(settings_);
    digitalWrite(pins_.cs, LOW);
    windowOpen_ = true;
  }

  // Set a physical address window while the pixel transaction owns CS.
  void writeWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
    const uint16_t lastX = static_cast<uint16_t>(x + w - 1);
    const uint16_t lastY = static_cast<uint16_t>(y + h - 1);
    const uint8_t columns[4] = {static_cast<uint8_t>(x >> 8), static_cast<uint8_t>(x),
                                static_cast<uint8_t>(lastX >> 8), static_cast<uint8_t>(lastX)};
    const uint8_t rows[4] = {static_cast<uint8_t>(y >> 8), static_cast<uint8_t>(y),
                             static_cast<uint8_t>(lastY >> 8), static_cast<uint8_t>(lastY)};
    write(0x2A, columns, 4);  // CASET
    write(0x2B, rows, 4);     // PASET
    digitalWrite(pins_.dc, LOW);
    bus_->write(0x2C);        // RAMWR
    digitalWrite(pins_.dc, HIGH);
  }

  void writePixels(const uint16_t* pixels, size_t count, bool reverse) {
    for (size_t i = 0; i < count; ++i) {
      const uint16_t pixel = pixels[reverse ? count - 1 - i : i];
      swap_[i] = static_cast<uint16_t>((pixel << 8) | (pixel >> 8));
    }
    // ESP32 SPI loads whole 32-bit words, even for an odd pixel count. The
    // extra half-word remains inside this buffer and is never sent on the wire.
    if (count & 1) swap_[count] = 0;
    bus_->writeBytes(reinterpret_cast<const uint8_t*>(swap_), count * 2);
  }

  static bool validPins(const Pins& pins) {
    const int8_t assigned[] = {pins.sck, pins.mosi, pins.dc, pins.cs, pins.reset, pins.backlight};
    for (size_t index = 0; index < sizeof(assigned) / sizeof(assigned[0]); ++index) {
      const int pin = assigned[index];
      if (index >= 4 && pin == -1) continue;
      if (pin < 0) return false;
#if defined(ARDUINO_ARCH_ESP32)
      // Bound the shift used by GPIO_IS_VALID_OUTPUT_GPIO as well as checking
      // this chip's output-capable pin mask. Board-specific conflicts belong
      // to the application, before it calls begin().
      if (pin >= 64 || !GPIO_IS_VALID_OUTPUT_GPIO(pin)) return false;
#endif
      for (size_t previous = 0; previous < index; ++previous) {
        if (assigned[previous] == pin) return false;
      }
    }
    return true;
  }

  // Writes one command plus its parameters. Chip select must already be held.
  void write(uint8_t command, const uint8_t* data, size_t length) {
    digitalWrite(pins_.dc, LOW);
    bus_->write(command);
    digitalWrite(pins_.dc, HIGH);
    // Parameters are at most 15 bytes and may have byte alignment. The ESP32
    // bulk API performs rounded-up word loads, so use byte writes here.
    if (data) for (size_t index = 0; index < length; ++index) bus_->write(data[index]);
  }

  void send(uint8_t command, const uint8_t* data, size_t length) {
    bus_->beginTransaction(controlSettings_);
    digitalWrite(pins_.cs, LOW);
    write(command, data, length);
    digitalWrite(pins_.cs, HIGH);
    bus_->endTransaction();
  }

  void setRotation(uint8_t rotation, bool softwareLandscape) {
    // MADCTL, BGR panels. Odd rotations swap the axes into landscape.
    static const uint8_t kMadctl[4] = {0x48, 0x28, 0x88, 0xE8};
    rotation_ = static_cast<uint8_t>(rotation & 0x03);
    const bool landscape = (rotation_ & 1) != 0;
    softwareLandscape_ = softwareLandscape && landscape;
    // Software mapping uses unmirrored native axes, matching reset addressing
    // even when the panel misses this write; only the BGR colour bit is set.
    madctl_ = softwareLandscape_ ? 0x08 : kMadctl[rotation_];
    send(0x36, &madctl_, 1);
    width_ = landscape ? 320 : 240;
    height_ = landscape ? 240 : 320;
  }

  void sendInitSequence() {
    static const uint8_t kGammaPositive[15] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                                               0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
    static const uint8_t kGammaNegative[15] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                                               0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};
    static const uint8_t kPowerA[5] = {0x39, 0x2C, 0x00, 0x34, 0x02};
    static const uint8_t kExtendedSetup[3] = {0x03, 0x80, 0x02};
    static const uint8_t kPowerB[3] = {0x00, 0xC1, 0x30};
    static const uint8_t kDriverTiming[3] = {0x85, 0x00, 0x78};
    static const uint8_t kDriverTimingB[2] = {0x00, 0x00};
    static const uint8_t kPowerSequence[4] = {0x64, 0x03, 0x12, 0x81};
    static const uint8_t kPumpRatio[1] = {0x20};
    static const uint8_t kVcom1[2] = {0x3E, 0x28};
    static const uint8_t kFrameRate[2] = {0x00, 0x18};
    static const uint8_t kDisplayFunction[3] = {0x08, 0x82, 0x27};
    const uint8_t pixelFormat = 0x55;  // 16 bits per pixel.
    const uint8_t power1 = 0x23, power2 = 0x10, vcom2 = 0x86, gammaSet = 0x01, gamma3 = 0x00;
    // Include the extended setup used by Adafruit_ILI9341, plus the driver
    // timing B write also present in TFT_eSPI's ILI9341 initialization.
    send(0xEF, kExtendedSetup, sizeof(kExtendedSetup));
    send(0xCF, kPowerB, 3);
    send(0xED, kPowerSequence, 4);
    send(0xE8, kDriverTiming, 3);
    send(0xCB, kPowerA, sizeof(kPowerA));
    send(0xF7, kPumpRatio, 1);
    send(0xEA, kDriverTimingB, sizeof(kDriverTimingB));
    send(0xC0, &power1, 1);
    send(0xC1, &power2, 1);
    send(0xC5, kVcom1, 2);
    send(0xC7, &vcom2, 1);
    send(0x3A, &pixelFormat, 1);
    send(0xB1, kFrameRate, 2);
    send(0xB6, kDisplayFunction, 3);
    send(0xF2, &gamma3, 1);
    send(0x26, &gammaSet, 1);
    send(0xE0, kGammaPositive, 15);
    send(0xE1, kGammaNegative, 15);
    send(0x11, nullptr, 0);  // SLPOUT
    delay(150);
    send(0x29, nullptr, 0);  // DISPON
    delay(150);
  }

  Pins pins_ = {-1, -1, -1, -1, -1, -1};
  SPIClass* bus_ = nullptr;
  SPISettings controlSettings_;
  SPISettings settings_;
  alignas(4) uint16_t swap_[kMaxSpanPixels] = {};
  int16_t width_ = 320;
  int16_t height_ = 240;
  int16_t windowX_ = 0;
  int16_t windowY_ = 0;
  int16_t windowWidth_ = 0;
  int16_t windowHeight_ = 0;
  int16_t windowColumn_ = 0;
  int16_t windowRow_ = 0;
  uint8_t madctl_ = 0x28;
  uint8_t rotation_ = 1;
  bool softwareLandscape_ = false;
  bool windowOpen_ = false;
  bool running_ = false;
};

}  // namespace voicebot_face
