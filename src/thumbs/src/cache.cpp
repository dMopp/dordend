#include "rummage/thumbs/cache.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/picture/wuffs.hpp"
#include "rummage/thumbs/md5.hpp"

namespace rummage::thumbs {
namespace {

constexpr std::string_view kUriKey = "Thumb::URI";
constexpr std::string_view kMtimeKey = "Thumb::MTime";

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return {};
    }
    std::vector<std::uint8_t> data(size);
    std::ifstream file{path, std::ios::binary};
    if (!file.read(reinterpret_cast<char*>(data.data()),  // NOLINT: bytes to char for read
                   static_cast<std::streamsize>(size))) {
        return {};
    }
    return data;
}

// The PNG at path was written for this URI and modification time (spec: both must match).
bool matches(std::span<const std::uint8_t> png, const std::string& uri, std::int64_t mtime) {
    bool uri_ok = false;
    bool mtime_ok = false;
    for (const auto& [key, value] : picture::png_text(png)) {
        if (key == kUriKey) {
            uri_ok = value == uri;
        } else if (key == kMtimeKey) {
            std::int64_t stored = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), stored);
            mtime_ok =
                error == std::errc{} && end == value.data() + value.size() && stored == mtime;
        }
    }
    return uri_ok && mtime_ok;
}

// Creates the directories down to path, the new ones private (0700, as the spec asks).
bool make_private_directories(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path partial;
    for (const std::filesystem::path& part : path) {
        partial /= part;
        if (!std::filesystem::is_directory(partial, error) && ::mkdir(partial.c_str(), 0700) != 0 &&
            errno != EEXIST) {
            return false;
        }
    }
    return std::filesystem::is_directory(path, error);
}

// Writes the PNG of image with the spec's text to target: to a private temporary file next to
// it, then renamed, so that readers never see half a thumbnail and two writers do not mix.
bool store(const std::filesystem::path& target, const picture::Image& image, const std::string& uri,
           std::int64_t mtime) {
    const std::vector<std::uint8_t> png =
        picture::encode_png(image, {{std::string{kUriKey}, uri},
                                    {std::string{kMtimeKey}, std::to_string(mtime)},
                                    {"Software", "rummage"}});
    if (png.empty() || !make_private_directories(target.parent_path())) {
        return false;
    }
    std::string temporary = target.string() + ".rummage-XXXXXX";
    const int fd = ::mkstemp(temporary.data());  // mode 0600
    if (fd < 0) {
        return false;
    }
    std::size_t written = 0;
    while (written < png.size()) {
        const ssize_t n = ::write(fd, png.data() + written, png.size() - written);
        if (n <= 0) {
            break;
        }
        written += static_cast<std::size_t>(n);
    }
    const bool ok = ::close(fd) == 0 && written == png.size() &&
                    ::rename(temporary.c_str(), target.c_str()) == 0;
    if (!ok) {
        ::unlink(temporary.c_str());
    }
    return ok;
}

Thumbnail failure(std::string error) {
    Thumbnail thumbnail;
    thumbnail.error = std::move(error);
    return thumbnail;
}

}  // namespace

std::uint32_t pixels(Size size) noexcept {
    switch (size) {
        case Size::normal:
            return 128;
        case Size::large:
            return 256;
        case Size::x_large:
            return 512;
        case Size::xx_large:
            break;
    }
    return 1024;
}

std::string_view directory_name(Size size) noexcept {
    switch (size) {
        case Size::normal:
            return "normal";
        case Size::large:
            return "large";
        case Size::x_large:
            return "x-large";
        case Size::xx_large:
            break;
    }
    return "xx-large";
}

Size size_for(std::uint32_t side) noexcept {
    for (const Size size : {Size::normal, Size::large, Size::x_large}) {
        if (side <= pixels(size)) {
            return size;
        }
    }
    return Size::xx_large;
}

std::string_view source_name(Source source) noexcept {
    switch (source) {
        case Source::cache:
            return "cache";
        case Source::preview:
            return "preview";
        case Source::decoded:
            return "decoded";
        case Source::failed:
            break;
    }
    return "failed";
}

std::filesystem::path default_root() {
    // NOLINTBEGIN(concurrency-mt-unsafe): read at startup, before any thread looks at it
    const char* cache = std::getenv("XDG_CACHE_HOME");
    if (cache != nullptr && *cache == '/') {
        return std::filesystem::path{cache} / "thumbnails";
    }
    const char* home = std::getenv("HOME");
    // NOLINTEND(concurrency-mt-unsafe)
    return std::filesystem::path{home != nullptr ? home : "/"} / ".cache" / "thumbnails";
}

Cache::Cache(std::filesystem::path root, std::string app)
    : root_{std::move(root)}, app_{std::move(app)} {}

std::filesystem::path Cache::path_for(const std::filesystem::path& file, Size size) const {
    return root_ / directory_name(size) / (md5_hex(file_uri(file)) + ".png");
}

std::filesystem::path Cache::fail_path(const std::filesystem::path& file,
                                       std::string_view mime) const {
    return root_ / "fail" / (mime.starts_with("video/") ? app_ + "-media-v1" : app_) /
           (md5_hex(file_uri(file)) + ".png");
}

Thumbnail Cache::get(const std::filesystem::path& file, Size size, std::string_view mime,
                     const core::CancelToken* cancel) const {
    struct stat info{};
    if (::stat(file.c_str(), &info) != 0) {
        return failure(std::error_code{errno, std::generic_category()}.message());
    }
    const std::string uri = file_uri(file);
    const std::vector<std::uint8_t> cached = read_file(path_for(file, size));
    if (!cached.empty() && matches(cached, uri, info.st_mtime)) {
        // Our own PNGs (or another app's, the same spec): read here, with wuffs, PNG only.
        std::string error;
        if (std::optional<picture::Image> stored = picture::decode_png(cached, error)) {
            return {.image = std::move(*stored), .source = Source::cache, .error = {}};
        }
    }
    const std::vector<std::uint8_t> failed = read_file(fail_path(file, mime));
    if (!failed.empty() && matches(failed, uri, info.st_mtime)) {
        return failure("failed before, and unchanged since");
    }
    return make(file, size, info.st_mtime, mime, cancel);
}

Thumbnail Cache::make(const std::filesystem::path& file, Size size, std::string_view mime,
                      const core::CancelToken* cancel) const {
    struct stat info{};
    if (::stat(file.c_str(), &info) != 0) {
        return failure(std::error_code{errno, std::generic_category()}.message());
    }
    return make(file, size, info.st_mtime, mime, cancel);
}

Thumbnail Cache::make(const std::filesystem::path& file, Size size, std::int64_t mtime,
                      std::string_view mime, const core::CancelToken* cancel) const {
    const std::string uri = file_uri(file);
    const std::filesystem::path relative = file.lexically_relative(root_);
    if (!relative.empty() && *relative.begin() != "..") {
        return failure("a thumbnail itself");  // the spec: never thumbnail the cache
    }
    const std::uint32_t side = pixels(size);
    if (cancel != nullptr && cancel->cancelled()) {
        return failure("cancelled");
    }
    picture::Loaded loaded = picture::load_picture({.path = file,
                                                    .max_width = side,
                                                    .max_height = side,
                                                    .mime = std::string{mime},
                                                    .cancel = cancel});
    if (loaded.failure != picture::Failure::none) {
        // Remembered, so that it is not tried again until the file changes.
        const picture::Image blank{.width = 1, .height = 1, .rgba = {0, 0, 0, 0}};
        if (loaded.failure != picture::Failure::unavailable &&
            (cancel == nullptr || !cancel->cancelled())) {
            static_cast<void>(store(fail_path(file, mime), blank, uri, mtime));
        }
        return failure(std::string{loaded.error()} + " (" + loaded.detail + ")");
    }
    static_cast<void>(store(path_for(file, size), loaded.image, uri, mtime));
    return {.image = std::move(loaded.image),
            .source = loaded.from_preview ? Source::preview : Source::decoded,
            .error = {}};
}

}  // namespace rummage::thumbs
