#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "rummage/core/file.hpp"
#include "rummage/picture/image.hpp"

namespace rummage::core {
class CancelToken;
}

namespace rummage::thumbs {

// The sizes of the freedesktop thumbnail spec (0.9): a thumbnail fits in a square of that side.
enum class Size : std::uint8_t { normal, large, x_large, xx_large };

[[nodiscard]] std::uint32_t pixels(Size size) noexcept;  // 128, 256, 512, 1024

[[nodiscard]] std::string_view directory_name(Size size) noexcept;  // "normal", "large", ...

// The smallest size whose side is at least side pixels; xx_large when none is.
[[nodiscard]] Size size_for(std::uint32_t side) noexcept;

// The file:// URI of a path as GLib writes it: other apps find the thumbnail by the MD5 of this
// string, so it must match byte for byte.
using core::file_uri;

// $XDG_CACHE_HOME/thumbnails, or ~/.cache/thumbnails. Reads the environment: call it at startup.
[[nodiscard]] std::filesystem::path default_root();

// Where a thumbnail comes from.
enum class Source : std::uint8_t {
    cache,    // a valid thumbnail was already there (ours or another app's)
    preview,  // made from a preview embedded in the file
    decoded,  // made from the whole image
    failed,   // could not be made (now, or remembered as failed)
};

[[nodiscard]] std::string_view source_name(Source source) noexcept;

struct Thumbnail {
    picture::Image image;  // upright, fits in pixels(size); empty when failed
    Source source{Source::failed};
    std::string error;  // why it failed
};

// The freedesktop thumbnail cache (~/.cache/thumbnails), shared with the other apps: a
// thumbnail is a PNG named after the MD5 of the file's URI, valid while its Thumb::URI and
// Thumb::MTime match the file. Failures are remembered in fail/<app>/ so that a file that cannot
// be read is not tried again until it changes.
//
// Stateless apart from its paths: thread-safe, meant to be called from workers.
class Cache {
public:
    // app names the failure directory, e.g. "rummage-0.0.1".
    Cache(std::filesystem::path root, std::string app);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // Where the thumbnail of file (an absolute path) lives, valid or not.
    [[nodiscard]] std::filesystem::path path_for(const std::filesystem::path& file,
                                                 Size size) const;

    // The thumbnail of file at size: the cached one when valid, else made from the file and
    // stored. Blocking (reads, decodes, writes): call it on a worker.
    [[nodiscard]] Thumbnail get(const std::filesystem::path& file, Size size,
                                std::string_view mime = {},
                                const core::CancelToken* cancel = nullptr) const;

    // Like get(), but never from the cache, and a remembered failure is tried again.
    [[nodiscard]] Thumbnail make(const std::filesystem::path& file, Size size,
                                 std::string_view mime = {},
                                 const core::CancelToken* cancel = nullptr) const;

private:
    [[nodiscard]] std::filesystem::path fail_path(const std::filesystem::path& file,
                                                  std::string_view mime) const;
    [[nodiscard]] Thumbnail make(const std::filesystem::path& file, Size size, std::int64_t mtime,
                                 std::string_view mime, const core::CancelToken* cancel) const;

    std::filesystem::path root_;
    std::string app_;
};

}  // namespace rummage::thumbs
