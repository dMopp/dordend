#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/platform/desktop_entry.hpp"

namespace rummage::platform {

// An Exec value split into arguments by the Desktop Entry spec's quoting rules: arguments are
// separated by spaces; a double-quoted argument may hold spaces, and inside it \" \` \$ \\ stand
// for the character. Field codes (%f, ...) are left in place. nullopt when a quote is not closed
// or nothing is left.
[[nodiscard]] std::optional<std::vector<std::string>> split_exec(std::string_view exec);

// The command lines that open files (absolute paths) with an entry, one per process: a single
// one when the Exec line takes a list (%F, %U), one per file when it takes one (%f, %u). The
// field codes are expanded: %f a path, %u a file:// URI, %i "--icon" and the Icon, %c the Name,
// %k the desktop file, %% a '%'; the deprecated ones are dropped. An Exec line without a file
// code gets the file appended, as GLib does. Empty when Exec cannot be split.
[[nodiscard]] std::vector<std::vector<std::string>> expand_exec(
    const DesktopEntry& entry, std::span<const std::filesystem::path> files);

// The command line that runs argv in a terminal window, from the first terminal found: the
// environment's $TERMINAL (with -e, or the form it takes when it is one of the known terminals),
// xdg-terminal-exec, then the known terminals (foot, ghostty, kitty, alacritty, wezterm, konsole,
// gnome-terminal, xterm) in the PATH. nullopt when there is none.
[[nodiscard]] std::optional<std::vector<std::string>> in_terminal(
    const std::vector<std::string>& argv, const Environment& environment);

// The command line that opens the same terminal as in_terminal() with its shell and nothing to
// run: the program and $TERMINAL's own options. Started in a folder, the shell opens there.
// nullopt when there is none.
[[nodiscard]] std::optional<std::vector<std::string>> terminal_alone(
    const Environment& environment);

}  // namespace rummage::platform
