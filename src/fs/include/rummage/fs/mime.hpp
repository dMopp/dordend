#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rummage::fs {

struct Entry;

// A MIME type interned by a MimeDatabase; 0 is "unknown". Ids are only meaningful with the
// database that made them.
using MimeId = std::uint32_t;
inline constexpr MimeId kUnknownMime = 0;

// The freedesktop shared-mime-info database (globs2, magic, aliases, subclasses), read from the
// "mime" directories of the XDG data dirs. Detection follows the spec's recommended order, as
// GLib does: the file name first (no I/O), then the content (magic) only when the name gives no
// type or several of equal standing, then a text/binary guess.
//
// Immutable after load(): safe to share between threads.
class MimeDatabase {
public:
    // Well-known types, interned first so their ids are fixed.
    static constexpr MimeId kOctetStream = 1;  // application/octet-stream
    static constexpr MimeId kTextPlain = 2;    // text/plain
    static constexpr MimeId kDirectory = 3;    // inode/directory
    static constexpr MimeId kZeroSize = 4;     // application/x-zerosize (empty file, no glob)
    static constexpr MimeId kSymlink = 5;      // inode/symlink (dangling link)
    static constexpr MimeId kSpecial = 6;      // inode/x-special (devices, sockets, fifos)

    // $XDG_DATA_HOME/mime, then each $XDG_DATA_DIRS/mime (defaults per the base directory spec),
    // highest priority first. Missing files are skipped; problems are logged. Never throws.
    [[nodiscard]] static std::shared_ptr<const MimeDatabase> load();

    // Same, from explicit mime directories, highest priority first (tests).
    [[nodiscard]] static std::shared_ptr<const MimeDatabase> load(
        const std::vector<std::filesystem::path>& mime_dirs);

    struct NameMatch {
        MimeId mime{kUnknownMime};  // best glob match
        bool certain{false};        // one type won: no need to look at the content
    };

    // Glob match on a file name (not a path).
    [[nodiscard]] NameMatch by_name(std::string_view file_name) const;

    // Best magic match on the first bytes of a file; kUnknownMime when none matches.
    [[nodiscard]] MimeId by_content(std::span<const std::byte> head) const;

    // Bytes of a file's head that by_content() may look at.
    [[nodiscard]] std::size_t magic_extent() const noexcept { return magic_extent_; }

    // Full detection for a directory entry: kind, name, then (only if needed) the content of
    // directory/entry.name. Blocking: call it on a worker.
    [[nodiscard]] MimeId detect(const std::filesystem::path& directory, const Entry& entry) const;

    // Detection from a name and an already read head (size = the whole file's size).
    [[nodiscard]] MimeId detect(std::string_view file_name, std::span<const std::byte> head,
                                std::uint64_t size) const;

    // The type a file is opened as: an empty file (kZeroSize, as GLib says) takes its name's
    // type when the name has one, so a new, empty notes.txt opens in a text editor; any other
    // type is kept.
    [[nodiscard]] MimeId to_open(MimeId mime, std::string_view file_name) const;

    [[nodiscard]] std::string_view name(MimeId mime) const;

    // Id of a type name, aliases resolved; kUnknownMime if the database does not know it.
    [[nodiscard]] MimeId find(std::string_view name) const;

    // True when mime is parent or a subtype of it (subclasses file, text/* -> text/plain,
    // everything but inode/* -> application/octet-stream).
    [[nodiscard]] bool is_a(MimeId mime, MimeId parent) const;

    // The direct parents of a type from the subclasses file, in the file's order. The implicit
    // ones (text/* -> text/plain, everything -> application/octet-stream) are not listed, as in
    // GLib's g_content_type_get_parents().
    [[nodiscard]] std::span<const MimeId> parents(MimeId mime) const;

    // The type's generic icon name (shared-mime-info generic-icons, else "<media>-x-generic"),
    // e.g. "package-x-generic" for archives, "image-x-generic" for images.
    [[nodiscard]] std::string generic_icon(MimeId mime) const;

    [[nodiscard]] std::size_t glob_count() const noexcept;
    [[nodiscard]] std::size_t magic_count() const noexcept;

    ~MimeDatabase();
    MimeDatabase(const MimeDatabase&) = delete;
    MimeDatabase& operator=(const MimeDatabase&) = delete;
    MimeDatabase(MimeDatabase&&) = delete;
    MimeDatabase& operator=(MimeDatabase&&) = delete;

private:
    struct Impl;
    MimeDatabase();

    [[nodiscard]] bool is_a(MimeId mime, MimeId parent, int depth) const;

    std::unique_ptr<Impl> impl_;
    std::size_t magic_extent_{0};
};

}  // namespace rummage::fs
