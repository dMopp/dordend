// Fuzzes rummage's own DNG reader (ROADMAP 2d) from memory: the raw image's IFD, its levels and
// colour tags, the sensor data (uncompressed, packed, lossless JPEG; strips and tiles) and the
// development, to a small box so that each run stays fast.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "dng/develop.hpp"
#include "dng/raw.hpp"

namespace image = rummage::formats::image;
namespace dng = rummage::formats::image::dng;
namespace tiff = rummage::formats::image::tiff;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const tiff::Source source{std::span<const std::uint8_t>{data, size}};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    if (!container) {
        return 0;
    }
    std::string error;
    image::Problem problem{};
    if (const std::optional<dng::Raw> raw = dng::find_raw(*container, error, problem)) {
        std::uint64_t sum = 0;
        static_cast<void>(dng::read_rows(
            *raw,
            [&](std::uint32_t, std::uint32_t, std::span<const std::uint16_t> values) {
                for (const std::uint16_t v : values) {
                    sum += v;
                }
                return true;
            },
            error));
    }
    static_cast<void>(dng::develop(*container, {.look = true, .max_width = 64, .max_height = 64},
                                   error, problem));
    return 0;
}
