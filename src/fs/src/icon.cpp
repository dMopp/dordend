#include "rummage/fs/icon.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rummage::fs {
namespace {

// Types with an icon of their own, checked in order with is_a(): subtypes match too, so
// image/x-dcraw covers every camera RAW format and text/x-python3 counts as code.
constexpr std::array<std::pair<std::string_view, std::string_view>, 30> kByType{{
    {"image/x-dcraw", "aperture"},
    {"image/svg+xml", "vector"},
    {"application/pdf", "file-type-pdf"},
    {"text/markdown", "markdown"},
    {"application/x-sqlite3", "database"},
    {"application/vnd.sqlite3", "database"},
    {"application/x-desktop", "app-window"},
    {"text/calendar", "calendar"},
    {"message/rfc822", "mail"},
    // Source code and structured text: before the generic icons, which file most of these
    // under text-x-generic (and Python under executables).
    {"text/x-csrc", "file-code"},
    {"text/x-chdr", "file-code"},
    {"text/x-c++src", "file-code"},
    {"text/x-c++hdr", "file-code"},
    {"text/x-python", "file-code"},
    {"text/rust", "file-code"},
    {"text/x-go", "file-code"},
    {"text/x-java", "file-code"},
    {"text/javascript", "file-code"},
    {"application/x-shellscript", "file-code"},
    {"text/x-makefile", "file-code"},
    {"text/x-cmake", "file-code"},
    {"text/css", "file-code"},
    {"text/html", "file-code"},
    {"application/xml", "file-code"},
    {"application/json", "file-code"},
    {"application/toml", "file-code"},
    {"application/yaml", "file-code"},
    {"application/x-yaml", "file-code"},
    {"text/x-lua", "file-code"},
    {"text/x-luau", "file-code"},
}};

// freedesktop generic icons (and "<media>-x-generic" names) to Tabler icons.
constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kByGenericIcon{{
    {"folder", "folder"},
    {"package-x-generic", "file-zip"},
    {"x-office-document", "file-description"},
    {"x-office-spreadsheet", "file-spreadsheet"},
    {"x-office-presentation", "presentation"},
    {"text-x-script", "file-code"},
    {"text-html", "file-code"},
    {"application-x-executable", "terminal-2"},
    {"font-x-generic", "typography"},
    {"media-optical", "disc"},
    {"media-floppy", "disc"},
    {"image-x-generic", "photo"},
    {"video-x-generic", "movie"},
    {"audio-x-generic", "music"},
    {"text-x-generic", "file-text"},
    {"text-x-generic-template", "file-text"},
}};

constexpr std::string_view kFile = "file";

bool ends_with_ignoring_case(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string_view> icon_names_in_use() {
    std::vector<std::string_view> names{kFile,      "folder", "folder-symlink",
                                        "link-off", "plug",   "adjustments-horizontal"};
    for (const auto& [type, icon] : kByType) {
        names.push_back(icon);
    }
    for (const auto& [generic, icon] : kByGenericIcon) {
        names.push_back(icon);
    }
    std::ranges::sort(names);
    const auto [first, last] = std::ranges::unique(names);
    names.erase(first, last);
    return names;
}

std::string_view icon_name(const MimeDatabase& db, MimeId mime) {
    if (mime == kUnknownMime || mime == MimeDatabase::kZeroSize) {
        return kFile;
    }
    if (mime == MimeDatabase::kDirectory) {
        return "folder";
    }
    for (const auto& [type, icon] : kByType) {
        if (const MimeId parent = db.find(type); parent != kUnknownMime && db.is_a(mime, parent)) {
            return icon;
        }
    }
    const std::string generic = db.generic_icon(mime);
    for (const auto& [name, icon] : kByGenericIcon) {
        if (generic == name) {
            return icon;
        }
    }
    return kFile;
}

std::string_view icon_name(const MimeDatabase& db, const Entry& entry) {
    switch (entry.kind) {
        case EntryKind::directory:
            return entry.is_link ? "folder-symlink" : "folder";
        case EntryKind::other:
            return entry.is_link ? "link-off" : "plug";  // dangling link; device, socket, fifo
        case EntryKind::file:
        case EntryKind::unknown:
            break;
    }
    // darktable's sidecars: edits of the photo next to them. shared-mime-info has no type for
    // them (they sniff as XML), and to a photographer they are not code.
    if (ends_with_ignoring_case(entry.name, ".xmp")) {
        return "adjustments-horizontal";
    }
    // An empty file's type is "empty" (as GLib says), but a freshly created notes.txt should
    // still look like text: the icon follows the name, as opening does.
    return icon_name(db, db.to_open(entry.mime, entry.name));
}

}  // namespace rummage::fs
