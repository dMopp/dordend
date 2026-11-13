#include "rummage/theme/syntax.hpp"

#include <cmath>
#include <numbers>

#include "rummage/theme/desktop.hpp"
#include "oklch.hpp"

namespace rummage::theme {
namespace {

// WCAG AA for normal text.
constexpr float kReadable = 4.5F;

// A green for added lines, about 143 degrees in OKLCH, as diff viewers use.
constexpr double kGreenHue = 2.5;
constexpr double kGreenChroma = 0.14;

// Halfway between two colours in OKLCH: lightness and chroma averaged, the hue along the shorter
// arc.
Lch between(Lch a, Lch b) {
    double turn = b.h - a.h;
    if (turn > std::numbers::pi) {
        turn -= 2.0 * std::numbers::pi;
    } else if (turn < -std::numbers::pi) {
        turn += 2.0 * std::numbers::pi;
    }
    return {(a.l + b.l) / 2.0, (a.c + b.c) / 2.0, a.h + (turn / 2.0)};
}

// colour, or the nearest one in lightness (hue and chroma kept as far as sRGB allows) that reads
// on background; away from the background: lighter on a dark one, darker on a light one.
Color readable(Lch colour, Color background) {
    Color shown = from_lch(colour);
    const double step = to_lch(background).l < 0.6 ? 0.01 : -0.01;
    for (int i = 0; i < 100 && contrast(shown, background) < kReadable; ++i) {
        colour.l += step;
        shown = from_lch(colour);
    }
    return shown;
}

}  // namespace

SyntaxColors syntax_colors(const Palette& palette) {
    const Color page = palette.surface_container.with_alpha(1.0F);
    const Lch primary = to_lch(palette.primary);
    const Lch secondary = to_lch(palette.secondary);
    const Lch tertiary = to_lch(palette.tertiary);
    const Lch error = to_lch(palette.error);
    return {
        readable(primary, page),                                              // keyword
        readable(tertiary, page),                                             // type
        readable(secondary, page),                                            // string
        readable(between(primary, error), page),                              // number
        readable(to_lch(palette.on_surface_variant), page),                   // comment
        readable(between(secondary, tertiary), page),                         // function
        readable({.l = primary.l, .c = kGreenChroma, .h = kGreenHue}, page),  // added
        readable(error, page),                                                // removed
    };
}

}  // namespace rummage::theme
