#include "rummage/views/file_clipboard.hpp"

#include <algorithm>
#include <utility>

#include "rummage/thumbs/cache.hpp"

namespace rummage::views {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Calls line for each line of text, CR before LF dropped.
template <class Line>
void each_line(std::string_view text, Line line) {
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view one = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        if (one.ends_with('\r')) {
            one.remove_suffix(1);
        }
        line(one);
    }
}

void add_uri(ClipboardFiles& files, std::string_view uri) {
    if (std::optional<std::filesystem::path> path = path_from_uri(uri)) {
        files.paths.push_back(std::move(*path));
    } else {
        ++files.refused;
    }
}

}  // namespace

std::string uri_list(std::span<const std::filesystem::path> paths) {
    std::string text;
    for (const std::filesystem::path& path : paths) {
        text += thumbs::file_uri(path);
        text += "\r\n";
    }
    return text;
}

std::string gnome_copied_files(bool cut, std::span<const std::filesystem::path> paths) {
    std::string text = cut ? "cut" : "copy";
    for (const std::filesystem::path& path : paths) {
        text += '\n';
        text += thumbs::file_uri(path);
    }
    return text;
}

std::string path_lines(std::span<const std::filesystem::path> paths) {
    std::string text;
    for (const std::filesystem::path& path : paths) {
        if (!text.empty()) {
            text += '\n';
        }
        text += path.string();
    }
    return text;
}

std::optional<std::filesystem::path> path_from_uri(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    if (uri.size() < scheme.size() ||
        !std::ranges::equal(uri.substr(0, scheme.size()), scheme, [](char a, char b) {
            return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
        })) {
        return std::nullopt;
    }
    uri.remove_prefix(scheme.size());
    const std::size_t slash = uri.find('/');
    if (slash == std::string_view::npos) {
        return std::nullopt;
    }
    if (const std::string_view host = uri.substr(0, slash); !host.empty() && host != "localhost") {
        return std::nullopt;  // another machine's file
    }
    uri.remove_prefix(slash);
    uri = uri.substr(0, uri.find_first_of("?#"));
    std::string path;
    path.reserve(uri.size());
    for (std::size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int high = hex_value(uri[i + 1]);
            const int low = hex_value(uri[i + 2]);
            if (high >= 0 && low >= 0) {
                path.push_back(static_cast<char>((high << 4U) | low));
                i += 2;
                continue;
            }
        }
        path.push_back(uri[i]);
    }
    if (path.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    std::filesystem::path normal = std::filesystem::path{path}.lexically_normal();
    if (!normal.has_filename() && normal.has_relative_path()) {
        normal = normal.parent_path();  // "file:///a/b/": the folder b
    }
    return normal;
}

ClipboardFiles parse_uri_list(std::string_view text) {
    ClipboardFiles files;
    each_line(text, [&files](std::string_view line) {
        if (!line.empty() && !line.starts_with('#')) {
            add_uri(files, line);
        }
    });
    return files;
}

std::optional<ClipboardFiles> parse_gnome_copied_files(std::string_view text) {
    std::optional<ClipboardFiles> files;
    bool first = true;
    each_line(text, [&](std::string_view line) {
        if (std::exchange(first, false)) {
            if (line == "copy" || line == "cut") {
                files = ClipboardFiles{.cut = line == "cut", .paths = {}, .refused = 0};
            }
            return;
        }
        if (files && !line.empty()) {
            add_uri(*files, line);
        }
    });
    return files;
}

}  // namespace rummage::views
