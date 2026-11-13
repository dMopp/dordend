#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/picture/image.hpp"

// The image formats rummage reads: decoding PNG, JPEG and GIF (wuffs), WebP (libwebp), JPEG XL
// (libjxl), PGM/PPM (ours). Runs in the isolated helper only.
namespace rummage::formats::image {

using picture::apply_orientation;
using picture::Image;

enum class Format : std::uint8_t { unknown, png, jpeg, gif, webp, jxl, pnm, tiff };

[[nodiscard]] std::string_view format_name(Format format) noexcept;

// The format from the first bytes of a file (16 are enough); unknown when none matches.
[[nodiscard]] Format sniff(std::span<const std::uint8_t> head) noexcept;

struct DecodeOptions {
    // Larger images are refused before any pixel memory is allocated (200 MP: 800 MB of RGBA).
    std::uint64_t max_pixels{200'000'000};
    // Turn the pixels as the EXIF orientation says, so that the image stands as it was shot.
    bool apply_orientation{true};
};

// What kind of problem a failed decode met.
enum class Problem : std::uint8_t { none, unknown_format, too_large, damaged, unreadable };

struct Decoded {
    Image image;  // empty on failure
    Format format{Format::unknown};
    std::uint8_t orientation{1};  // EXIF orientation found in the file, 1-8 (1: as stored)
    std::string error;            // why it failed, in the decoder's words; empty on success
    Problem problem{Problem::none};
};

// Decodes a PNG, JPEG, GIF (first frame), WebP (first frame), JPEG XL, PGM, PPM or TIFF image
// (its first full-resolution one). Never
// throws for bad input: problems land in error. Thread-safe: call it from workers.
[[nodiscard]] Decoded decode(std::span<const std::uint8_t> data, const DecodeOptions& options = {});

// Reads the file and decodes it.
[[nodiscard]] Decoded decode_file(const std::filesystem::path& path,
                                  const DecodeOptions& options = {});

// The EXIF orientation (1-8) in a TIFF-structured EXIF block, with or without the "Exif\0\0"
// prefix of a JPEG APP1 segment; 1 when absent or unreadable.
[[nodiscard]] std::uint8_t exif_orientation(std::span<const std::uint8_t> exif) noexcept;

}  // namespace rummage::formats::image
