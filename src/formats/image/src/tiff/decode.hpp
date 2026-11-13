#pragma once

// TIFF images, decoded by rummage's own code (ROADMAP 2c): strips and tiles, chunky and planar,
// no compression, LZW, Deflate, PackBits, the horizontal and floating-point predictors, 1 to 16
// bit integers and 16 to 64 bit floats, grey, RGB, palette and CMYK, with or without alpha, to
// 8-bit RGBA. The first full-resolution image of the file; its Orientation tag is reported.

#include "container.hpp"
#include "rummage/formats/image/decode.hpp"

namespace rummage::formats::image::tiff {

// Whether a file built like TIFF is a camera raw (it holds a colour filter array or linear raw
// image, a 10/12/14-bit one, is a DNG, or is a Canon CR2): those show their embedded previews,
// not decoded here.
[[nodiscard]] bool is_camera_raw(const Source& source);

// The first full-resolution image of the TIFF in source. Problems land in result.error and
// result.problem (unknown_format for what is valid TIFF but not read here).
[[nodiscard]] Decoded decode(const Source& source, const DecodeOptions& options);

// The JPEG inside the first IFD of a TIFF compressed as JPEG in one strip (compression 7, or
// the old 6 when the strip is a whole JPEG): DNG previews are stored so (Leica M240's thumbnail,
// Samsung GX20's full-size preview). nullopt for anything else.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> jpeg_in_tiff(
    std::span<const std::uint8_t> data);

}  // namespace rummage::formats::image::tiff
