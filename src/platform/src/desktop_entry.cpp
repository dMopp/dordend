#include "rummage/platform/desktop_entry.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace rummage::platform {
namespace {

std::string_view trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t at = text.find(separator);
        parts.push_back(text.substr(0, at));
        if (at == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(at + 1);
    }
}

bool parse_bool(std::string_view value) { return value == "true" || value == "1"; }

// The localized key suffixes to try for a locale, best first: "it_IT@euro", "it_IT", "it@euro",
// "it". Empty for C and POSIX.
std::vector<std::string> locale_variants(std::string_view locale) {
    std::string_view modifier;
    if (const std::size_t at = locale.find('@'); at != std::string_view::npos) {
        modifier = locale.substr(at);
        locale = locale.substr(0, at);
    }
    if (const std::size_t dot = locale.find('.'); dot != std::string_view::npos) {
        locale = locale.substr(0, dot);  // the encoding does not take part
    }
    if (locale.empty() || locale == "C" || locale == "POSIX") {
        return {};
    }
    const std::string_view lang = locale.substr(0, locale.find('_'));
    std::vector<std::string> variants;
    const auto add = [&variants](std::string variant) {
        if (std::ranges::find(variants, variant) == variants.end()) {
            variants.push_back(std::move(variant));
        }
    };
    if (!modifier.empty()) {
        add(std::string{locale} + std::string{modifier});
    }
    add(std::string{locale});
    if (!modifier.empty()) {
        add(std::string{lang} + std::string{modifier});
    }
    add(std::string{lang});
    return variants;
}

// $name as a string; empty when unset.
std::string env(const char* name) {
    const char* const value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): at startup
    return value != nullptr ? std::string{value} : std::string{};
}

// An XDG base directory variable: its absolute entries, else the fallback.
std::vector<std::filesystem::path> dir_list(const std::string& value, std::string_view fallback) {
    std::vector<std::filesystem::path> dirs;
    for (const std::string_view dir : split(value, ':')) {
        if (dir.starts_with('/')) {
            dirs.emplace_back(dir);
        }
    }
    if (dirs.empty()) {
        for (const std::string_view dir : split(fallback, ':')) {
            dirs.emplace_back(dir);
        }
    }
    return dirs;
}

}  // namespace

Environment Environment::from_environment() {
    Environment environment;
    const std::string home = env("HOME");
    const std::string config_home = env("XDG_CONFIG_HOME");
    environment.config_dirs.emplace_back(config_home.starts_with('/')
                                             ? std::filesystem::path{config_home}
                                             : std::filesystem::path{home} / ".config");
    for (std::filesystem::path& dir : dir_list(env("XDG_CONFIG_DIRS"), "/etc/xdg")) {
        environment.config_dirs.push_back(std::move(dir));
    }
    const std::string data_home = env("XDG_DATA_HOME");
    environment.data_dirs.emplace_back(data_home.starts_with('/')
                                           ? std::filesystem::path{data_home}
                                           : std::filesystem::path{home} / ".local" / "share");
    for (std::filesystem::path& dir :
         dir_list(env("XDG_DATA_DIRS"), "/usr/local/share:/usr/share")) {
        environment.data_dirs.push_back(std::move(dir));
    }
    // Kept in a variable: split() gives views into it, and a temporary would be gone by the
    // loop's first turn.
    const std::string desktops = env("XDG_CURRENT_DESKTOP");
    for (const std::string_view desktop : split(desktops, ':')) {
        if (!desktop.empty()) {
            std::string lower{desktop};
            std::ranges::transform(lower, lower.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            environment.desktops.push_back(std::move(lower));
        }
    }
    for (const char* const name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        environment.locale = env(name);
        if (!environment.locale.empty()) {
            break;
        }
    }
    environment.path = env("PATH");
    if (environment.path.empty()) {
        environment.path = "/usr/local/bin:/usr/bin:/bin";
    }
    environment.terminal = env("TERMINAL");
    return environment;
}

std::string unescape_value(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            out.push_back(value[i]);
            continue;
        }
        switch (value[++i]) {
            case 's':
                out.push_back(' ');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case '\\':
                out.push_back('\\');
                break;
            default:  // not an escape: kept as written (";" in lists is split_list's business)
                out.push_back('\\');
                out.push_back(value[i]);
                break;
        }
    }
    return out;
}

std::vector<std::string> split_list(std::string_view value) {
    std::vector<std::string> items;
    std::string raw;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == ';') {
            raw.push_back(';');
            ++i;
        } else if (value[i] == ';') {
            if (!raw.empty()) {
                items.push_back(unescape_value(raw));
            }
            raw.clear();
        } else {
            raw.push_back(value[i]);
        }
    }
    if (!raw.empty()) {
        items.push_back(unescape_value(raw));
    }
    return items;
}

std::optional<DesktopEntry> parse_desktop_entry(std::string_view text, std::string_view locale) {
    const std::vector<std::string> variants = locale_variants(locale);
    DesktopEntry entry;
    bool in_group = false;
    bool found = false;
    std::size_t name_rank = variants.size() + 1;  // lower is better; unlocalized = variants.size()
    for (std::string_view line : split(text, '\n')) {
        line = trim(line);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line.starts_with('[')) {
            if (found && in_group) {
                break;  // the group is over; other groups (actions) do not matter here
            }
            in_group = line == "[Desktop Entry]";
            found = found || in_group;
            continue;
        }
        if (!in_group) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        std::string_view key_locale;
        if (const std::size_t open = key.find('['); open != std::string_view::npos) {
            if (!key.ends_with(']')) {
                continue;
            }
            key_locale = key.substr(open + 1, key.size() - open - 2);
            key = key.substr(0, open);
        }
        if (key == "Name") {
            std::size_t rank = variants.size();
            if (!key_locale.empty()) {
                const auto it = std::ranges::find(variants, key_locale);
                if (it == variants.end()) {
                    continue;
                }
                rank = static_cast<std::size_t>(it - variants.begin());
            }
            if (rank <= name_rank) {
                name_rank = rank;
                entry.name = unescape_value(value);
            }
            continue;
        }
        if (!key_locale.empty()) {
            continue;  // no other localized key matters here
        }
        if (key == "Type") {
            entry.type = unescape_value(value);
        } else if (key == "Exec") {
            entry.exec = unescape_value(value);
        } else if (key == "TryExec") {
            entry.try_exec = unescape_value(value);
        } else if (key == "Icon") {
            entry.icon = unescape_value(value);
        } else if (key == "Path") {
            entry.working_directory = unescape_value(value);
        } else if (key == "Terminal") {
            entry.terminal = parse_bool(value);
        } else if (key == "Hidden") {
            entry.hidden = parse_bool(value);
        } else if (key == "DBusActivatable") {
            entry.dbus_activatable = parse_bool(value);
        }
    }
    if (!found) {
        return std::nullopt;
    }
    return entry;
}

std::optional<std::filesystem::path> find_program(std::string_view program, std::string_view path) {
    const auto runnable = [](const std::filesystem::path& file) {
        struct stat info{};
        return stat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
               access(file.c_str(), X_OK) == 0;
    };
    if (program.empty()) {
        return std::nullopt;
    }
    if (program.find('/') != std::string_view::npos) {
        std::filesystem::path file{program};
        return runnable(file) ? std::optional{std::move(file)} : std::nullopt;
    }
    for (const std::string_view dir : split(path, ':')) {
        std::filesystem::path file =
            std::filesystem::path{dir.empty() ? std::string_view{"."} : dir} / program;
        if (runnable(file)) {
            return file;
        }
    }
    return std::nullopt;
}

}  // namespace rummage::platform
