#include "draw_helpers.hpp"

#include <algorithm>
#include <cstddef>

namespace rummage::views {

float text_width(ui::Renderer& renderer, std::string_view text, float size) {
    return renderer.fonts().layout(text, size, renderer.scale()).width / renderer.scale();
}

void draw_centered(ui::Renderer& renderer, std::string_view text, const ui::Rect& bounds, float y,
                   float size, ui::Color color) {
    const float max = bounds.width - 24.0F;
    const float width = std::min(text_width(renderer, text, size), max);
    renderer.draw_text(text, bounds.x + ((bounds.width - width) / 2.0F), y, size, color,
                       std::max(1.0F, max), ui::Ellipsis::middle);
}

void draw_right_aligned(ui::Renderer& renderer, std::string_view text, float right_x, float top_y,
                        float font_size, ui::Color color) {
    renderer.draw_text(text, right_x - text_width(renderer, text, font_size), top_y, font_size,
                       color);
}

std::vector<ui::ColorRun> colour_runs(std::span<const text::Span> spans,
                                      const theme::SyntaxColors& colours, float alpha) {
    std::vector<ui::ColorRun> runs;
    runs.reserve(spans.size());
    for (const text::Span& span : spans) {
        runs.push_back(
            {.start = span.start,
             .end = span.start + span.length,
             .color = colours.at(static_cast<std::size_t>(span.style)).with_alpha(alpha)});
    }
    return runs;
}

}  // namespace rummage::views
