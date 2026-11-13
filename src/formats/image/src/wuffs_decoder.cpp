#include <utility>

#include "decoders.hpp"
#include "rummage/picture/wuffs.hpp"

namespace rummage::formats::image::detail {

// PNG, JPEG and GIF through picture's wuffs; the EXIF orientation read from the block it found.
void decode_wuffs(std::span<const std::uint8_t> data, const DecodeOptions& options,
                  Decoded& result) {
    picture::WuffsDecoded decoded = picture::decode_wuffs(data, options.max_pixels);
    if (!decoded.error.empty()) {
        result.error = std::move(decoded.error);
        return;
    }
    result.image = std::move(decoded.image);
    if (!decoded.exif.empty()) {
        result.orientation = exif_orientation(decoded.exif);
    }
}

}  // namespace rummage::formats::image::detail
