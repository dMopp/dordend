#pragma once

#include <optional>

#include "rummage/theme/palette.hpp"

// A palette for desktops without Noctalia (ROADMAP 7.4): built from what the desktop says about
// its look through the freedesktop portal (light or dark, accent colour), so rummage matches
// GNOME and Plasma when there is no colors.toml.
namespace rummage::theme {

struct Desktop {
    bool dark{false};
    std::optional<Color> accent;  // opaque; none: the desktop has no accent colour

    bool operator==(const Desktop&) const = default;
};

// GNOME's default blue, for desktops that give no accent.
inline constexpr Color kDefaultAccent = Color::rgb(0x3584E4);

// Neutral surfaces with a trace of the accent's hue, the accent for the primary roles, its
// lightness moved where text and icons stay readable on those surfaces (OKLCH tones).
[[nodiscard]] Palette desktop_palette(const Desktop& desktop);

// WCAG 2 contrast ratio of two opaque colours, 1 to 21.
[[nodiscard]] float contrast(Color a, Color b);

}  // namespace rummage::theme
