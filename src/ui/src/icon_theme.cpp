#include "rummage/ui/icon_theme.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>

#include "rummage/picture/scale.hpp"
#include "rummage/picture/wuffs.hpp"
#include "rummage/svg/svg.hpp"

namespace rummage::ui {
namespace {

using Section = std::map<std::string, std::string>;
using Ini = std::map<std::string, Section>;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string{text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1)};
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> result;
    while (!text.empty()) {
        const auto end = text.find(separator);
        const std::string value = trim(text.substr(0, end));
        if (!value.empty()) {
            result.push_back(value);
        }
        if (end == std::string_view::npos) {
            break;
        }
        text.remove_prefix(end + 1);
    }
    return result;
}

Ini read_ini(const std::filesystem::path& path) {
    Ini result;
    std::ifstream file{path};
    std::string line;
    std::string section;
    std::size_t bytes = 0;
    while (std::getline(file, line) && (bytes += line.size()) < 1024U * 1024U) {
        line = trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
        } else if (const auto equal = line.find('='); equal != std::string::npos) {
            std::string value = trim(std::string_view{line}.substr(equal + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            result[section][trim(std::string_view{line}.substr(0, equal))] = std::move(value);
        }
    }
    return result;
}

int number(const Section& section, const std::string& key, int fallback) {
    const auto it = section.find(key);
    if (it == section.end()) {
        return fallback;
    }
    int value = 0;
    const auto& text = it->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && value >= 0 && value <= 4096
               ? value
               : fallback;
}

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

bool safe_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos;
}

bool safe_directory(const std::filesystem::path& path) {
    return !path.empty() && !path.is_absolute() &&
           std::ranges::none_of(path, [](const auto& part) { return part == ".."; });
}

}  // namespace

std::string desktop_icon_theme() {
    std::filesystem::path config = env("XDG_CONFIG_HOME");
    if (!config.is_absolute()) {
        config = std::filesystem::path{env("HOME")} / ".config";
    }
    const auto gtk3 = read_ini(config / "gtk-3.0/settings.ini");
    const auto gtk4 = read_ini(config / "gtk-4.0/settings.ini");
    const auto kde = read_ini(config / "kdeglobals");
    const auto qt = read_ini(config / "qt6ct/qt6ct.conf");
    const auto setting = [](const Ini& ini, const std::string& section, const std::string& key) {
        const auto group = ini.find(section);
        if (group == ini.end()) {
            return std::string{};
        }
        const auto value = group->second.find(key);
        return value == group->second.end() ? std::string{} : value->second;
    };
    std::vector<std::string> choices;
    if (env("XDG_CURRENT_DESKTOP").find("KDE") != std::string::npos) {
        choices.push_back(setting(kde, "Icons", "Theme"));
    }
    choices.push_back(setting(gtk3, "Settings", "gtk-icon-theme-name"));
    choices.push_back(setting(gtk4, "Settings", "gtk-icon-theme-name"));
    choices.push_back(setting(kde, "Icons", "Theme"));
    choices.push_back(setting(qt, "Appearance", "icon_theme"));
    for (const auto& choice : choices) {
        if (safe_name(choice)) {
            return choice;
        }
    }
    return "hicolor";
}

std::vector<std::filesystem::path> icon_theme_roots() {
    std::vector<std::filesystem::path> roots;
    const std::filesystem::path home{env("HOME")};
    const std::filesystem::path data{env("XDG_DATA_HOME")};
    if (home.is_absolute()) {
        roots.push_back(home / ".icons");
    }
    if (data.is_absolute()) {
        roots.push_back(data / "icons");
    } else if (home.is_absolute()) {
        roots.push_back(home / ".local/share/icons");
    }
    std::string dirs = env("XDG_DATA_DIRS");
    if (dirs.empty()) {
        dirs = "/usr/local/share:/usr/share";
    }
    for (const auto& dir : split(dirs, ':')) {
        if (std::filesystem::path{dir}.is_absolute()) {
            roots.emplace_back(std::filesystem::path{dir} / "icons");
        }
    }
    return roots;
}

IconTheme::IconTheme(std::string name, std::vector<std::filesystem::path> roots)
    : roots_{std::move(roots)} {
    add_theme(name);
    add_theme("hicolor");
}

void IconTheme::add_theme(const std::string& name) {
    if (!safe_name(name) || themes_.size() >= 32 ||
        std::ranges::any_of(themes_, [&](const Theme& theme) { return theme.name == name; })) {
        return;
    }
    Ini ini;
    for (const auto& root : roots_) {
        ini = read_ini(root / name / "index.theme");
        if (ini.contains("Icon Theme")) {
            break;
        }
    }
    Theme theme{.name = name, .directories = {}};
    Section& header = ini["Icon Theme"];
    auto dirs = split(header["Directories"], ',');
    const auto scaled = split(header["ScaledDirectories"], ',');
    dirs.insert(dirs.end(), scaled.begin(), scaled.end());
    for (const auto& dir : dirs) {
        if (!safe_directory(dir)) {
            continue;
        }
        Section& info = ini[dir];
        const int size = number(info, "Size", 48);
        const int scale = std::clamp(number(info, "Scale", 1), 1, 8);
        int low = size;
        int high = size;
        if (info["Type"] == "Scalable") {
            low = number(info, "MinSize", size);
            high = number(info, "MaxSize", size);
        } else if (info["Type"] != "Fixed") {
            const int threshold = number(info, "Threshold", 2);
            low = std::max(0, size - threshold);
            high = size + threshold;
        }
        theme.directories.push_back({dir, low * scale, std::max(low, high) * scale});
    }
    themes_.push_back(std::move(theme));
    for (const auto& parent : split(header["Inherits"], ',')) {
        add_theme(parent);
    }
}

std::filesystem::path IconTheme::find(std::string_view icon, int pixels) const {
    if (icon.starts_with('/')) {
        const std::filesystem::path path{icon};
        std::error_code error;
        if ((path.extension() == ".svg" || path.extension() == ".png") &&
            std::filesystem::is_regular_file(path, error))
            return path;
        return {};
    }
    if (!safe_name(icon)) {
        return {};
    }
    for (const auto& theme : themes_) {
        int best = std::numeric_limits<int>::max();
        std::filesystem::path found;
        for (const auto& directory : theme.directories) {
            const int distance = std::max({directory.min - pixels, pixels - directory.max, 0});
            if (distance >= best) {
                continue;
            }
            for (const auto& root : roots_) {
                for (const auto* extension : {".png", ".svg"}) {
                    const auto candidate =
                        root / theme.name / directory.name / (std::string{icon} + extension);
                    std::error_code error;
                    if (std::filesystem::is_regular_file(candidate, error)) {
                        found = candidate;
                        best = distance;
                        break;
                    }
                }
                if (best == distance) {
                    break;
                }
            }
        }
        if (!found.empty()) {
            return found;
        }
    }
    for (const auto& root : roots_) {
        for (const auto* extension : {".png", ".svg"}) {
            const auto candidate = root / (std::string{icon} + extension);
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
    }
    return {};
}

picture::Image IconTheme::render(const std::filesystem::path& file, int pixels,
                                 bool* unsupported_svg_filter) {
    if (unsupported_svg_filter)
        *unsupported_svg_filter = false;
    pixels = std::clamp(pixels, 1, 1024);
    std::error_code error;
    const auto bytes = std::filesystem::file_size(file, error);
    if (error || bytes > 8U * 1024U * 1024U) {
        return {};
    }
    std::ifstream input{file, std::ios::binary};
    std::vector<std::uint8_t> data(bytes);
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(bytes))) {
        return {};
    }
    if (file.extension() == ".png") {
        const auto decoded = picture::decode_wuffs(data, 16U * 1024U * 1024U);
        if (decoded.image.empty()) {
            return {};
        }
        return picture::scale_to_fit(decoded.image, static_cast<std::uint32_t>(pixels),
                                     static_cast<std::uint32_t>(pixels));
    }
    return svg::render(data, pixels,
                       {[](void* context, std::string_view) {
                            if (context)
                                *static_cast<bool*>(context) = true;
                        },
                        unsupported_svg_filter});
}

std::vector<std::string> IconTheme::folder_icons() const {
    std::vector<std::string> names;
    const auto scan = [&names](const std::filesystem::path& directory) {
        std::error_code error;
        for (std::filesystem::directory_iterator it{directory, error};
             !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
            const auto& path = it->path();
            if (path.extension() != ".svg" && path.extension() != ".png")
                continue;
            const auto name = path.stem().string();
            std::error_code file_error;
            if ((name == "folder" || name.starts_with("folder-") || name == "user-home" ||
                 name == "user-desktop") &&
                !name.ends_with("-symbolic") && it->is_regular_file(file_error))
                names.push_back(name);
        }
    };
    for (const auto& theme : themes_)
        for (const auto& directory : theme.directories)
            for (const auto& root : roots_)
                scan(root / theme.name / directory.name);
    for (const auto& root : roots_)
        scan(root);
    std::ranges::sort(names);
    const auto duplicates = std::ranges::unique(names);
    names.erase(duplicates.begin(), duplicates.end());
    return names;
}

}  // namespace rummage::ui
