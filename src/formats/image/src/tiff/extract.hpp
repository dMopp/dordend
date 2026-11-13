#pragma once

// One image of a TIFF structure copied out as a TIFF of its own (ROADMAP 2e): the preview IFDs of
// camera raws (DNG, NEF, 3FR, ERF) are images inside a larger file; the decoder and the JPEG-in-
// TIFF check read TIFFs, so the preview becomes one. Only the tags that describe the pixels are
// kept; the data is read from the source, chunk by chunk.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "container.hpp"

namespace rummage::formats::image::tiff {

// ifd's image as a classic TIFF in the source's byte order; empty when it cannot be copied (no
// strips or tiles, a chunk out of the data, more than max_bytes in all, a BigTIFF source).
[[nodiscard]] std::vector<std::uint8_t> extract(const Ifd& ifd,
                                                std::size_t max_bytes = std::size_t{64} << 20);

}  // namespace rummage::formats::image::tiff
