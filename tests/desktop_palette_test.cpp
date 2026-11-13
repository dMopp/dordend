#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/portal.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/theme.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::platform::Appearance;
using rummage::platform::appearance_from;
using rummage::test::TempDir;
using rummage::theme::Color;
using rummage::theme::contrast;
using rummage::theme::Desktop;
using rummage::theme::desktop_palette;
using rummage::theme::kDefaultAccent;
using rummage::theme::Palette;
using rummage::theme::Theme;

namespace {

// GNOME's nine accents, Plasma's default, and the hard cases: no colour, the extremes, a pure
// yellow (light by itself), a pure blue (dark by itself).
constexpr std::array kAccents{
    0x3584E4U, 0x2190A4U, 0x3A944AU, 0xC88800U, 0xED5B00U, 0xE62D42U, 0xD56199U, 0x9141ACU,
    0x6F8396U, 0x3DAEE9U, 0xFFFFFFU, 0x000000U, 0x808080U, 0xFFFF00U, 0x0000FFU,
};

std::string name(std::uint32_t accent, bool dark) {
    return std::format("#{:06x} {}", accent, dark ? "dark" : "light");
}

float luminance(Color c) { return (0.2126F * c.r) + (0.7152F * c.g) + (0.0722F * c.b); }

}  // namespace

TEST_CASE("desktop palette: text and icons read on their backgrounds, for every accent") {
    for (const bool dark : {true, false}) {
        for (const std::uint32_t accent : kAccents) {
            CAPTURE(name(accent, dark));
            const Palette p = desktop_palette({.dark = dark, .accent = Color::rgb(accent)});
            CHECK(p.dark == dark);
            CHECK(contrast(p.on_surface, p.surface) >= 7.0F);
            CHECK(contrast(p.on_surface, p.surface_container_highest) >= 4.5F);
            CHECK(contrast(p.on_surface_variant, p.surface) >= 4.5F);
            CHECK(contrast(p.on_surface_variant, p.surface_container_low) >= 4.5F);
            // Accent-coloured icons and links on the surfaces.
            CHECK(contrast(p.primary, p.surface) >= 4.5F);
            CHECK(contrast(p.primary, p.surface_container_high) >= 4.5F);
            // Text on accent-coloured fills.
            CHECK(contrast(p.on_primary, p.primary) >= 4.5F);
            CHECK(contrast(p.on_primary_container, p.primary_container) >= 4.5F);
            CHECK(contrast(p.on_secondary_container, p.secondary_container) >= 4.5F);
            CHECK(contrast(p.on_hover, p.hover) >= 4.5F);
            CHECK(contrast(p.error, p.surface) >= 4.5F);
            CHECK(contrast(p.on_error, p.error) >= 4.5F);
            CHECK(contrast(p.on_error_container, p.error_container) >= 4.5F);
        }
    }
}

TEST_CASE("desktop palette: dark is dark, light is light, surfaces step the right way") {
    for (const std::uint32_t accent : kAccents) {
        CAPTURE(name(accent, true));
        const Palette dark = desktop_palette({.dark = true, .accent = Color::rgb(accent)});
        const Palette light = desktop_palette({.dark = false, .accent = Color::rgb(accent)});
        CHECK(luminance(dark.surface) < luminance(dark.on_surface));
        CHECK(luminance(light.surface) > luminance(light.on_surface));
        // Containers move away from the background: lighter in dark mode, darker in light mode.
        CHECK(luminance(dark.surface) < luminance(dark.surface_container_low));
        CHECK(luminance(dark.surface_container_low) < luminance(dark.surface_container));
        CHECK(luminance(dark.surface_container) < luminance(dark.surface_container_high));
        CHECK(luminance(dark.surface_container_high) < luminance(dark.surface_container_highest));
        CHECK(luminance(light.surface) > luminance(light.surface_container_low));
        CHECK(luminance(light.surface_container_low) > luminance(light.surface_container));
        CHECK(luminance(light.surface_container) > luminance(light.surface_container_high));
        CHECK(luminance(light.surface_container_high) > luminance(light.surface_container_highest));
    }
}

TEST_CASE("desktop palette: the accent's hue, and grey surfaces") {
    const Palette blue = desktop_palette({.dark = true, .accent = Color::rgb(0x3584E4)});
    CHECK(blue.primary.b > blue.primary.r);
    const Palette orange = desktop_palette({.dark = false, .accent = Color::rgb(0xED5B00)});
    CHECK(orange.primary.r > orange.primary.b);
    // The surfaces carry only a trace of the accent.
    for (const Palette& p : {blue, orange}) {
        CHECK(std::abs(p.surface.r - p.surface.b) < 0.03F);
        CHECK(std::abs(p.surface_container_high.r - p.surface_container_high.b) < 0.03F);
    }
    // No accent: the default one.
    CHECK(desktop_palette({.dark = true, .accent = std::nullopt}).primary ==
          desktop_palette({.dark = true, .accent = kDefaultAccent}).primary);
}

TEST_CASE("portal values: scheme and accent, checked") {
    CHECK(appearance_from(1U, std::nullopt).dark);
    CHECK_FALSE(appearance_from(0U, std::nullopt).dark);  // no preference reads as light
    CHECK_FALSE(appearance_from(2U, std::nullopt).dark);
    CHECK_FALSE(appearance_from(7U, std::nullopt).dark);
    CHECK_FALSE(appearance_from(std::nullopt, std::nullopt).dark);

    const std::array<double, 3> blue{0.2, 0.5, 0.9};
    CHECK(appearance_from(1U, blue).accent == blue);
    // Out of range is the portal's "unset".
    CHECK_FALSE(appearance_from(1U, std::array{-1.0, -1.0, -1.0}).accent);
    CHECK_FALSE(appearance_from(1U, std::array{0.5, 1.5, 0.5}).accent);
    CHECK(appearance_from(1U, blue) == Appearance{.dark = true, .accent = blue});
}

TEST_CASE("Theme: the desktop's look without colors.toml, the file's colours with it") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "colors.toml";
    Theme theme{file};
    int changes = 0;
    theme.set_change_handler([&] { ++changes; });
    CHECK(theme.palette().primary == Palette{}.primary);

    const Desktop gnome{.dark = false, .accent = Color::rgb(0x9141AC)};
    theme.set_desktop(gnome);
    CHECK(changes == 1);
    CHECK(theme.palette().surface == desktop_palette(gnome).surface);
    CHECK(theme.palette().primary == desktop_palette(gnome).primary);
    theme.set_desktop(gnome);  // the same look again: nothing to redraw
    CHECK(changes == 1);

    // colors.toml appears: it wins, and a desktop change no longer matters.
    dir.write("colors.toml", "[colors]\nprimary = \"#112233\"\n");
    theme.reload();
    CHECK(changes == 2);
    CHECK(theme.palette().primary == Color::rgb(0x112233));
    theme.set_desktop(Desktop{.dark = true, .accent = std::nullopt});
    CHECK(changes == 2);
    CHECK(theme.palette().primary == Color::rgb(0x112233));

    // Removed: back to the desktop's look, the latest one.
    std::filesystem::remove(file);
    theme.reload();
    CHECK(changes == 3);
    CHECK(theme.palette().dark);
    CHECK(theme.palette().surface ==
          desktop_palette({.dark = true, .accent = std::nullopt}).surface);

    // The desktop stops saying anything: the default palette.
    theme.set_desktop(std::nullopt);
    CHECK(changes == 4);
    CHECK(theme.palette().surface == Palette{}.surface);
}

TEST_CASE("portal: the real session bus answers or has no portal (reads only)") {
    EventLoop loop;
    const rummage::platform::DesktopPortal portal{loop, [] {}};
    if (!portal.available()) {
        MESSAGE("no session bus, portal or appearance settings: skipped");
        return;
    }
    const Appearance& appearance = portal.appearance();
    const std::string look = std::string{"desktop is "} + (appearance.dark ? "dark" : "light") +
                             (appearance.accent ? ", with an accent" : ", no accent");
    MESSAGE(look);
    if (appearance.accent) {
        for (const double v : *appearance.accent) {
            CHECK(v >= 0.0);
            CHECK(v <= 1.0);
        }
    }
}
