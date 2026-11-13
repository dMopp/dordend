#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>

#include <doctest/doctest.h>

#include "rummage/text/text.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/theme/syntax.hpp"

using rummage::theme::Color;
using rummage::theme::contrast;
using rummage::theme::desktop_palette;
using rummage::theme::Palette;
using rummage::theme::syntax_colors;
using rummage::theme::SyntaxColors;

namespace {

// As in desktop_palette_test.cpp: GNOME's accents, Plasma's, and the hard cases.
constexpr std::array kAccents{
    0x3584E4U, 0x2190A4U, 0x3A944AU, 0xC88800U, 0xED5B00U, 0xE62D42U, 0xD56199U, 0x9141ACU,
    0x6F8396U, 0x3DAEE9U, 0xFFFFFFU, 0x000000U, 0x808080U, 0xFFFF00U, 0x0000FFU,
};

// One step of an 8-bit channel.
constexpr float kClose = 1.0F / 255.0F;

bool same(Color a, Color b) {
    return std::abs(a.r - b.r) <= kClose && std::abs(a.g - b.g) <= kClose &&
           std::abs(a.b - b.b) <= kClose;
}

void every_colour_reads(const Palette& palette) {
    const SyntaxColors colours = syntax_colors(palette);
    for (std::size_t i = 0; i < colours.size(); ++i) {
        CAPTURE(i);
        CHECK(colours.at(i).a == 1.0F);  // opaque
        CHECK(contrast(colours.at(i), palette.surface_container) >= 4.5F);
    }
}

}  // namespace

TEST_CASE("syntax colours: one per style") {
    CHECK(rummage::theme::kSyntaxColors == rummage::text::kStyleCount);
}

TEST_CASE("syntax colours: Noctalia's palette as it is, every colour readable") {
    const Palette noctalia;
    every_colour_reads(noctalia);
    const SyntaxColors colours = syntax_colors(noctalia);
    // Its colours read already: kept as they are.
    CHECK(same(colours.at(0), noctalia.primary));    // keyword
    CHECK(same(colours.at(1), noctalia.tertiary));   // type
    CHECK(same(colours.at(2), noctalia.secondary));  // string
    CHECK(same(colours.at(7), noctalia.error));      // removed
    // Keywords, types, strings, comments and numbers apart.
    for (std::size_t a = 0; a < 5; ++a) {
        for (std::size_t b = a + 1; b < 5; ++b) {
            CAPTURE(std::format("{} {}", a, b));
            CHECK_FALSE(same(colours.at(a), colours.at(b)));
        }
    }
}

TEST_CASE("syntax colours: readable on every desktop palette, dark and light") {
    for (const bool dark : {true, false}) {
        for (const std::uint32_t accent : kAccents) {
            CAPTURE(std::format("#{:06x} {}", accent, dark ? "dark" : "light"));
            every_colour_reads(desktop_palette({.dark = dark, .accent = Color::rgb(accent)}));
        }
    }
}

TEST_CASE("syntax colours: a palette whose colours do not read on its page is mended") {
    Palette bad;
    bad.surface_container = Color::rgb(0x202020);
    bad.primary = Color::rgb(0x202020);  // the page's own colour
    bad.secondary = Color::rgb(0x252530);
    bad.tertiary = Color::rgb(0x1A1A1A);
    bad.error = Color::rgb(0x400000);
    bad.on_surface_variant = Color::rgb(0x303030);
    every_colour_reads(bad);
    Palette light = bad;
    light.surface_container = Color::rgb(0xF0F0F0);
    light.primary = Color::rgb(0xFFFF80);
    light.secondary = Color::rgb(0xE0E0FF);
    light.tertiary = Color::rgb(0xFFFFFF);
    light.error = Color::rgb(0xFFC0C0);
    light.on_surface_variant = Color::rgb(0xD0D0D0);
    every_colour_reads(light);
}
