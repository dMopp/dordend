#pragma once

// One function per decoding library, behind decode(). Each fills result.image (RGBA, straight
// alpha), result.orientation when the library reports EXIF, or result.error.

#include <span>

#include "rummage/formats/image/decode.hpp"

namespace rummage::formats::image::detail {

using picture::kTooLarge;

// PNG, JPEG and GIF (first frame), with wuffs.
void decode_wuffs(std::span<const std::uint8_t> data, const DecodeOptions& options,
                  Decoded& result);

// WebP, still or the first frame of an animation, with libwebp.
void decode_webp(std::span<const std::uint8_t> data, const DecodeOptions& options, Decoded& result);

// Binary PGM and PPM (P5, P6), 8 or 16 bits a sample, ours.
void decode_pnm(std::span<const std::uint8_t> data, const DecodeOptions& options, Decoded& result);

// JPEG XL, with libjxl (which applies the orientation itself).
void decode_jxl(std::span<const std::uint8_t> data, const DecodeOptions& options, Decoded& result);

}  // namespace rummage::formats::image::detail
