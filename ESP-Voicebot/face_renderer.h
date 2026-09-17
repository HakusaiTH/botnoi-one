#pragma once

#include <stddef.h>
#include <stdint.h>

#include "face_state.h"

namespace voicebot_face {

struct Rect {
  int16_t x, y, w, h;
};

// Face geometry in panel pixels, sized for a 320x240 landscape ILI9341. Each
// region is pushed to the panel as an independent window, so two regions must
// never overlap; FaceRenderer::valid() rejects a layout where they would.
struct Layout {
  int16_t width = 320;
  int16_t height = 240;
  int16_t eyeCenterY = 100;
  int16_t eyeSpacing = 108;   // Distance between the two eye centres.
  int16_t eyeRadiusX = 33;
  int16_t eyeRadiusY = 36;    // Vertical radius of a fully open eye.
  int16_t lidThickness = 9;   // Total height of a fully closed eye.
  int16_t gazeLimitX = 14;    // Bounds the eye windows, so gaze cannot clip.
  int16_t gazeLimitY = 8;
  int16_t mouthCenterY = 170;
  int16_t mouthHalfWidth = 46;
  int16_t mouthNarrowing = 6;  // A wide-open mouth rounds towards an "O".
  int16_t mouthMinDepth = 14;  // Downward reach of a closed smile.
  int16_t mouthMaxDepth = 46;  // Downward reach at mouthOpen 255.
  int16_t mouthRiseLimit = 18; // Upward reach; also bounds the frown.
};

struct Palette {
  uint16_t background = 0xFFFF;  // RGB565. A white screen, as on the product.
  uint16_t eye = 0x0000;
  uint16_t mouth = 0xFB74;       // Warm pink.
};

// Source-over blend of RGB565. Only antialiased shape edges use it.
inline uint16_t blend565(uint16_t back, uint16_t front, uint8_t alpha) {
  if (!alpha) return back;
  if (alpha == 255) return front;
  const uint32_t inverse = 255u - alpha;
  const uint32_t red = ((back >> 11) * inverse + (front >> 11) * alpha + 127u) / 255u;
  const uint32_t green =
      (((back >> 5) & 0x3Fu) * inverse + ((front >> 5) & 0x3Fu) * alpha + 127u) / 255u;
  const uint32_t blue = ((back & 0x1Fu) * inverse + (front & 0x1Fu) * alpha + 127u) / 255u;
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

inline uint32_t integerSquareRoot(uint32_t value) {
  uint32_t result = 0;
  uint32_t bit = 1u << 30;
  while (bit > value) bit >>= 2;
  while (bit) {
    if (value >= result + bit) {
      value -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

// Rasterizes one FaceFrame one row at a time. Nothing is allocated and no
// framebuffer is held: the caller owns a single row buffer, which is why the
// whole face fits in a few hundred bytes of the render task's stack.
class FaceRenderer {
 public:
  // Widest window the caller's row buffer must accommodate.
  static const int16_t kMaxRegionWidth = 128;
  enum Region : uint8_t { kLeftEye = 0, kRightEye = 1, kMouth = 2, kRegionCount = 3 };

  FaceRenderer() {}
  FaceRenderer(const Layout& layout, const Palette& palette)
      : layout_(layout), palette_(palette) {}

  const Layout& layout() const { return layout_; }
  const Palette& palette() const { return palette_; }

  bool valid() const {
    if (layout_.width <= 0 || layout_.height <= 0) return false;
    if (layout_.eyeRadiusX <= 0 || layout_.eyeRadiusY <= layout_.lidThickness / 2) return false;
    if (layout_.mouthHalfWidth <= layout_.mouthNarrowing) return false;
    if (layout_.mouthMaxDepth < layout_.mouthMinDepth) return false;
    for (uint8_t index = 0; index < kRegionCount; ++index) {
      const Rect rect = region(index);
      if (rect.w <= 0 || rect.h <= 0 || rect.w > kMaxRegionWidth) return false;
      if (rect.x < 0 || rect.y < 0) return false;
      if (rect.x + rect.w > layout_.width || rect.y + rect.h > layout_.height) return false;
    }
    return !overlaps(region(kLeftEye), region(kRightEye)) &&
           !overlaps(region(kLeftEye), region(kMouth)) &&
           !overlaps(region(kRightEye), region(kMouth));
  }

  Rect region(uint8_t index) const {
    Rect rect;
    if (index == kMouth) {
      rect.x = static_cast<int16_t>(layout_.width / 2 - layout_.mouthHalfWidth - 1);
      rect.w = static_cast<int16_t>(2 * layout_.mouthHalfWidth + 3);
      rect.y = static_cast<int16_t>(layout_.mouthCenterY - layout_.mouthRiseLimit - 1);
      rect.h = static_cast<int16_t>(layout_.mouthRiseLimit + layout_.mouthMaxDepth + 3);
    } else {
      const int16_t reachX = static_cast<int16_t>(layout_.gazeLimitX + layout_.eyeRadiusX);
      const int16_t reachY = static_cast<int16_t>(layout_.gazeLimitY + layout_.eyeRadiusY);
      rect.x = static_cast<int16_t>(eyeCenterX(index) - reachX - 1);
      rect.w = static_cast<int16_t>(2 * reachX + 3);
      rect.y = static_cast<int16_t>(layout_.eyeCenterY - reachY - 1);
      rect.h = static_cast<int16_t>(2 * reachY + 3);
    }
    return rect;
  }

  // True when this region's pixels differ between two frames. Skipping a clean
  // region is what keeps a talking mouth from redrawing both eyes every frame.
  bool dirty(uint8_t index, const FaceFrame& previous, const FaceFrame& next) const {
    if (index == kMouth) {
      return previous.mouthOpen != next.mouthOpen || previous.mouthCurve != next.mouthCurve;
    }
    const uint8_t before = index == kLeftEye ? previous.leftEyeOpen : previous.rightEyeOpen;
    const uint8_t after = index == kLeftEye ? next.leftEyeOpen : next.rightEyeOpen;
    return before != after || previous.gazeX != next.gazeX || previous.gazeY != next.gazeY;
  }

  // Writes region(index).w pixels. `row` is relative to the region's top.
  void renderRow(uint8_t index, const FaceFrame& frame, int16_t row, uint16_t* pixels) const {
    const Rect rect = region(index);
    if (!pixels || rect.w <= 0 || rect.w > kMaxRegionWidth) return;
    for (int16_t i = 0; i < rect.w; ++i) pixels[i] = palette_.background;
    if (row < 0 || row >= rect.h) return;

    uint16_t coverage[kMaxRegionWidth];
    for (int16_t i = 0; i < rect.w; ++i) coverage[i] = 0;
    const int32_t rowCentreQ8 = (static_cast<int32_t>(rect.y) + row) * 256;

    int32_t centreXQ8 = 0, centreYQ8 = 0, radiusXQ8 = 0, upQ8 = 0, downQ8 = 0;
    if (index == kMouth) {
      mouthShape(frame, centreXQ8, centreYQ8, radiusXQ8, upQ8, downQ8);
    } else {
      eyeShape(index, frame, centreXQ8, centreYQ8, radiusXQ8, upQ8);
      downQ8 = upQ8;
    }
    const int32_t originQ8 = static_cast<int32_t>(rect.x) * 256;

    // Three vertical samples per output row. Combined with the exact fractional
    // horizontal span this antialiases both the flat and the steep edges of the
    // ellipses, which is what stops the eyes looking like stair steps.
    static const int32_t kSubRow[3] = {-85, 0, 85};
    for (uint8_t sub = 0; sub < 3; ++sub) {
      const int32_t dy = rowCentreQ8 + kSubRow[sub] - centreYQ8;
      const int32_t radiusYQ8 = dy < 0 ? upQ8 : downQ8;
      const int32_t half = ellipseHalfWidthQ8(radiusXQ8, radiusYQ8, dy);
      if (half <= 0) continue;
      accumulate(coverage, rect.w, centreXQ8 - half - originQ8, centreXQ8 + half - originQ8);
    }
    const uint16_t colour = index == kMouth ? palette_.mouth : palette_.eye;
    for (int16_t i = 0; i < rect.w; ++i) {
      if (!coverage[i]) continue;
      // Three sub-rows of 256 units each: 768 * 85 / 256 is exactly 255.
      const uint32_t total = coverage[i] > 768 ? 768u : coverage[i];
      pixels[i] = blend565(palette_.background, colour, static_cast<uint8_t>((total * 85u) >> 8));
    }
  }

  int16_t eyeCenterX(uint8_t index) const {
    const int16_t half = static_cast<int16_t>(layout_.eyeSpacing / 2);
    return static_cast<int16_t>(index == kLeftEye ? layout_.width / 2 - half
                                                 : layout_.width / 2 + half);
  }

 private:
  static bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
  }

  // Half width of an ellipse at vertical distance dy, in Q8 pixels.
  static int32_t ellipseHalfWidthQ8(int32_t radiusXQ8, int32_t radiusYQ8, int32_t dyQ8) {
    if (radiusXQ8 <= 0 || radiusYQ8 <= 0) return 0;
    if (dyQ8 < 0) dyQ8 = -dyQ8;
    if (dyQ8 >= radiusYQ8) return 0;
    const int32_t ratio = static_cast<int32_t>(dyQ8 * 4096 / radiusYQ8);        // Q12
    const int32_t remaining = 4096 - ((ratio * ratio) >> 12);                   // Q12
    if (remaining <= 0) return 0;
    const int32_t root =
        static_cast<int32_t>(integerSquareRoot(static_cast<uint32_t>(remaining) << 12));  // Q12
    return (radiusXQ8 * root) >> 12;
  }

  // Adds the exact horizontal coverage of one span, in units of 1/256 pixel.
  static void accumulate(uint16_t* coverage, int16_t width, int32_t leftQ8, int32_t rightQ8) {
    if (rightQ8 <= leftQ8) return;
    int32_t first = (leftQ8 - 128) >> 8;
    int32_t last = ((rightQ8 + 128) >> 8) + 1;
    if (first < 0) first = 0;
    if (last > width) last = width;
    for (int32_t pixel = first; pixel < last; ++pixel) {
      const int32_t centre = pixel * 256;
      const int32_t low = leftQ8 > centre - 128 ? leftQ8 : centre - 128;
      const int32_t high = rightQ8 < centre + 128 ? rightQ8 : centre + 128;
      if (high > low) coverage[pixel] = static_cast<uint16_t>(coverage[pixel] + (high - low));
    }
  }

  void eyeShape(uint8_t index, const FaceFrame& frame, int32_t& centreXQ8, int32_t& centreYQ8,
                int32_t& radiusXQ8, int32_t& radiusYQ8) const {
    const uint8_t open = index == kLeftEye ? frame.leftEyeOpen : frame.rightEyeOpen;
    centreXQ8 = (static_cast<int32_t>(eyeCenterX(index)) + frame.gazeX) * 256;
    centreYQ8 = (static_cast<int32_t>(layout_.eyeCenterY) + frame.gazeY) * 256;
    radiusXQ8 = static_cast<int32_t>(layout_.eyeRadiusX) * 256;
    // A closed eye keeps its full width, so it reads as a lowered lid rather
    // than as a disappearing eye.
    const int32_t closed = static_cast<int32_t>(layout_.lidThickness / 2) * 256;
    const int32_t travel = static_cast<int32_t>(layout_.eyeRadiusY) * 256 - closed;
    radiusYQ8 = closed + static_cast<int32_t>(open) * travel / 255;
  }

  void mouthShape(const FaceFrame& frame, int32_t& centreXQ8, int32_t& centreYQ8,
                  int32_t& radiusXQ8, int32_t& upQ8, int32_t& downQ8) const {
    centreXQ8 = static_cast<int32_t>(layout_.width / 2) * 256;
    centreYQ8 = static_cast<int32_t>(layout_.mouthCenterY) * 256;
    const int32_t narrowing =
        static_cast<int32_t>(frame.mouthOpen) * layout_.mouthNarrowing / 255;
    radiusXQ8 = (static_cast<int32_t>(layout_.mouthHalfWidth) - narrowing) * 256;
    const int32_t range = layout_.mouthMaxDepth - layout_.mouthMinDepth;
    int32_t bow = layout_.mouthMinDepth + static_cast<int32_t>(frame.mouthOpen) * range / 255;
    const int32_t curve = frame.mouthCurve >= 0 ? frame.mouthCurve : -static_cast<int32_t>(frame.mouthCurve);
    // A thin far lip keeps the closed mouth asymmetric: a flat top over a
    // rounded bottom reads as a smile, and the inverse reads as a frown.
    const int32_t lip = 2 + curve / 64;
    if (frame.mouthCurve >= 0) {
      downQ8 = bow * 256;
      upQ8 = lip * 256;
    } else {
      // A frown bows upward instead, bounded so it cannot reach the eyes.
      if (bow > layout_.mouthRiseLimit) bow = layout_.mouthRiseLimit;
      upQ8 = bow * 256;
      downQ8 = lip * 256;
    }
  }

  Layout layout_;
  Palette palette_;
};

}  // namespace voicebot_face
