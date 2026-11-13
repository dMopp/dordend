#include "codecs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <zlib.h>

namespace rummage::formats::image::tiff {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the TIFF reader writes samples in little-endian order");

std::optional<std::vector<std::uint8_t>> packbits(std::span<const std::uint8_t> in,
                                                  std::size_t expected) {
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    std::size_t at = 0;
    while (at < in.size() && out.size() < expected) {
        const auto control = static_cast<std::int8_t>(in[at++]);
        if (control >= 0) {
            // 1 + control literal bytes.
            const std::size_t count =
                std::min<std::size_t>(static_cast<std::size_t>(control) + 1, expected - out.size());
            if (in.size() - at < count) {
                return std::nullopt;
            }
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(at),
                       in.begin() + static_cast<std::ptrdiff_t>(at + count));
            at += static_cast<std::size_t>(control) + 1;
        } else if (control != -128) {
            // The next byte, 1 - control times.
            if (at >= in.size()) {
                return std::nullopt;
            }
            const std::size_t count =
                std::min<std::size_t>(static_cast<std::size_t>(1 - control), expected - out.size());
            out.insert(out.end(), count, in[at++]);
        }
    }
    out.resize(expected, 0);
    return out;
}

// TIFF's LZW: codes of 9 to 12 bits, most significant bit first, Clear 256, EndOfInformation
// 257, the code width growing one code early ("early change").
std::optional<std::vector<std::uint8_t>> lzw(std::span<const std::uint8_t> in,
                                             std::size_t expected) {
    constexpr std::uint32_t clear_code = 256;
    constexpr std::uint32_t end_code = 257;
    constexpr std::uint32_t first_code = 258;
    constexpr std::uint32_t codes = 4096;
    // The old, incompatible LZW of libtiff before 1991 starts with 0x00 0x01: not read.
    if (in.size() >= 2 && in[0] == 0x00 && (in[1] & 0x01) != 0) {
        return std::nullopt;
    }
    std::array<std::uint16_t, codes> prefix{};
    std::array<std::uint8_t, codes> suffix{};
    std::array<std::uint8_t, codes> first{};
    std::array<std::uint16_t, codes> length{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        suffix.at(i) = static_cast<std::uint8_t>(i);
        first.at(i) = static_cast<std::uint8_t>(i);
        length.at(i) = 1;
    }
    std::vector<std::uint8_t> out;
    out.reserve(expected);
    std::uint32_t next = first_code;
    std::uint32_t width = 9;
    std::uint32_t previous = codes;  // none
    std::uint64_t bits = 0;
    std::uint32_t held = 0;
    std::size_t at = 0;
    // Writes code's string at the end of out (cut at expected).
    const auto emit = [&](std::uint32_t code) {
        const std::size_t size = length.at(code);
        const std::size_t start = out.size();
        const std::size_t room = expected - start;
        out.resize(start + std::min(size, room));
        std::uint32_t c = code;
        for (std::size_t i = size; i > 0; --i) {
            if (i - 1 < room) {
                out[start + i - 1] = suffix.at(c);
            }
            c = prefix.at(c);
        }
    };
    while (out.size() < expected) {
        while (held < width) {
            if (at >= in.size()) {
                out.resize(expected, 0);  // ends early
                return out;
            }
            bits = (bits << 8) | in[at++];
            held += 8;
        }
        const auto code =
            static_cast<std::uint32_t>((bits >> (held - width)) & ((1U << width) - 1));
        held -= width;
        if (code == end_code) {
            break;
        }
        if (code == clear_code) {
            next = first_code;
            width = 9;
            previous = codes;
            continue;
        }
        if (previous == codes) {
            if (code >= 256) {
                return std::nullopt;  // after a Clear, a literal must come
            }
            emit(code);
            previous = code;
            continue;
        }
        if (code > next || next >= codes) {
            return std::nullopt;
        }
        const std::uint8_t head = code == next ? first.at(previous) : first.at(code);
        // The new entry: the previous string and the first byte of this one.
        prefix.at(next) = static_cast<std::uint16_t>(previous);
        suffix.at(next) = head;
        first.at(next) = first.at(previous);
        length.at(next) = static_cast<std::uint16_t>(length.at(previous) + 1);
        emit(code);
        ++next;
        if (next + 1 >= (1U << width) && width < 12) {
            ++width;
        }
        previous = code;
    }
    out.resize(expected, 0);
    return out;
}

std::optional<std::vector<std::uint8_t>> deflate(std::span<const std::uint8_t> in,
                                                 std::size_t expected) {
    std::vector<std::uint8_t> out(expected);
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return std::nullopt;
    }
    stream.next_in = const_cast<Bytef*>(in.data());  // NOLINT: zlib's API is not const
    stream.avail_in = static_cast<uInt>(std::min<std::size_t>(in.size(), UINT32_MAX));
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(std::min<std::size_t>(out.size(), UINT32_MAX));
    int status = Z_OK;
    while (status == Z_OK && stream.avail_out > 0) {
        status = inflate(&stream, Z_NO_FLUSH);
    }
    inflateEnd(&stream);
    if (status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) {
        return std::nullopt;  // the data itself is broken
    }
    // Z_BUF_ERROR: the input ended early, the rest stays zero.
    return out;
}

}  // namespace

bool supported_compression(std::uint64_t compression) noexcept {
    return compression == kNone || compression == kLzw || compression == kDeflate ||
           compression == kDeflateOld || compression == kPackBits;
}

std::optional<std::vector<std::uint8_t>> unpack(std::uint64_t compression,
                                                std::span<const std::uint8_t> in,
                                                std::size_t expected) {
    switch (compression) {
        case kNone: {
            std::vector<std::uint8_t> out(
                in.begin(),
                in.begin() + static_cast<std::ptrdiff_t>(std::min(in.size(), expected)));
            out.resize(expected, 0);
            return out;
        }
        case kLzw:
            return lzw(in, expected);
        case kDeflate:
        case kDeflateOld:
            return deflate(in, expected);
        case kPackBits:
            return packbits(in, expected);
        default:
            return std::nullopt;
    }
}

void undo_horizontal(std::span<std::uint8_t> data, std::size_t row_samples, std::size_t stride,
                     std::size_t bytes_per_sample) {
    const std::size_t row_bytes = row_samples * bytes_per_sample;
    if (row_bytes == 0 || stride == 0) {
        return;
    }
    for (std::size_t row = 0; row + row_bytes <= data.size(); row += row_bytes) {
        std::uint8_t* p = data.data() + row;
        for (std::size_t i = stride; i < row_samples; ++i) {
            switch (bytes_per_sample) {
                case 1:
                    p[i] = static_cast<std::uint8_t>(p[i] + p[i - stride]);
                    break;
                case 2: {
                    std::uint16_t a = 0;
                    std::uint16_t b = 0;
                    std::memcpy(&a, p + (i * 2), 2);
                    std::memcpy(&b, p + ((i - stride) * 2), 2);
                    a = static_cast<std::uint16_t>(a + b);
                    std::memcpy(p + (i * 2), &a, 2);
                    break;
                }
                case 4: {
                    std::uint32_t a = 0;
                    std::uint32_t b = 0;
                    std::memcpy(&a, p + (i * 4), 4);
                    std::memcpy(&b, p + ((i - stride) * 4), 4);
                    a += b;
                    std::memcpy(p + (i * 4), &a, 4);
                    break;
                }
                default: {
                    std::uint64_t a = 0;
                    std::uint64_t b = 0;
                    std::memcpy(&a, p + (i * 8), 8);
                    std::memcpy(&b, p + ((i - stride) * 8), 8);
                    a += b;
                    std::memcpy(p + (i * 8), &a, 8);
                    break;
                }
            }
        }
    }
}

void undo_floating_point(std::span<std::uint8_t> data, std::size_t row_samples, std::size_t stride,
                         std::size_t bytes_per_sample) {
    const std::size_t row_bytes = row_samples * bytes_per_sample;
    if (row_bytes == 0 || stride == 0) {
        return;
    }
    std::vector<std::uint8_t> planes(row_bytes);
    for (std::size_t row = 0; row + row_bytes <= data.size(); row += row_bytes) {
        std::uint8_t* p = data.data() + row;
        // The bytes were differenced as one row of row_bytes, stride apart...
        for (std::size_t i = stride; i < row_bytes; ++i) {
            p[i] = static_cast<std::uint8_t>(p[i] + p[i - stride]);
        }
        // ...after splitting each sample into byte planes, most significant first.
        std::memcpy(planes.data(), p, row_bytes);
        for (std::size_t sample = 0; sample < row_samples; ++sample) {
            for (std::size_t byte = 0; byte < bytes_per_sample; ++byte) {
                p[(sample * bytes_per_sample) + byte] =
                    planes[((bytes_per_sample - byte - 1) * row_samples) + sample];
            }
        }
    }
}

}  // namespace rummage::formats::image::tiff
