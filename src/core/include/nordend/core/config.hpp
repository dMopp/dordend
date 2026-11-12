#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// User settings from config.toml. Every key is optional: a missing key keeps its default, a bad
// value (wrong type, out of range) is reported and keeps its default, an unknown key is reported
// and ignored, and a file that does not parse is ignored as a whole. Reading a config never fails.
namespace nordend::core {

struct Config {
    struct Font {
        std::string family{"sans-serif"};    // fontconfig pattern, e.g. "Inter" or "Inter:size=11"
        std::string monospace{"monospace"};  // text previews, e.g. "JetBrains Mono"
    };

    struct Pointer {
        int double_click_ms{400};      // presses closer in time count as a double click
        int double_click_distance{4};  // ... and closer than this, in logical pixels
    };

    struct Icon {
        std::string theme{"system"};  // desktop theme, installed name, or "builtin"
        // Nordend's icon in the desktop's accent, written to ~/.local/share/icons (ROADMAP 9.2);
        // false takes it away and leaves the installed one.
        bool follow_accent{true};
    };

    struct Appearance {
        float background_opacity{1.0F};  // background only; foreground content stays solid
    };

    struct Plugins {
        // Plugins compiled in but not started (their ids: "remotes"); read at start.
        std::vector<std::string> disabled;
    };

    std::map<std::string, std::string> keybindings;

    Appearance appearance;
    Font font;
    Pointer pointer;
    Icon icon;
    Plugins plugins;
};

struct ConfigResult {
    Config config;
    bool from_file{false};  // the file existed and parsed; false means pure defaults
    // One line per problem, ready to print: "<file>:<line>:<column>: <what>".
    std::vector<std::string> problems;
};

// $XDG_CONFIG_HOME/nordend/config.toml, else $HOME/.config/nordend/config.toml; empty when
// neither variable is set.
[[nodiscard]] std::filesystem::path default_config_path();

// Creates the complete reference at path only if no entry exists. Existing files and
// symlinks are untouched; no path (no HOME/XDG_CONFIG_HOME) is a no-op.
[[nodiscard]] std::error_code create_default_config(const std::filesystem::path& path);

// Parses TOML text. source names the file in problem messages.
[[nodiscard]] ConfigResult parse_config(std::string_view text, std::string_view source);

// Reads and parses a file. A missing file is not a problem: defaults, from_file = false.
[[nodiscard]] ConfigResult load_config(const std::filesystem::path& path);

}  // namespace nordend::core
