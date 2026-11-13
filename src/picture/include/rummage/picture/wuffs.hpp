#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rummage/picture/image.hpp"

namespace rummage::picture {

struct WuffsDecoded {
    Image image;                     // RGBA, straight alpha; empty on failure
    std::vector<std::uint8_t> exif;  // the EXIF block the file carries, raw; empty when none
    std::string error;               // why it failed; empty on success
};

// PNG, JPEG and GIF (first frame) with wuffs, which is memory-safe by construction. Pictures over
// max_pixels are refused (kTooLarge) before any pixel memory is allocated.
[[nodiscard]] WuffsDecoded decode_wuffs(std::span<const std::uint8_t> data,
                                        std::uint64_t max_pixels);

// A PNG and nothing else: the thumbnail cache's own files, read in rummage. nullopt (error says
// why) for anything that is not a PNG or does not decode.
[[nodiscard]] std::optional<Image> decode_png(std::span<const std::uint8_t> data,
                                              std::string& error);

}  // namespace rummage::picture
