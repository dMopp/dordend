#pragma once

// What rummage does with a picture, wherever it comes from (a photo, later a video frame or a
// PDF page): the pixels, turned and scaled for the screen. No parser of outside files here:
// those live in formats/, inside the isolated helper.

#include <cstdint>
#include <vector>

namespace rummage::picture {

// Decoded pixels: RGBA, 8 bits per channel, straight (not premultiplied) alpha, rows top to
// bottom with no padding.
struct Image {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
};

// The error every decoder gives for a picture over its pixel limit.
inline constexpr const char* kTooLarge = "image too large";

// The image turned as EXIF orientation says (2-8 mirror and/or rotate; 1 or anything else
// returns it as it is).
[[nodiscard]] Image apply_orientation(Image image, std::uint8_t orientation);

}  // namespace rummage::picture
