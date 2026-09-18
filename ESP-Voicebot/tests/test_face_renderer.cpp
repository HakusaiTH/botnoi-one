#include "../face_renderer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using voicebot_face::FaceFrame;
using voicebot_face::FaceRenderer;
using voicebot_face::Layout;
using voicebot_face::Palette;
using voicebot_face::Rect;

namespace {

// Renders one region into a dense buffer and reports how much of the shape it
// covers, using the alpha implied by each pixel against a white background.
struct Coverage {
  uint32_t weight = 0;   // Sum of per-pixel alpha, 255 per fully drawn pixel.
  uint32_t filled = 0;   // Fully drawn pixels only.
  int16_t firstRow = -1;
  int16_t lastRow = -1;
};

uint8_t alphaOf(uint16_t pixel, const Palette& palette) {
  // The only colours present are the background, the shape and blends of the
  // two, so the green channel alone recovers the coverage.
  const int background = (palette.background >> 5) & 0x3F;
  const int foreground = (pixel >> 5) & 0x3F;
  const int value = (background - foreground) * 255 / background;
  return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

Coverage measure(const FaceRenderer& renderer, uint8_t index, const FaceFrame& frame) {
  const Rect rect = renderer.region(index);
  std::vector<uint16_t> row(static_cast<size_t>(rect.w) + 8, 0xDEAD);
  Coverage result;
  for (int16_t line = 0; line < rect.h; ++line) {
    renderer.renderRow(index, frame, line, row.data());
    // Nothing may be written past the region width.
    for (size_t i = static_cast<size_t>(rect.w); i < row.size(); ++i) assert(row[i] == 0xDEAD);
    bool any = false;
    for (int16_t i = 0; i < rect.w; ++i) {
      const uint8_t alpha = alphaOf(row[i], renderer.palette());
      if (!alpha) continue;
      any = true;
      result.weight += alpha;
      if (alpha == 255) ++result.filled;
    }
    if (any) {
      if (result.firstRow < 0) result.firstRow = line;
      result.lastRow = line;
    }
  }
  return result;
}

}  // namespace

static void the_default_layout_fits_the_panel_without_overlapping_regions() {
  FaceRenderer renderer;
  assert(renderer.valid());
  const Layout& layout = renderer.layout();
  for (uint8_t index = 0; index < FaceRenderer::kRegionCount; ++index) {
    const Rect rect = renderer.region(index);
    assert(rect.x >= 0 && rect.y >= 0);
    assert(rect.x + rect.w <= layout.width);
    assert(rect.y + rect.h <= layout.height);
    assert(rect.w <= FaceRenderer::kMaxRegionWidth);
  }
  const Rect left = renderer.region(FaceRenderer::kLeftEye);
  const Rect right = renderer.region(FaceRenderer::kRightEye);
  const Rect mouth = renderer.region(FaceRenderer::kMouth);
  assert(left.x + left.w <= right.x);
  assert(left.y + left.h <= mouth.y);
  assert(renderer.eyeCenterX(FaceRenderer::kLeftEye) < renderer.eyeCenterX(FaceRenderer::kRightEye));
}

static void an_invalid_layout_is_rejected_instead_of_clipping() {
  Layout wide = Layout();
  wide.eyeRadiusX = 80;  // Eye windows would exceed the row buffer and collide.
  assert(!FaceRenderer(wide, Palette()).valid());
  Layout tall = Layout();
  tall.mouthMaxDepth = 120;  // The mouth window would run off the bottom.
  assert(!FaceRenderer(tall, Palette()).valid());
  Layout collide = Layout();
  collide.mouthCenterY = 130;  // The mouth window would land on the eyes.
  assert(!FaceRenderer(collide, Palette()).valid());
  Layout closed = Layout();
  closed.lidThickness = 200;  // A "closed" eye taller than an open one.
  assert(!FaceRenderer(closed, Palette()).valid());
  for (int bad = 0; bad < 7; ++bad) {
    Layout layout;
    if (bad == 0) layout.gazeLimitX = -1;
    if (bad == 1) layout.gazeLimitY = -1;
    if (bad == 2) layout.mouthNarrowing = -1;
    if (bad == 3) layout.lidThickness = 1;
    if (bad == 4) layout.mouthMinDepth = -1;
    if (bad == 5) layout.mouthRiseLimit = 0;
    if (bad == 6) layout.eyeRadiusY = INT16_MAX;
    FaceRenderer invalid(layout, Palette());
    assert(!invalid.valid());
    uint16_t untouched[FaceRenderer::kMaxRegionWidth];
    for (auto& pixel : untouched) pixel = 0xDEAD;
    invalid.renderRow(FaceRenderer::kLeftEye, FaceFrame{}, 0, untouched);
    for (auto pixel : untouched) assert(pixel == 0xDEAD);
  }
}

static void gaze_is_clamped_and_invalid_regions_do_not_alias_an_eye() {
  FaceRenderer renderer;
  FaceFrame extreme, bounded;
  extreme.gazeX = INT8_MIN; extreme.gazeY = INT8_MAX;
  bounded.gazeX = -renderer.layout().gazeLimitX;
  bounded.gazeY = renderer.layout().gazeLimitY;
  assert(!renderer.dirty(FaceRenderer::kLeftEye, extreme, bounded));
  const Rect rect = renderer.region(FaceRenderer::kLeftEye);
  uint16_t first[FaceRenderer::kMaxRegionWidth], second[FaceRenderer::kMaxRegionWidth];
  for (int16_t row = 0; row < rect.h; ++row) {
    renderer.renderRow(FaceRenderer::kLeftEye, extreme, row, first);
    renderer.renderRow(FaceRenderer::kLeftEye, bounded, row, second);
    for (int16_t column = 0; column < rect.w; ++column) assert(first[column] == second[column]);
  }
  for (auto& pixel : first) pixel = 0xDEAD;
  for (uint8_t index : {uint8_t(3), uint8_t(255)}) {
    assert(renderer.region(index).w == 0);
    renderer.renderRow(index, FaceFrame{}, 40, first);
    assert(!renderer.dirty(index, FaceFrame{}, FaceFrame{}));
  }
  for (auto pixel : first) assert(pixel == 0xDEAD);
}

static void large_valid_geometry_does_not_overflow_fixed_point_math() {
  Layout layout;
  layout.height = 12000; layout.eyeRadiusY = 3000;
  layout.eyeCenterY = 4000; layout.mouthCenterY = 10000;
  FaceRenderer renderer(layout, Palette());
  assert(renderer.valid());
  const Rect rect = renderer.region(FaceRenderer::kLeftEye);
  uint16_t pixels[FaceRenderer::kMaxRegionWidth + 1];
  for (auto& pixel : pixels) pixel = 0xDEAD;
  // dyQ8*4096 exceeds int32 here, although the geometry itself is valid.
  renderer.renderRow(FaceRenderer::kLeftEye, FaceFrame{}, 100, pixels);
  bool drewEye = false;
  for (int16_t i = 0; i < rect.w; ++i) drewEye = drewEye || pixels[i] != renderer.palette().background;
  assert(drewEye);
  for (size_t i = rect.w; i < sizeof(pixels) / sizeof(pixels[0]); ++i) assert(pixels[i] == 0xDEAD);
}

static void an_open_eye_is_solid_in_the_middle_and_clear_in_the_corners() {
  FaceRenderer renderer;
  const Rect rect = renderer.region(FaceRenderer::kLeftEye);
  FaceFrame frame;
  std::vector<uint16_t> row(static_cast<size_t>(rect.w));
  const int16_t centreRow = static_cast<int16_t>(renderer.layout().eyeCenterY - rect.y);
  renderer.renderRow(FaceRenderer::kLeftEye, frame, centreRow, row.data());
  const int16_t centreColumn =
      static_cast<int16_t>(renderer.eyeCenterX(FaceRenderer::kLeftEye) - rect.x);
  assert(row[static_cast<size_t>(centreColumn)] == renderer.palette().eye);
  assert(row[0] == renderer.palette().background);
  assert(row[static_cast<size_t>(rect.w - 1)] == renderer.palette().background);
  // Rows outside the region are background, never stale or out-of-bounds.
  renderer.renderRow(FaceRenderer::kLeftEye, frame, rect.h, row.data());
  for (int16_t i = 0; i < rect.w; ++i) assert(row[static_cast<size_t>(i)] == renderer.palette().background);
  renderer.renderRow(FaceRenderer::kLeftEye, frame, -1, row.data());
  for (int16_t i = 0; i < rect.w; ++i) assert(row[static_cast<size_t>(i)] == renderer.palette().background);
}

static void eye_openness_scales_the_height_but_keeps_the_width() {
  FaceRenderer renderer;
  FaceFrame open;
  FaceFrame closed;
  closed.leftEyeOpen = 0;
  closed.rightEyeOpen = 0;
  const Coverage wide = measure(renderer, FaceRenderer::kLeftEye, open);
  const Coverage lid = measure(renderer, FaceRenderer::kLeftEye, closed);
  const int16_t openHeight = static_cast<int16_t>(wide.lastRow - wide.firstRow + 1);
  const int16_t lidHeight = static_cast<int16_t>(lid.lastRow - lid.firstRow + 1);
  assert(openHeight > 2 * renderer.layout().eyeRadiusY - 2);
  assert(lidHeight <= renderer.layout().lidThickness + 1);
  assert(lidHeight >= 2);  // A closed eye is a visible lid line, not nothing.
  assert(lid.weight > 0 && lid.weight * 3 < wide.weight);
  // Antialiasing must not leak: a filled ellipse is mostly solid pixels.
  assert(wide.filled * 100 > wide.weight / 255 * 85);
  // Openness is monotonic, so a blink never appears to jump back open.
  uint32_t previous = 0;
  for (int level = 0; level <= 255; level += 15) {
    FaceFrame frame;
    frame.leftEyeOpen = static_cast<uint8_t>(level);
    const Coverage step = measure(renderer, FaceRenderer::kLeftEye, frame);
    assert(step.weight >= previous);
    previous = step.weight;
  }
}

static void gaze_moves_the_eye_and_stays_inside_its_window() {
  FaceRenderer renderer;
  const Rect rect = renderer.region(FaceRenderer::kLeftEye);
  const Layout& layout = renderer.layout();
  for (int x = -layout.gazeLimitX; x <= layout.gazeLimitX; ++x) {
    for (int y = -layout.gazeLimitY; y <= layout.gazeLimitY; y += layout.gazeLimitY) {
      FaceFrame frame;
      frame.gazeX = static_cast<int8_t>(x);
      frame.gazeY = static_cast<int8_t>(y);
      const Coverage moved = measure(renderer, FaceRenderer::kLeftEye, frame);
      // Clipping at an edge would shrink the eye; the window must be big enough.
      assert(moved.firstRow > 0 && moved.lastRow < rect.h - 1);
      std::vector<uint16_t> row(static_cast<size_t>(rect.w));
      for (int16_t line = 0; line < rect.h; ++line) {
        renderer.renderRow(FaceRenderer::kLeftEye, frame, line, row.data());
        assert(row[0] == renderer.palette().background);
        assert(row[static_cast<size_t>(rect.w - 1)] == renderer.palette().background);
      }
    }
  }
}

static void the_mouth_opens_downward_and_the_frown_bows_upward() {
  FaceRenderer renderer;
  const Rect rect = renderer.region(FaceRenderer::kMouth);
  const int16_t centreRow = static_cast<int16_t>(renderer.layout().mouthCenterY - rect.y);
  FaceFrame shut;
  FaceFrame wide;
  wide.mouthOpen = 255;
  const Coverage closed = measure(renderer, FaceRenderer::kMouth, shut);
  const Coverage open = measure(renderer, FaceRenderer::kMouth, wide);
  assert(closed.weight > 0);                 // A closed smile still shows lips.
  assert(open.weight > 2 * closed.weight);
  assert(open.lastRow > closed.lastRow);     // It opens towards the chin.
  assert(closed.firstRow >= centreRow - renderer.layout().mouthRiseLimit);
  // Opening is monotonic so a syllable cannot look like a flicker.
  uint32_t previous = 0;
  for (int level = 0; level <= 255; level += 15) {
    FaceFrame frame;
    frame.mouthOpen = static_cast<uint8_t>(level);
    const Coverage step = measure(renderer, FaceRenderer::kMouth, frame);
    assert(step.weight >= previous);
    previous = step.weight;
  }
  FaceFrame frown;
  frown.mouthCurve = -80;
  frown.mouthOpen = 255;
  const Coverage sad = measure(renderer, FaceRenderer::kMouth, frown);
  assert(sad.firstRow < centreRow && sad.lastRow <= centreRow + 8);
  // The frown is bounded so it can never reach into the eye windows.
  assert(sad.firstRow >= centreRow - renderer.layout().mouthRiseLimit - 1);
  assert(measure(renderer, FaceRenderer::kMouth, frown).weight <= open.weight);
}

static void dirty_tracks_exactly_the_fields_each_region_draws() {
  FaceRenderer renderer;
  const FaceFrame base;
  FaceFrame moved = base;
  moved.gazeX = 3;
  assert(renderer.dirty(FaceRenderer::kLeftEye, base, moved));
  assert(renderer.dirty(FaceRenderer::kRightEye, base, moved));
  assert(!renderer.dirty(FaceRenderer::kMouth, base, moved));
  FaceFrame winked = base;
  winked.rightEyeOpen = 0;
  assert(!renderer.dirty(FaceRenderer::kLeftEye, base, winked));
  assert(renderer.dirty(FaceRenderer::kRightEye, base, winked));
  FaceFrame talking = base;
  talking.mouthOpen = 60;
  assert(renderer.dirty(FaceRenderer::kMouth, base, talking));
  assert(!renderer.dirty(FaceRenderer::kLeftEye, base, talking));
  FaceFrame curved = base;
  curved.mouthCurve = -40;
  assert(renderer.dirty(FaceRenderer::kMouth, base, curved));
  FaceFrame sameGeometry = base;
  sameGeometry.mouthCurve = 125; // Same positive lip quantization as104.
  assert(!renderer.dirty(FaceRenderer::kMouth, base, sameGeometry));
  for (uint8_t index = 0; index < FaceRenderer::kRegionCount; ++index) {
    assert(!renderer.dirty(index, base, base));
  }
}

static void blending_and_the_integer_square_root_stay_exact_at_the_ends() {
  assert(voicebot_face::blend565(0xFFFF, 0x0000, 0) == 0xFFFF);
  assert(voicebot_face::blend565(0xFFFF, 0x0000, 255) == 0x0000);
  const uint16_t half = voicebot_face::blend565(0xFFFF, 0x0000, 128);
  assert((half >> 11) > 12 && (half >> 11) < 19);
  assert(voicebot_face::integerSquareRoot(0) == 0);
  assert(voicebot_face::integerSquareRoot(1) == 1);
  assert(voicebot_face::integerSquareRoot(16777216) == 4096);
  for (uint32_t value = 0; value < 5000; ++value) {
    const uint32_t root = voicebot_face::integerSquareRoot(value);
    assert(root * root <= value && (root + 1) * (root + 1) > value);
  }
}

int main() {
  the_default_layout_fits_the_panel_without_overlapping_regions();
  an_invalid_layout_is_rejected_instead_of_clipping();
  gaze_is_clamped_and_invalid_regions_do_not_alias_an_eye();
  large_valid_geometry_does_not_overflow_fixed_point_math();
  an_open_eye_is_solid_in_the_middle_and_clear_in_the_corners();
  eye_openness_scales_the_height_but_keeps_the_width();
  gaze_moves_the_eye_and_stays_inside_its_window();
  the_mouth_opens_downward_and_the_frown_bows_upward();
  dirty_tracks_exactly_the_fields_each_region_draws();
  blending_and_the_integer_square_root_stay_exact_at_the_ends();
  printf("test_face_renderer: all cases passed\n");
  return 0;
}
