#pragma once

// Small text and chrome helpers shared by the views.

#include <span>
#include <string_view>
#include <vector>

#include "rummage/text/text.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/syntax.hpp"
#include "rummage/ui/geometry.hpp"
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

}  // namespace rummage::views
