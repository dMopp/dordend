#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "decoders.hpp"

namespace rummage::formats::image::detail {
namespace {

bool is_space(std::uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Reads the header's numbers one by one: whitespace and "#" comments (to the end of the line)
// before each, the digits, then exactly one whitespace byte after the last (the maxval).
class HeaderReader {
public:
    explicit HeaderReader(std::span<const std::uint8_t> data) : data_{data} {}

    // A decimal number up to 2^24, after whitespace and comments; nullopt if there is none.
    std::optional<std::uint32_t> number() {
        while (at_ < data_.size() && (is_space(data_[at_]) || data_[at_] == '#')) {
            if (data_[at_] == '#') {
                while (at_ < data_.size() && data_[at_] != '\n') {
                    ++at_;
                }
            } else {
                ++at_;
            }
        }
        std::uint32_t value = 0;
        std::size_t digits = 0;
        while (at_ < data_.size() && data_[at_] >= '0' && data_[at_] <= '9') {
            value = (value * 10) + (data_[at_] - '0');
            if (value > (1U << 24)) {
                return std::nullopt;
            }
            ++at_;
            ++digits;
        }
        if (digits == 0) {
            return std::nullopt;
        }
        return value;
    }

    // The single whitespace byte that ends the header: the pixels start after it.
    std::optional<std::size_t> pixels_start() {
        if (at_ >= data_.size() || !is_space(data_[at_])) {
            return std::nullopt;
        }
        return at_ + 1;
    }

    void skip(std::size_t bytes) { at_ += bytes; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t at_{0};
};

}  // namespace

void decode_pnm(std::span<const std::uint8_t> data, const DecodeOptions& options, Decoded& result) {
    if (data.size() < 3 || data[0] != 'P' || (data[1] != '5' && data[1] != '6')) {
        result.error = "not a binary PGM or PPM";
        return;
    }
    const std::size_t channels = data[1] == '6' ? 3 : 1;
    HeaderReader header{data};
    header.skip(2);
    const std::optional<std::uint32_t> width = header.number();
    const std::optional<std::uint32_t> height = header.number();
    const std::optional<std::uint32_t> maxval = header.number();
    const std::optional<std::size_t> start = header.pixels_start();
    if (!width || !height || !maxval || !start || *width == 0 || *height == 0 || *maxval == 0 ||
        *maxval > 65535) {
        result.error = "a PGM or PPM with a broken header";
        return;
    }
    if (std::uint64_t{*width} * *height > options.max_pixels) {
        result.error = kTooLarge;
        return;
    }
    const std::size_t sample = *maxval > 255 ? 2 : 1;
    const std::size_t pixels = std::size_t{*width} * *height;
    if (data.size() - *start < pixels * channels * sample) {
        result.error = "a PGM or PPM cut short";
        return;
    }
    Image& image = result.image;
    image.width = *width;
    image.height = *height;
    image.rgba.resize(pixels * 4);
    const std::uint8_t* from = data.data() + *start;
    std::uint8_t* to = image.rgba.data();
    const std::uint32_t max = *maxval;
    // A sample scaled to 0-255, rounded; 16-bit samples are big-endian.
    const auto next = [&] {
        std::uint32_t value = from[0];
        if (sample == 2) {
            value = (value << 8) | from[1];
        }
        from += sample;
        value = std::min(value, max);
        return static_cast<std::uint8_t>(((value * 255) + (max / 2)) / max);
    };
    for (std::size_t i = 0; i < pixels; ++i, to += 4) {
        if (channels == 3) {
            to[0] = next();
            to[1] = next();
            to[2] = next();
        } else {
            to[0] = to[1] = to[2] = next();
        }
        to[3] = 255;
    }
}

}  // namespace rummage::formats::image::detail
