#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rummage/picture/image.hpp"

namespace rummage::picture {

using PngText = std::vector<std::pair<std::string, std::string>>;

// The image as a PNG file (8-bit RGBA) with text chunks (tEXt: Latin-1 keys and values, as the
// freedesktop thumbnail spec uses them). Empty when the image is empty or zlib fails.
[[nodiscard]] std::vector<std::uint8_t> encode_png(const Image& image, const PngText& text = {});

// The tEXt chunks of a PNG file, in order, without decoding its pixels; empty when it is not a
// PNG. Only the chunks before the image data are read (where thumbnails keep them).
[[nodiscard]] PngText png_text(std::span<const std::uint8_t> png);

}  // namespace rummage::picture
