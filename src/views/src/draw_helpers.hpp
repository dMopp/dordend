#pragma once

// Small text and chrome helpers shared by the views.

#include <span>
#include <string_view>
#include <vector>

#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/syntax.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"

namespace rummage::views {

// Height of the bar above the views (the breadcrumb).
inline constexpr float kTitleHeight = 40.0F;

// Width of text in logical pixels.
float text_width(ui::Renderer& renderer, std::string_view text, float size);

// Text centered in bounds (with 12 px margins, ellipsized when longer), its top at y.
void draw_centered(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds, float y,
                   float size, ui::Color color);

void draw_right_aligned(ui::Renderer& renderer, std::string_view text, float right_x, float top_y,
                        float font_size, ui::Color color);

// A line's syntax colours as runs for Renderer::draw_text_runs, at opacity alpha.
std::vector<ui::ColorRun> colour_runs(std::span<const text::Span> spans,
                                      const theme::SyntaxColors& colours, float alpha);

// A mark on the bottom right of a file's icon drawn in the size x size box at (x, y), saying why
// its preview failed: a lock (password protected) or a cross (damaged). Nothing for other
// failures, which say nothing about the file.
void draw_failure_badge(ui::Renderer& renderer, ui::IconSet& icons, const theme::Palette& palette,
                        picture::Failure failure, float x, float y, float size, float alpha = 1.0F);

}  // namespace rummage::views
