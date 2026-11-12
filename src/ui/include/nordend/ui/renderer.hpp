#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "nordend/ui/color.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/text.hpp"
#include "nordend/ui/texture.hpp"

namespace nordend::ui {

// A colour for the bytes [start, end) of a line of text (syntax colours).
struct ColorRun {
    std::uint32_t start{0};
    std::uint32_t end{0};
    Color color{};
};

struct BoxStyle {
    Color fill{};
    Corners radius{};
    float border_width{0.0F};  // drawn inside the box
    Color border{};
};

// Batched 2D renderer for one GL context. Coordinates are logical pixels; the renderer maps
// them to physical pixels with the frame's scale and snaps box edges to the pixel grid so
// they stay sharp at fractional scales.
//
// Usage per frame: begin_frame, any number of draw/clip calls, end_frame. Draw order is
// paint order, also between boxes and text. Glyphs are rasterized on first use into two
// 2048x2048 atlases (coverage and color) and reused across frames. Requires the GL context to be
// current for every call, including construction and destruction. Throws std::runtime_error if the
// shaders fail to build.
class Renderer {
public:
    explicit Renderer(FontSystem& fonts);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    void begin_frame(std::int32_t buffer_width, std::int32_t buffer_height, float scale,
                     Color clear, float background_opacity = 1.0F);
    void end_frame();

    [[nodiscard]] float scale() const noexcept { return scale_; }

    [[nodiscard]] FontSystem& fonts() noexcept { return fonts_; }

    // GPU bytes of the two glyph atlases (the memory report).
    [[nodiscard]] static std::size_t atlas_bytes() noexcept;

    void draw_box(const Rect& rect, const BoxStyle& style);

    void fill_rect(const Rect& rect, Color color) { draw_box(rect, {.fill = color}); }

    // Replaces the base background inside rect, respecting clips and background opacity.
    // Draw before foreground content: overlapping panels must not accumulate alpha.
    void fill_background(const Rect& rect, Color color);

    // Soft shadow of a rounded box; blur is the Gaussian sigma in logical pixels. Offset the
    // rect to move the shadow.
    void draw_shadow(const Rect& rect, const Corners& radius, float blur, Color color);

    // texture stretched over rect, cut to its rounded corners, at opacity (0-1).
    void draw_image(const Texture& texture, const Rect& rect, const Corners& radius = {},
                    float opacity = 1.0F);

    // One line of text; (x, y) is the top-left of the line box, size the font size, both in
    // logical pixels. With max_width > 0 the text is truncated with an ellipsis. Returns the
    // drawn width in logical pixels.
    float draw_text(std::string_view utf8, float x, float y, float size, Color color,
                    float max_width = 0.0F, Ellipsis ellipsis = Ellipsis::end,
                    Typeface typeface = Typeface::proportional);

    // Like draw_text, one shaping for the whole line, each glyph in the colour of the run holding
    // the byte it comes from (runs in order, apart), the others in color.
    float draw_text_runs(std::string_view utf8, float x, float y, float size, Color color,
                         std::span<const ColorRun> runs, float max_width = 0.0F,
                         Ellipsis ellipsis = Ellipsis::end,
                         Typeface typeface = Typeface::proportional);

    // Draws text shaped by FontSystem::layout at the current scale; (x, y) as in draw_text; with
    // runs, coloured as in draw_text_runs.
    void draw_layout(const TextLayout& layout, float x, float y, Color color,
                     std::span<const ColorRun> runs = {});

    // Restricts drawing to rect, intersected with the current clip, until pop_clip.
    void push_clip(const Rect& rect);
    void pop_clip();

private:
    struct Gl;
    struct BoxInstance;
    struct GlyphInstance;
    struct GlyphEntry;

    GlyphEntry rasterize(const ShapedGlyph& glyph, float pixel_size, float x_offset);
    void push_box(const BoxInstance& instance);
    void push_glyph(const GlyphInstance& instance);
    void flush();
    void apply_clip();

    std::unique_ptr<Gl> gl_;
    FontSystem& fonts_;
    std::vector<BoxInstance> boxes_;  // pending, at most one of the two lists is non-empty
    std::vector<GlyphInstance> glyphs_;
    std::vector<Rect> clips_;  // logical; empty = whole buffer
    std::int32_t buffer_width_{0};
    std::int32_t buffer_height_{0};
    float scale_{1.0F};
    float background_opacity_{1.0F};
};

}  // namespace nordend::ui
