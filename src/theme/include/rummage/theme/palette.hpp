#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/ui/color.hpp"

// The color palette, in Noctalia's (Material) role names. Noctalia renders it to
// ~/.config/rummage/colors.toml through the rummage template:
//
//   mode = "dark"
//   [colors]
//   primary = "#fff59b"
//   ...
//
// The template writes every token Noctalia has; rummage reads the roles below and ignores the
// rest, so the template does not change when rummage starts using another role.
namespace rummage::theme {

using ui::Color;

// Defaults: Noctalia 5.3.0's built-in "Noctalia" palette, dark mode, as Noctalia expands it
// (src/theme/fixed_palette.cpp). Used when colors.toml is missing, and per role when a role is
// missing from it.
struct Palette {
    bool dark{true};

    Color primary{Color::rgb(0xFFF59B)};
    Color on_primary{Color::rgb(0x0E0E43)};
    Color primary_container{Color::rgb(0xE8D000)};
    Color on_primary_container{Color::rgb(0x5E5A31)};
    Color secondary{Color::rgb(0xA9AEFE)};
    Color on_secondary{Color::rgb(0x0E0E43)};
    Color secondary_container{Color::rgb(0x0210F2)};
    Color on_secondary_container{Color::rgb(0xCDD0FE)};
    Color tertiary{Color::rgb(0x9BFECE)};
    Color on_tertiary{Color::rgb(0x0E0E43)};
    Color tertiary_container{Color::rgb(0x02E577)};
    Color on_tertiary_container{Color::rgb(0x305C49)};
    Color error{Color::rgb(0xFD4663)};
    Color on_error{Color::rgb(0x0E0E43)};
    Color error_container{Color::rgb(0x8F0118)};
    Color on_error_container{Color::rgb(0xFECDD4)};

    Color surface{Color::rgb(0x070722)};
    Color on_surface{Color::rgb(0xF3EDF7)};
    Color surface_variant{Color::rgb(0x11112D)};
    Color on_surface_variant{Color::rgb(0x7C80B4)};
    Color surface_dim{Color::rgb(0x040411)};
    Color surface_bright{Color::rgb(0x222259)};
    Color surface_container_lowest{Color::rgb(0x090924)};
    Color surface_container_low{Color::rgb(0x0C0C27)};
    Color surface_container{Color::rgb(0x11112D)};
    Color surface_container_high{Color::rgb(0x17173C)};
    Color surface_container_highest{Color::rgb(0x1C1C4B)};

    Color outline{Color::rgb(0x51589B)};
    Color outline_variant{Color::rgb(0x21215F)};
    Color shadow{Color::rgb(0x070722)};
    Color scrim{Color::rgb(0x000000)};
    Color hover{Color::rgb(0x9BFECE)};
    Color on_hover{Color::rgb(0x0E0E43)};
};

struct PaletteResult {
    Palette palette;
    bool from_file{false};  // the file existed and parsed; false means the default palette
    // One line per problem, ready to print: "<file>:<line>:<column>: <what>".
    std::vector<std::string> problems;
};

// colors.toml next to config.toml (see core::default_config_path); empty when that is empty.
[[nodiscard]] std::filesystem::path default_colors_path();

// Parses colors.toml text. source names the file in problem messages. A file that does not parse
// gives the default palette with from_file = false: on a reload, keep the current palette.
[[nodiscard]] PaletteResult parse_palette(std::string_view text, std::string_view source);

// Reads and parses a file. A missing file is not a problem: defaults, from_file = false.
[[nodiscard]] PaletteResult load_palette(const std::filesystem::path& path);

}  // namespace rummage::theme
