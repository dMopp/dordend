#pragma once

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"

namespace rummage::app {

// Visual check of every renderer primitive (ROADMAP 1.4): solid and fractional-position rects,
// radii, borders, shadows, blending, nested clipping, physical-pixel hairlines and text
// (sizes, accents, emoji, CJK, invalid UTF-8, truncation), in the theme's colors. It is the
// window content until the file views exist.
void draw_test_scene(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

}  // namespace rummage::app
