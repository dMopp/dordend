// Fuzzes rummage's own TIFF reader (ROADMAP 2c) from memory: the container, the codecs, the
// predictors and the conversions. Pictures are capped at 4 MP so that each run stays fast.
#include <cstddef>
#include <cstdint>
#include <span>

#include "tiff/decode.hpp"

namespace tiff = rummage::formats::image::tiff;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes{data, size};
    const tiff::Source source{bytes};
    static_cast<void>(tiff::is_camera_raw(source));
    static_cast<void>(tiff::decode(source, {.max_pixels = 4'000'000, .apply_orientation = true}));
    static_cast<void>(tiff::jpeg_in_tiff(bytes));
    return 0;
}
