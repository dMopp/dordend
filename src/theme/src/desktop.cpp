#include "rummage/theme/desktop.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "oklch.hpp"

namespace rummage::theme {
namespace {

double luminance(Color color) {
    return (0.2126 * to_linear(color.r)) + (0.7152 * to_linear(color.g)) +
           (0.0722 * to_linear(color.b));
}

// Surfaces carry this much of the accent's chroma at most: grey, warmed or cooled a little.
constexpr double kTint = 0.005;
// Text and icons on their background: WCAG AA for normal text.
constexpr double kReadable = 4.5;

// Lightness of each surface and of the neutral roles, dark then light.
struct Tones {
    double surface;
    double lowest;
    double dim;
    double low;
    double container;
    double high;
    double highest;
    double bright;
    double on_surface;
    double on_surface_variant;
    double outline;
    double outline_variant;
    double primary_min;  // the accent's lightness is kept inside [primary_min, primary_max]
    double primary_max;
    double container_tone;  // primary_container (the selection), secondary_container
    double secondary_container_tone;
    double error;
    double error_container;
};

// Dark: around GNOME's #222226 window and #2e2e32 sidebar. Light: near white.
constexpr Tones kDark{.surface = 0.235,
                      .lowest = 0.215,
                      .dim = 0.200,
                      .low = 0.255,
                      .container = 0.275,
                      .high = 0.305,
                      .highest = 0.335,
                      .bright = 0.370,
                      .on_surface = 0.940,
                      .on_surface_variant = 0.760,
                      .outline = 0.560,
                      .outline_variant = 0.370,
                      .primary_min = 0.720,
                      .primary_max = 0.900,
                      .container_tone = 0.440,
                      .secondary_container_tone = 0.380,
                      .error = 0.720,
                      .error_container = 0.420};
constexpr Tones kLight{.surface = 0.985,
                       .lowest = 1.000,
                       .dim = 0.900,
                       .low = 0.965,
                       .container = 0.950,
                       .high = 0.930,
                       .highest = 0.910,
                       .bright = 0.995,
                       .on_surface = 0.220,
                       .on_surface_variant = 0.450,
                       .outline = 0.620,
                       .outline_variant = 0.860,
                       .primary_min = 0.400,
                       .primary_max = 0.540,
                       .container_tone = 0.870,
                       .secondary_container_tone = 0.840,
                       .error = 0.540,
                       .error_container = 0.900};

// OKLCH hue of a clear red, for the error roles whatever the accent.
constexpr double kErrorHue = 25.0 * std::numbers::pi / 180.0;

class Builder {
public:
    Builder(Lch accent, bool dark) : accent_{accent}, dark_{dark} {}

    [[nodiscard]] Color tone(double lightness, double max_chroma) const {
        return from_lch({lightness, std::min(accent_.c, max_chroma), accent_.h});
    }

    [[nodiscard]] Color neutral(double lightness) const { return tone(lightness, kTint); }

    // Light or dark text for a background, whichever reads better.
    [[nodiscard]] Color on(Color background) const {
        const Color light = neutral(0.990);
        const Color dark = neutral(0.200);
        return contrast(background, light) >= contrast(background, dark) ? light : dark;
    }

    // Moves the lightness away from the background until the colour reads on it.
    [[nodiscard]] Color readable(Lch lch, Color background) const {
        constexpr double step = 0.01;
        Color color = from_lch(lch);
        while (static_cast<double>(contrast(color, background)) < kReadable && lch.l > 0.0 &&
               lch.l < 1.0) {
            lch.l += dark_ ? step : -step;
            color = from_lch(lch);
        }
        return color;
    }

    [[nodiscard]] const Lch& accent() const { return accent_; }

private:
    Lch accent_;
    bool dark_;
};

}  // namespace

float contrast(Color a, Color b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    constexpr double flare = 0.05;
    return static_cast<float>((std::max(la, lb) + flare) / (std::min(la, lb) + flare));
}

Palette desktop_palette(const Desktop& desktop) {
    const Tones& t = desktop.dark ? kDark : kLight;
    const Lch accent = to_lch(desktop.accent.value_or(kDefaultAccent));
    const Builder b{accent, desktop.dark};

    Palette p;
    p.dark = desktop.dark;

    p.surface = b.neutral(t.surface);
    p.surface_container_lowest = b.neutral(t.lowest);
    p.surface_dim = b.neutral(t.dim);
    p.surface_container_low = b.neutral(t.low);
    p.surface_container = b.neutral(t.container);
    p.surface_variant = p.surface_container;
    p.surface_container_high = b.neutral(t.high);
    p.surface_container_highest = b.neutral(t.highest);
    p.surface_bright = b.neutral(t.bright);
    p.on_surface = b.neutral(t.on_surface);
    p.on_surface_variant = b.tone(t.on_surface_variant, 0.02);
    p.outline = b.tone(t.outline, 0.02);
    p.outline_variant = b.tone(t.outline_variant, 0.015);
    p.shadow = Color::rgb(0x000000);
    p.scrim = Color::rgb(0x000000);

    // The accent, as close to the desktop's as reads on the busiest surface it sits on.
    const Lch primary{std::clamp(accent.l, t.primary_min, t.primary_max), accent.c, accent.h};
    p.primary = b.readable(primary, p.surface_container_high);
    p.on_primary = b.on(p.primary);
    p.primary_container = b.tone(t.container_tone, 0.10);
    p.on_primary_container = b.on(p.primary_container);

    // Secondary and tertiary: the accent's neighbours, for the few places that want another hue.
    const double turn = std::numbers::pi / 6.0;
    const Lch secondary{primary.l, std::min(accent.c, 0.10), accent.h - turn};
    const Lch tertiary{primary.l, std::min(accent.c, 0.10), accent.h + (2.0 * turn)};
    p.secondary = b.readable(secondary, p.surface_container_high);
    p.on_secondary = b.on(p.secondary);
    p.secondary_container = b.tone(t.secondary_container_tone, 0.08);
    p.on_secondary_container = b.on(p.secondary_container);
    p.tertiary = b.readable(tertiary, p.surface_container_high);
    p.on_tertiary = b.on(p.tertiary);
    p.tertiary_container =
        from_lch({t.container_tone, std::min(accent.c, 0.10), accent.h + (2.0 * turn)});
    p.on_tertiary_container = b.on(p.tertiary_container);

    p.error = b.readable({t.error, 0.17, kErrorHue}, p.surface_container_high);
    p.on_error = b.on(p.error);
    p.error_container = from_lch({t.error_container, 0.10, kErrorHue});
    p.on_error_container = b.on(p.error_container);

    // The context menu's row under the pointer: the accent, as a desktop menu shows it.
    p.hover = p.primary;
    p.on_hover = p.on_primary;
    return p;
}

}  // namespace rummage::theme
