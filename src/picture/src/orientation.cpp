#include <algorithm>
#include <cstddef>
#include <cstring>

#include "rummage/picture/image.hpp"

namespace rummage::picture {
namespace {

constexpr std::ptrdiff_t kTile = 32;  // apply_orientation works in kTile x kTile blocks

}  // namespace

Image apply_orientation(Image image, std::uint8_t orientation) {
    if (orientation < 2 || orientation > 8 || image.empty()) {
        return image;
    }
    const auto w = static_cast<std::ptrdiff_t>(image.width);
    const auto h = static_cast<std::ptrdiff_t>(image.height);
    // The input pixel of output pixel (x, y) is start + x * step_x + y * step_y.
    std::ptrdiff_t start = 0;
    std::ptrdiff_t step_x = 1;
    std::ptrdiff_t step_y = w;
    switch (orientation) {
        case 2:  // mirrored left to right
            start = w - 1;
            step_x = -1;
            break;
        case 3:  // upside down
            start = (h * w) - 1;
            step_x = -1;
            step_y = -w;
            break;
        case 4:  // mirrored top to bottom
            start = (h - 1) * w;
            step_y = -w;
            break;
        case 5:  // transposed
            step_x = w;
            step_y = 1;
            break;
        case 6:  // turned a quarter clockwise
            start = (h - 1) * w;
            step_x = -w;
            step_y = 1;
            break;
        case 7:  // transversed
            start = (h * w) - 1;
            step_x = -w;
            step_y = -1;
            break;
        default:  // 8: turned a quarter counterclockwise
            start = w - 1;
            step_x = w;
            step_y = -1;
            break;
    }
    const bool swap = orientation >= 5;  // 5-8 turn by a quarter: width and height swap
    Image out;
    out.width = swap ? image.height : image.width;
    out.height = swap ? image.width : image.height;
    out.rgba.resize(image.rgba.size());
    const auto out_w = static_cast<std::ptrdiff_t>(out.width);
    const auto out_h = static_cast<std::ptrdiff_t>(out.height);
    const std::uint8_t* const in = image.rgba.data();
    std::uint8_t* const to = out.rgba.data();
    // In tiles, so that quarter turns read and write within a few cache lines.
    for (std::ptrdiff_t tile_y = 0; tile_y < out_h; tile_y += kTile) {
        for (std::ptrdiff_t tile_x = 0; tile_x < out_w; tile_x += kTile) {
            const std::ptrdiff_t end_y = std::min(tile_y + kTile, out_h);
            const std::ptrdiff_t end_x = std::min(tile_x + kTile, out_w);
            for (std::ptrdiff_t y = tile_y; y < end_y; ++y) {
                std::ptrdiff_t from = start + (tile_x * step_x) + (y * step_y);
                for (std::ptrdiff_t x = tile_x; x < end_x; ++x, from += step_x) {
                    std::memcpy(to + (((y * out_w) + x) * 4), in + (from * 4), 4);
                }
            }
        }
    }
    return out;
}

}  // namespace rummage::picture
