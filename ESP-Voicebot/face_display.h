#pragma once

#include <SPI.h>
#include <stdint.h>

#include "display_ili9341.h"
#include "face_renderer.h"
#include "face_state.h"

namespace voicebot_face {

// Owns the panel, the rasterizer and the single row buffer, and pushes only the
// regions whose pixels actually changed. Driven by one low-priority task; no
// other task may touch the panel or its SPI bus.
class FaceDisplay {
 public:
  bool begin(const Ili9341::Pins& pins, uint8_t rotation, uint32_t frequency, SPIClass& bus,
             const Layout& layout = Layout(), const Palette& palette = Palette()) {
    renderer_ = FaceRenderer(layout, palette);
    if (!renderer_.valid()) return false;
    if (!panel_.begin(pins, rotation, frequency, bus)) return false;
    if (panel_.width() < layout.width || panel_.height() < layout.height) {
      panel_.end();
      return false;
    }
    // Centre a layout smaller than the panel instead of anchoring top-left.
    offsetX_ = static_cast<int16_t>((panel_.width() - layout.width) / 2);
    offsetY_ = static_cast<int16_t>((panel_.height() - layout.height) / 2);
    panel_.fill(0, 0, panel_.width(), panel_.height(), palette.background);
    first_ = true;
    return true;
  }

  void end() {
    panel_.end();
  }

  bool running() const { return panel_.running(); }

  void update(const FaceFrame& frame) {
    if (!panel_.running()) return;
    for (uint8_t index = 0; index < FaceRenderer::kRegionCount; ++index) {
      if (!first_ && !renderer_.dirty(index, shown_, frame)) continue;
      const Rect rect = renderer_.region(index);
      panel_.beginWindow(static_cast<int16_t>(rect.x + offsetX_),
                         static_cast<int16_t>(rect.y + offsetY_), rect.w, rect.h);
      for (int16_t row = 0; row < rect.h; ++row) {
        renderer_.renderRow(index, frame, row, row_);
        panel_.writeRow(row_, static_cast<size_t>(rect.w));
      }
      panel_.endWindow();
    }
    if (first_) {
      // The backlight comes up only once a complete face is on the panel, so
      // boot never shows the controller's uninitialized RAM.
      panel_.backlight(true);
      first_ = false;
    }
    shown_ = frame;
  }

  const FaceRenderer& renderer() const { return renderer_; }

 private:
  Ili9341 panel_;
  FaceRenderer renderer_;
  FaceFrame shown_;
  uint16_t row_[FaceRenderer::kMaxRegionWidth];
  int16_t offsetX_ = 0;
  int16_t offsetY_ = 0;
  bool first_ = true;
};

}  // namespace voicebot_face
