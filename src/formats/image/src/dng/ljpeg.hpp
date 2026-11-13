#pragma once

// Lossless JPEG (ITU T.81 process 14, the SOF3 frame), as DNG and many raw formats store sensor
// data (ROADMAP 2d): 2 to 16-bit samples, 1 to 4 components each sampled 1x1, one interleaved
// scan, Huffman coding, predictors 1 to 7, the point transform, restart intervals of whole
// rows. Every length, table and code is checked; data that ends early decodes as zeros, but a
// stream shorter than a bit a value is refused (the work stays in proportion to the data).

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace rummage::formats::image::dng {

struct LosslessFrame {
    std::uint32_t width{0};   // samples per line, of each component
    std::uint32_t height{0};  // lines
    std::uint32_t components{1};
    std::uint32_t precision{16};  // bits a sample
};

// The frame of the lossless JPEG in data, read from its headers; nullopt with the reason in error
// when it is not one this decoder reads.
[[nodiscard]] std::optional<LosslessFrame> lossless_frame(std::span<const std::uint8_t> data,
                                                          std::string& error);

// Decodes data line by line, top to bottom: line(y, samples) gets width x components samples,
// the components of each pixel side by side; it returns false to stop. False with the reason in
// error when data is not a lossless JPEG this decoder reads (lines already given stay given).
[[nodiscard]] bool decode_lossless(
    std::span<const std::uint8_t> data,
    const std::function<bool(std::uint32_t y, std::span<const std::uint16_t> samples)>& line,
    std::string& error);

}  // namespace rummage::formats::image::dng
