#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "rummage/formats/image/decode.hpp"

namespace rummage::formats::image {

// The part of a preview that shows the photo, when the camera stores more (Panasonic GX8: a 4:3
// preview for every aspect ratio). In the preview's stored pixels.
struct PreviewCrop {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

// An image that a camera file carries next to its main image: the EXIF thumbnail (160x120), the
// maker-note preview of Olympus / OM System JPEGs (1600x1200) and raw files (3200x2400), the
// previews other raw formats keep (read by rummage's own code: camera/previews.hpp), and, when a
// file has no JPEG one large enough, a small uncompressed TIFF (Hasselblad 3FR). Decoding one is
// a fraction of decoding the photo.
struct EmbeddedPreview {
    std::vector<std::uint8_t> data;  // a JPEG or a TIFF; empty when there is none
    bool tiff{false};
    // Its size as shown: after the crop, before the orientation.
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::optional<PreviewCrop> crop;
    std::uint8_t orientation{1};  // the main image's EXIF orientation, which the preview shares
    // The main image's size as stored (before the orientation), when the file says; else 0.
    std::uint32_t full_width{0};
    std::uint32_t full_height{0};
    std::string error;  // the file could not be read; empty when it simply has none
};

// The smallest embedded preview whose long side is at least min_long_side, else the largest
// one. Reads only the metadata and the preview's bytes. Thread-safe.
[[nodiscard]] EmbeddedPreview embedded_preview(const std::filesystem::path& path,
                                               std::uint32_t min_long_side);

// The same from a file open for reading as fd (kept open, the caller's).
[[nodiscard]] EmbeddedPreview embedded_preview(int fd, std::uint32_t min_long_side);

// The preview's pixels, cropped, as stored: the orientation is not applied (it is in
// preview.orientation, and turning is cheaper after scaling).
[[nodiscard]] Decoded decode_preview(const EmbeddedPreview& preview);

}  // namespace rummage::formats::image
