#pragma once

// Walks the segments of a JPEG before its image data: what rummage::image needs from them
// without decoding (EXIF, size).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace rummage::formats::image::detail {

// The EXIF block of the APP1 "Exif" segment: its TIFF data (after "Exif\0\0") and where that
// data starts in the file. nullopt when there is none before the image data.
struct JpegExif {
    std::span<const std::uint8_t> tiff;
    std::size_t offset{0};
};

[[nodiscard]] std::optional<JpegExif> jpeg_exif(std::span<const std::uint8_t> data);

// Width and height from the start-of-frame segment; nullopt when it is not found.
struct JpegSize {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

[[nodiscard]] std::optional<JpegSize> jpeg_size(std::span<const std::uint8_t> data);

// Whether data holds a JPEG from its SOI to its EOI, following the segments and the scans'
// markers (bytes after the EOI are allowed): a preview cut short by a wrong length in the file
// fails this. Reads every byte once.
[[nodiscard]] bool whole_jpeg(std::span<const std::uint8_t> data);

}  // namespace rummage::formats::image::detail
