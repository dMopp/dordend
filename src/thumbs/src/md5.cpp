#include "rummage/thumbs/md5.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace rummage::thumbs {
namespace {

// Per-round shift amounts and the sines table of RFC 1321.
constexpr std::array<std::uint32_t, 64> kShift{
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
    14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

constexpr std::array<std::uint32_t, 64> kSines{
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr std::string_view kHex = "0123456789abcdef";

std::uint32_t load_le(const std::uint8_t* bytes) {
    return std::uint32_t{bytes[0]} | (std::uint32_t{bytes[1]} << 8) |
           (std::uint32_t{bytes[2]} << 16) | (std::uint32_t{bytes[3]} << 24);
}

}  // namespace

std::string md5_hex(std::string_view data) {
    // The message, a 1 bit, zeros up to 56 mod 64 bytes, then its length in bits (little endian).
    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    for (int i = 0; i < 8; ++i) {
        message.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }

    std::array<std::uint32_t, 4> state{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::array<std::uint32_t, 16> words{};
        for (std::size_t i = 0; i < words.size(); ++i) {
            words.at(i) = load_le(&message[block + (i * 4)]);
        }
        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = ((5 * i) + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = ((3 * i) + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const std::uint32_t next = d;
            d = c;
            c = b;
            b += std::rotl(a + f + kSines.at(i) + words.at(g), static_cast<int>(kShift.at(i)));
            a = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }

    std::string hex;
    hex.reserve(32);
    for (const std::uint32_t word : state) {
        for (int i = 0; i < 4; ++i) {
            const auto byte = static_cast<std::uint8_t>(word >> (8 * i));
            hex.push_back(kHex[byte >> 4]);
            hex.push_back(kHex[byte & 0x0F]);
        }
    }
    return hex;
}

}  // namespace rummage::thumbs
