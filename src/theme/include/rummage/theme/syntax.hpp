#pragma once

#include <array>
#include <cstddef>

#include "rummage/theme/palette.hpp"

// The colours of source code in Quick Look and the info pane (ROADMAP 3b-2), made from the
// palette so that a new theme recolours the code too: keywords in the primary colour, types in
// the tertiary, strings in the secondary, comments in on_surface_variant, removed lines in the
// error colour; numbers, functions and added lines from hues between those (OKLCH). Every colour
// is moved in lightness until it reads on surface_container (WCAG AA, 4.5:1), the page the text
// is drawn on.
namespace rummage::theme {

// In text::Style's order: keyword, type, string, number, comment, function, added, removed.
inline constexpr std::size_t kSyntaxColors = 8;
using SyntaxColors = std::array<Color, kSyntaxColors>;

[[nodiscard]] SyntaxColors syntax_colors(const Palette& palette);

}  // namespace rummage::theme
