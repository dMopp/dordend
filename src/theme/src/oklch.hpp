#pragma once

#include "rummage/ui/color.hpp"

// OKLCH, shared by the desktop palette and the application icon (private to the theme library).
namespace rummage::theme {

using ui::Color;

// OKLCH: lightness 0 to 1, chroma (0 grey, about 0.37 at most in sRGB), hue in radians.
// Conversions from Björn Ottosson's OKLab definition.
struct Lch {
    double l{0.0};
    double c{0.0};
    double h{0.0};
};

// One sRGB-encoded component to linear light.
[[nodiscard]] double to_linear(float component);

[[nodiscard]] Lch to_lch(Color color);

// The colour at that lightness and hue, with as much of the chroma as sRGB can show; opaque.
[[nodiscard]] Color from_lch(Lch lch);

}  // namespace rummage::theme
