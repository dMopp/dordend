#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "rummage/picture/image.hpp"
#include "rummage/picture/photo_info.hpp"

// How rummage asks for a picture: what it asks (LoadRequest), what comes back (Loaded), why it
// may fail (Failure), and where the asking goes (the loader: in rummage, the isolated helpers).
// Making the picture is the formats' job (formats/), never done here.
namespace rummage::core {
class CancelToken;
}

namespace rummage::picture {

// Why a file gave no picture.
enum class Failure : std::uint8_t {
    none,
    unknown_format,       // not an image format rummage reads
    no_readable_preview,  // a camera raw file without a preview rummage can read
    too_large,            // more pixels than DecodeOptions::max_pixels
    damaged,              // the format is known but the data is broken or cut short
    unreadable,           // the file could not be read (permissions, I/O)
    unavailable,          // the isolated helper stopped, took too long, or could not start
};

// The sentence shown for a failure; empty for none.
[[nodiscard]] std::string_view failure_message(Failure failure) noexcept;

struct LoadRequest {
    std::filesystem::path path;
    // The picture fits in max_width x max_height pixels (scaled down, never up).
    std::uint32_t max_width{0};
    std::uint32_t max_height{0};
    std::string mime{};  // Empty retains the default image engine for existing callers.
    const core::CancelToken* cancel{nullptr};  // Valid for the blocking load call.
};

struct Loaded {
    Image image;  // upright, fits in the asked size; empty on failure
    // The size of the whole image, upright, when known (0 when only a preview was read and the
    // file does not say).
    std::uint32_t full_width{0};
    std::uint32_t full_height{0};
    bool from_preview{false};  // made from a preview stored in the file, not the whole image
    Failure failure{Failure::none};
    std::string detail;  // the technical reason of a failure, for the log

    [[nodiscard]] std::string_view error() const noexcept { return failure_message(failure); }
};

// Where rummage's pictures are made. rummage sets this once at startup to its isolated helpers
// (ROADMAP 2b); tests and development tools set the formats' own load, in their process.
using Loader = std::function<Loaded(const LoadRequest&)>;
void set_loader(Loader loader);

// A picture for thumbnails, Quick Look and the info pane, through the loader set; "unavailable"
// when none is. Thread-safe.
[[nodiscard]] Loaded load_picture(const LoadRequest& request);

// Where a photo's information is read (the info pane): set like the loader. rummage: its
// isolated helpers; tests and tools: the image family in their process.
using InfoLoader = std::function<PhotoInfo(const std::filesystem::path&)>;
void set_info_loader(InfoLoader loader);

// What path says about itself (EXIF), through the information loader;
// empty when none is set or the file says nothing. Thread-safe.
[[nodiscard]] PhotoInfo load_photo_info(const std::filesystem::path& path);

}  // namespace rummage::picture
