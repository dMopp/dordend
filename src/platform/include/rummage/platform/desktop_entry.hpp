#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::platform {

// What rummage reads of the environment to find and start applications, read once at startup
// (getenv is not thread-safe) and then handed to workers.
struct Environment {
    // Where mimeapps.list files are, highest priority first: $XDG_CONFIG_HOME (or ~/.config),
    // then $XDG_CONFIG_DIRS (or /etc/xdg).
    std::vector<std::filesystem::path> config_dirs;
    // The XDG data dirs, highest priority first: $XDG_DATA_HOME (or ~/.local/share), then
    // $XDG_DATA_DIRS (or /usr/local/share:/usr/share). Desktop entries are in their
    // "applications" folders.
    std::vector<std::filesystem::path> data_dirs;
    // $XDG_CURRENT_DESKTOP, split at ':' and lowercased ("umbriel"): desktop-specific
    // mimeapps.list files come first.
    std::vector<std::string> desktops;
    // $LC_ALL, else $LC_MESSAGES, else $LANG ("it_IT.UTF-8"): picks localized names.
    std::string locale;
    // $PATH (or "/usr/local/bin:/usr/bin:/bin"): where programs are looked up.
    std::string path;
    // $TERMINAL: the user's terminal, tried first for Terminal=true applications.
    std::string terminal;

    [[nodiscard]] static Environment from_environment();
};

// The keys of a desktop entry's [Desktop Entry] group that opening files needs (freedesktop
// Desktop Entry Specification 1.5). String values are unescaped (\s \n \t \r \\).
struct DesktopEntry {
    std::string id;  // the desktop file ID: "org.darktable.darktable.desktop"
    std::filesystem::path file;
    std::string type;  // "Application", "Link", "Directory"
    std::string name;  // localized for Environment::locale
    std::string exec;  // still quoted: split_exec() reads it
    std::string try_exec;
    std::string icon;
    std::filesystem::path working_directory;  // Path
    bool terminal{false};
    bool hidden{false};  // Hidden=true: the user deleted it, it does not exist
    bool dbus_activatable{false};
};

// Parses a desktop file's text. nullopt when it has no [Desktop Entry] group. locale picks the
// localized Name (the spec's matching: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang).
[[nodiscard]] std::optional<DesktopEntry> parse_desktop_entry(std::string_view text,
                                                              std::string_view locale);

// A string value with the spec's escapes decoded.
[[nodiscard]] std::string unescape_value(std::string_view value);

// A list value ("image/png;image/jpeg;") split at unescaped ';', each item unescaped; empty items
// are dropped.
[[nodiscard]] std::vector<std::string> split_list(std::string_view value);

// The program a command names: itself when it contains a '/', else the first match in path (a
// ':'-separated list) that is an executable regular file. nullopt when there is none.
[[nodiscard]] std::optional<std::filesystem::path> find_program(std::string_view program,
                                                                std::string_view path);

}  // namespace rummage::platform
