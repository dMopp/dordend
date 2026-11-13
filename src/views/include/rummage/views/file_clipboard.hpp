#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Files on the system clipboard (ROADMAP 6.5): the formats other programs read and write, and the
// clipboard as the browser sees it.
namespace rummage::views {

// The types rummage offers for files, the most precise first.
inline constexpr std::string_view kGnomeFiles = "x-special/gnome-copied-files";
inline constexpr std::string_view kUriList = "text/uri-list";
inline constexpr std::string_view kKdeCut = "application/x-kde-cutselection";
inline constexpr std::string_view kText = "text/plain;charset=utf-8";
// Text as other programs may offer it, preferred first.
inline constexpr std::array<std::string_view, 4> kTextTypes{kText, "UTF8_STRING", "text/plain",
                                                            "TEXT"};

// "file:///a/b%20c\r\n..." (RFC 2483: CRLF after each), each path as GLib writes a file URI.
[[nodiscard]] std::string uri_list(std::span<const std::filesystem::path> paths);

// "copy\nfile:///a\nfile:///b" or "cut\n...": what GNOME's and most file managers read.
[[nodiscard]] std::string gnome_copied_files(bool cut,
                                             std::span<const std::filesystem::path> paths);

// The paths, one per line: for a terminal or an editor.
[[nodiscard]] std::string path_lines(std::span<const std::filesystem::path> paths);

// The local path a file URI names ("file:///a%20b", "file://localhost/a"); nullopt for anything
// else (another host, another scheme, a relative path).
[[nodiscard]] std::optional<std::filesystem::path> path_from_uri(std::string_view uri);

// Files read from the clipboard.
struct ClipboardFiles {
    bool cut{false};  // moved, not copied
    std::vector<std::filesystem::path> paths;
    std::size_t refused{0};  // URIs that are not local files ("https://...", another host)
};

// A text/uri-list (comments and blank lines skipped, CRLF or LF).
[[nodiscard]] ClipboardFiles parse_uri_list(std::string_view text);

// x-special/gnome-copied-files: "copy" or "cut", then the URIs; nullopt without the first line.
[[nodiscard]] std::optional<ClipboardFiles> parse_gnome_copied_files(std::string_view text);

// The system clipboard, as the app connects it (Wayland's wl_data_device); tests give a fake.
// Without it, Ctrl+C, Ctrl+X and Ctrl+V work inside rummage only.
struct SystemClipboard {
    using Format = std::pair<std::string, std::string>;  // type, bytes
    using Done = std::function<void(std::optional<std::string> data)>;

    // Takes the clipboard with these formats (Ctrl+C, Ctrl+X).
    std::function<void(std::vector<Format> formats)> offer;
    // Empties it (a cut was pasted: the files are no longer there).
    std::function<void()> clear;
    // rummage's own offer is on it.
    std::function<bool()> owned;
    // Changes whenever the clipboard gets other contents: tells whether it still holds what was
    // read.
    std::function<std::uint64_t()> generation;
    // The types on it now; empty when it holds nothing.
    std::function<std::vector<std::string>()> types;
    // Reads one type; done gets nullopt when that failed. Later, on the loop thread.
    std::function<void(std::string type, Done done)> read;
};

}  // namespace rummage::views
