// Fuzzes rummage's own lossless JPEG decoder (ROADMAP 2d) alone: headers, Huffman tables, the
// entropy-coded data, predictors, restarts.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "dng/ljpeg.hpp"

namespace dng = rummage::formats::image::dng;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes{data, size};
    std::string error;
    static_cast<void>(dng::lossless_frame(bytes, error));
    std::uint64_t sum = 0;
    static_cast<void>(dng::decode_lossless(
        bytes,
        [&](std::uint32_t, std::span<const std::uint16_t> samples) {
            for (const std::uint16_t v : samples) {
                sum += v;
            }
            return true;  // the decoder refuses frames larger than 8 values a byte
        },
        error));
    return 0;
}
