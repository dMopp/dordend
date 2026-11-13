#include "rummage/picture/png.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <zlib.h>

namespace rummage::picture {
namespace {

constexpr std::array<std::uint8_t, 8> kSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t get_u32(std::span<const std::uint8_t> data, std::size_t at) {
    return (std::uint32_t{data[at]} << 24) | (std::uint32_t{data[at + 1]} << 16) |
           (std::uint32_t{data[at + 2]} << 8) | std::uint32_t{data[at + 3]};
}

void put_chunk(std::vector<std::uint8_t>& out, std::string_view type,
               std::span<const std::uint8_t> data) {
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    const auto crc = crc32(0, &out[start], static_cast<uInt>(out.size() - start));
    put_u32(out, static_cast<std::uint32_t>(crc));
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return static_cast<std::uint8_t>(a);
    }
    return static_cast<std::uint8_t>(pb <= pc ? b : c);
}

// Rows prefixed by their filter: of None, Sub, Up and Paeth, the one whose bytes, taken as
// signed, sum smallest (libpng's heuristic).
std::vector<std::uint8_t> filter_rows(const Image& image) {
    const std::size_t stride = std::size_t{image.width} * 4;
    std::vector<std::uint8_t> out;
    out.reserve((stride + 1) * image.height);
    std::array<std::vector<std::uint8_t>, 4> candidates;
    for (auto& candidate : candidates) {
        candidate.resize(stride);
    }
    for (std::size_t y = 0; y < image.height; ++y) {
        const std::uint8_t* const row = &image.rgba[y * stride];
        const std::uint8_t* const above = y > 0 ? row - stride : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int left = i >= 4 ? row[i - 4] : 0;
            const int up = above != nullptr ? above[i] : 0;
            const int corner = (above != nullptr && i >= 4) ? above[i - 4] : 0;
            candidates[0][i] = row[i];
            candidates[1][i] = static_cast<std::uint8_t>(row[i] - left);
            candidates[2][i] = static_cast<std::uint8_t>(row[i] - up);
            candidates[3][i] = static_cast<std::uint8_t>(row[i] - paeth(left, up, corner));
        }
        std::size_t best = 0;
        std::uint64_t best_sum = UINT64_MAX;
        const std::array<std::uint8_t, 4> types{0, 1, 2, 4};  // None, Sub, Up, Paeth
        for (std::size_t f = 0; f < candidates.size(); ++f) {
            std::uint64_t sum = 0;
            for (const std::uint8_t byte : candidates.at(f)) {
                sum += static_cast<std::uint64_t>(std::abs(static_cast<std::int8_t>(byte)));
            }
            if (sum < best_sum) {
                best_sum = sum;
                best = f;
            }
        }
        out.push_back(types.at(best));
        out.insert(out.end(), candidates.at(best).begin(), candidates.at(best).end());
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> encode_png(const Image& image, const PngText& text) {
    if (image.empty()) {
        return {};
    }
    const std::vector<std::uint8_t> raw = filter_rows(image);
    std::vector<std::uint8_t> compressed(compressBound(static_cast<uLong>(raw.size())));
    auto compressed_size = static_cast<uLongf>(compressed.size());
    // Level 1: about 5 times faster than the default 6 on thumbnails, for files about 13 % larger.
    if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()),
                  1) != Z_OK) {
        return {};
    }
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> out(kSignature.begin(), kSignature.end());
    std::vector<std::uint8_t> header;
    put_u32(header, image.width);
    put_u32(header, image.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no interlace
    put_chunk(out, "IHDR", header);
    for (const auto& [key, value] : text) {
        std::vector<std::uint8_t> chunk(key.begin(), key.end());
        chunk.push_back(0);
        chunk.insert(chunk.end(), value.begin(), value.end());
        put_chunk(out, "tEXt", chunk);
    }
    put_chunk(out, "IDAT", compressed);
    put_chunk(out, "IEND", {});
    return out;
}

PngText png_text(std::span<const std::uint8_t> png) {
    PngText text;
    if (png.size() < kSignature.size() ||
        !std::equal(kSignature.begin(), kSignature.end(), png.begin())) {
        return text;
    }
    std::size_t at = kSignature.size();
    while (at + 12 <= png.size()) {
        const std::uint32_t length = get_u32(png, at);
        if (length > png.size() - at - 12) {
            break;
        }
        const std::string_view type{reinterpret_cast<const char*>(&png[at + 4]),  // NOLINT
                                    4};
        if (type == "IDAT" || type == "IEND") {
            break;
        }
        if (type == "tEXt") {
            const std::span<const std::uint8_t> data = png.subspan(at + 8, length);
            const auto zero = std::ranges::find(data, std::uint8_t{0});
            if (zero != data.end()) {
                text.emplace_back(std::string(data.begin(), zero),
                                  std::string(zero + 1, data.end()));
            }
        }
        at += std::size_t{length} + 12;
    }
    return text;
}

}  // namespace rummage::picture
