#include "test_scene.hpp"

#include <array>
#include <cstddef>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/color.hpp"
#include "rummage/ui/text.hpp"

namespace rummage::app {
namespace {

using ui::BoxStyle;
using ui::Color;
using ui::Corners;
using ui::Rect;
using ui::Renderer;

// The scene's colors, from the theme's roles; the shadow demo keeps a light panel of its own.
struct Ink {
    Color red;
    Color peach;
    Color yellow;
    Color green;
    Color blue;
    Color mauve;
    Color text;
    Color surface0;
    Color surface1;

    explicit Ink(const theme::Palette& palette)
        : red{palette.error},
          peach{palette.primary_container},
          yellow{palette.tertiary},
          green{palette.secondary},
          blue{palette.primary},
          mauve{palette.tertiary_container},
          text{palette.on_surface},
          surface0{palette.surface_container},
          surface1{palette.surface_container_high} {}
};

constexpr Color kLightPanel = Color::rgb(0xDCE0E8);
constexpr Color kWhite = Color::rgb(0xFFFFFF);

constexpr float kMargin = 32.0F;
constexpr float kSwatch = 56.0F;
constexpr float kGap = 12.0F;
constexpr float kRowStep = kSwatch + 24.0F;
constexpr float kRightColumn = 560.0F;

float column_x(float origin, std::size_t index) {
    return origin + (static_cast<float>(index) * (kSwatch + kGap));
}

void draw_solid_rects(Renderer& r, const Ink& ink, float y) {
    const std::array swatches{ink.red, ink.peach, ink.yellow, ink.green, ink.blue};
    for (std::size_t i = 0; i < swatches.size(); ++i) {
        r.fill_rect({column_x(kMargin, i), y, kSwatch, kSwatch}, swatches.at(i));
    }
    // Fractional positions and sizes must still land on whole physical pixels: sharp edges.
    const float x = column_x(kMargin, swatches.size());
    r.fill_rect({x + 0.3F, y + 0.4F, 20.6F, kSwatch}, ink.mauve);
    r.fill_rect({x + 28.5F, y + 0.5F, 20.5F, kSwatch - 0.5F}, ink.mauve);
}

void draw_radii(Renderer& r, const Ink& ink, float y) {
    constexpr std::array radii{0.0F, 4.0F, 8.0F, 16.0F, kSwatch / 2.0F};
    for (std::size_t i = 0; i < radii.size(); ++i) {
        r.draw_box({column_x(kMargin, i), y, kSwatch, kSwatch},
                   {.fill = ink.blue, .radius = Corners::all(radii.at(i))});
    }
    // Per-corner radii, and a pill wider than tall.
    r.draw_box({column_x(kMargin, radii.size()), y, kSwatch, kSwatch},
               {.fill = ink.green,
                .radius = {.top_left = 0, .top_right = 24, .bottom_right = 0, .bottom_left = 24}});
    r.draw_box({column_x(kMargin, radii.size() + 1), y + 14.0F, 96.0F, 28.0F},
               {.fill = ink.peach, .radius = Corners::all(14.0F)});
}

void draw_borders(Renderer& r, const Ink& ink, float y) {
    const std::array styles{
        BoxStyle{.fill = ink.surface1, .border_width = 1.0F, .border = ink.text},
        BoxStyle{.fill = ink.surface1,
                 .radius = Corners::all(8),
                 .border_width = 1.0F,
                 .border = ink.text},
        BoxStyle{.fill = ink.surface1,
                 .radius = Corners::all(12),
                 .border_width = 2.0F,
                 .border = ink.mauve},
        BoxStyle{.fill = ink.surface1,
                 .radius = Corners::all(kSwatch / 2.0F),
                 .border_width = 4.0F,
                 .border = ink.yellow},
        // Border only: transparent fill.
        BoxStyle{.fill = ink.surface1.with_alpha(0.0F),
                 .radius = Corners::all(8),
                 .border_width = 1.0F,
                 .border = ink.green},
    };
    for (std::size_t i = 0; i < styles.size(); ++i) {
        r.draw_box({column_x(kMargin, i), y, kSwatch, kSwatch}, styles.at(i));
    }
}

void draw_shadows(Renderer& r, float y) {
    const Rect panel{kMargin, y, 480.0F, 200.0F};
    r.draw_box(panel, {.fill = kLightPanel, .radius = Corners::all(16)});
    constexpr std::array blurs{4.0F, 12.0F, 24.0F};
    constexpr float card_width = 128.0F;
    constexpr float card_height = 96.0F;
    const auto card_at = [&](std::size_t i) {
        return Rect{panel.x + 24.0F + (static_cast<float>(i) * (card_width + 24.0F)),
                    panel.y + 52.0F, card_width, card_height};
    };
    // All shadows first: a wide shadow must not paint over a neighbouring card.
    for (std::size_t i = 0; i < blurs.size(); ++i) {
        const float blur = blurs.at(i);
        const Rect card = card_at(i);
        r.draw_shadow({card.x, card.y + (blur / 3.0F), card.width, card.height}, Corners::all(12),
                      blur, Color::rgba(0x00000066));
    }
    for (std::size_t i = 0; i < blurs.size(); ++i) {
        r.draw_box(card_at(i), {.fill = kWhite, .radius = Corners::all(12)});
    }
}

void draw_blending(Renderer& r, const Ink& ink, float y) {
    constexpr float size = 96.0F;
    const std::array circles{
        Rect{kRightColumn, y, size, size},
        Rect{kRightColumn + 56.0F, y, size, size},
        Rect{kRightColumn + 28.0F, y + 48.0F, size, size},
    };
    const std::array colors{ink.red, ink.green, ink.blue};
    for (std::size_t i = 0; i < circles.size(); ++i) {
        r.draw_box(circles.at(i),
                   {.fill = colors.at(i).with_alpha(0.5F), .radius = Corners::all(size / 2.0F)});
    }
}

void draw_clipping(Renderer& r, const Ink& ink, float y) {
    const Rect outer{kRightColumn, y, 320.0F, 120.0F};
    // Outline just outside the clip, to see where it is.
    r.draw_box(outer.inset(-1.0F),
               {.fill = ink.surface0, .border_width = 1.0F, .border = ink.text});

    r.push_clip(outer);
    // Larger than the clip on every side: only the clipped part may appear.
    r.draw_box({outer.x - 40.0F, outer.y + 20.0F, 200.0F, 160.0F},
               {.fill = ink.mauve, .radius = Corners::all(24)});
    const Rect inner{outer.x + 180.0F, outer.y + 20.0F, 120.0F, 80.0F};
    r.draw_box(inner, {.fill = ink.surface1});
    r.push_clip(inner);  // nested: intersects with outer
    r.draw_box({inner.x + 60.0F, inner.y - 30.0F, 120.0F, 120.0F},
               {.fill = ink.yellow, .radius = Corners::all(60)});
    r.pop_clip();
    r.pop_clip();
}

void draw_hairlines(Renderer& r, const Ink& ink, float y) {
    // Lines exactly one physical pixel wide, one pixel apart: crisp at any scale.
    const float pixel = 1.0F / r.scale();
    const Rect band{kRightColumn, y, 320.0F, 60.0F};
    r.fill_rect(band, ink.surface0);
    const auto lines = static_cast<int>(band.width * r.scale() / 2.0F);
    for (int i = 0; i < lines; ++i) {
        r.fill_rect({band.x + (static_cast<float>(i) * 2.0F * pixel), band.y, pixel, band.height},
                    ink.text);
    }
    // One logical pixel border: one physical pixel at scale 1, more at higher scales.
    r.draw_box({kRightColumn, y + 76.0F, 320.0F, 40.0F},
               {.fill = ink.surface0, .border_width = 1.0F, .border = ink.text});
}

void draw_text_samples(Renderer& r, const Ink& ink, float y, float width) {
    const Rect panel{kMargin, y, width, 300.0F};
    r.draw_box(panel, {.fill = ink.surface0, .radius = Corners::all(12)});
    const float x = panel.x + 16.0F;
    float line = panel.y + 12.0F;

    struct Sample {
        float size;
        const char* text;
    };

    const std::array samples{
        Sample{11.0F, "11 px \u2014 The quick brown fox jumps over the lazy dog 0123456789"},
        Sample{14.0F, "14 px \u2014 rummage: file manager nativo per Wayland"},
        Sample{20.0F,
               "Accenti: \u00e0\u00e8\u00e9\u00ec\u00f2\u00f9 \u00c0\u00c8\u00c9 "
               "\u00f1 \u00e7 \u00df \u00f8 \u00e5 \u0153 \u2014 decomposta: e\u0301"},
        Sample{32.0F, "32 px rummage Ag"},
        Sample{14.0F,
               "Emoji: Foto \U0001F4F7 vacanza \U0001F3D4\uFE0F famiglia "
               "\U0001F468\u200D\U0001F469\u200D\U0001F467 ok \U0001F44D\U0001F3FD"},
        Sample{14.0F,
               "CJK: \u5199\u771F\u30D5\u30A9\u30EB\u30C0 \uC0AC\uC9C4 "
               "\u4E2D\u6587\u76EE\u5F55"},
        Sample{14.0F, "UTF-8 non valido: bad\xFF\xFE-name.txt"},
    };
    for (const Sample& sample : samples) {
        const ui::TextLayout& layout = r.fonts().layout(sample.text, sample.size, r.scale());
        r.draw_layout(layout, x, line, ink.text);
        line += ((layout.ascent + layout.descent) / r.scale()) + 6.0F;
    }

    // Truncation inside a 300 px box: at the end, and in the middle (file names).
    constexpr const char* name = "IMG_20241015_vacanza_dolomiti_tramonto_sul_lago_finale_v2.ORF";
    constexpr float box_width = 300.0F;
    const std::array modes{ui::Ellipsis::end, ui::Ellipsis::middle};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        const Rect box{x + (static_cast<float>(i) * (box_width + 16.0F)), line + 4.0F, box_width,
                       24.0F};
        r.draw_box(box.inset(-1.0F),
                   {.fill = ink.surface1, .border_width = 1.0F, .border = ink.blue});
        r.draw_text(name, box.x, box.y + 3.0F, 14.0F, ink.text, box.width, modes.at(i));
    }
}

}  // namespace

void draw_test_scene(Renderer& renderer, const Rect& bounds, const theme::Palette& palette) {
    const Ink ink{palette};
    renderer.push_clip(bounds);

    float y = kMargin;
    draw_solid_rects(renderer, ink, y);
    y += kRowStep;
    draw_radii(renderer, ink, y);
    y += kRowStep;
    draw_borders(renderer, ink, y);
    y += kRowStep;
    draw_shadows(renderer, y);

    draw_blending(renderer, ink, kMargin);
    draw_clipping(renderer, ink, kMargin + 180.0F);
    draw_hairlines(renderer, ink, kMargin + 340.0F);
    draw_text_samples(renderer, ink, 500.0F, 896.0F);

    renderer.pop_clip();
}

}  // namespace rummage::app
