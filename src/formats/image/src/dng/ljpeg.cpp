#include "ljpeg.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace rummage::formats::image::dng {
namespace {

constexpr std::uint8_t kSof3 = 0xC3;
constexpr std::uint8_t kDht = 0xC4;
constexpr std::uint8_t kSoi = 0xD8;
constexpr std::uint8_t kEoi = 0xD9;
constexpr std::uint8_t kSos = 0xDA;
constexpr std::uint8_t kDri = 0xDD;
constexpr std::uint32_t kMaxComponents = 4;
// A line of 2^20 samples (a 2^18-pixel strip of 4 components) is far beyond any sensor.
constexpr std::uint64_t kMaxLine = std::uint64_t{1} << 20;

// A Huffman table: a lookup on the next 11 bits for the short codes (most of them; 12 kB, built
// quickly for each of a DNG's tiles), the canonical codes for the long ones.
constexpr std::uint32_t kFastBits = 11;

struct Lookup {
    // (code length << 8) | symbol, 0 for a code longer than 11 bits (or none).
    std::array<std::uint16_t, std::size_t{1} << kFastBits> fast{};
    // When a code and its extra bits fit in those 11 bits: (difference << 8) | bits taken; else 0.
    std::array<std::int32_t, std::size_t{1} << kFastBits> whole{};
    // Codes of each length: the largest (-1 for none), and what to add to a code for the index
    // of its symbol.
    std::array<std::int32_t, 17> max_code{};
    std::array<std::int32_t, 17> offset{};
    std::array<std::uint8_t, 256> symbols{};
};

// The headers up to the first scan.
struct Headers {
    LosslessFrame frame;
    std::array<std::uint8_t, kMaxComponents> table{};  // Huffman table of each component
    std::uint32_t predictor{1};
    std::uint32_t point_transform{0};
    std::uint32_t restart_interval{0};               // in pixels; 0: none
    std::size_t scan{0};                             // where the entropy-coded data starts
    std::array<std::unique_ptr<Lookup>, 4> lookups;  // on the heap: 12 kB each
};

std::uint32_t be16(std::span<const std::uint8_t> data, std::size_t at) {
    return (std::uint32_t{data[at]} << 8) | data[at + 1];
}

// Builds a lookup from a DHT's 16 counts and its symbols; false when the codes overflow.
bool build(std::span<const std::uint8_t> counts, std::span<const std::uint8_t> symbols,
           Lookup& lookup) {
    lookup.fast.fill(0);
    lookup.whole.fill(0);
    lookup.max_code.fill(-1);
    std::ranges::copy(symbols, lookup.symbols.begin());
    std::uint32_t code = 0;
    std::size_t next = 0;
    for (std::uint32_t length = 1; length <= 16; ++length) {
        lookup.offset.at(length) =
            static_cast<std::int32_t>(next) - static_cast<std::int32_t>(code);
        if (counts[length - 1] != 0) {
            lookup.max_code.at(length) = static_cast<std::int32_t>(code + counts[length - 1]) - 1;
        }
        for (std::uint32_t i = 0; i < counts[length - 1]; ++i) {
            if (code >= (1U << length)) {
                return false;
            }
            const std::uint32_t shift = 16 - length;
            const std::uint32_t first = code << shift;
            const std::uint32_t last = (code + 1) << shift;
            const auto entry = static_cast<std::uint16_t>((length << 8) | symbols[next++]);
            if (length <= kFastBits) {
                const std::uint32_t size = symbols[next - 1];
                for (std::uint32_t at = first >> (16 - kFastBits); at < last >> (16 - kFastBits);
                     ++at) {
                    lookup.fast.at(at) = entry;
                    if (size < 16 && length + size <= kFastBits) {
                        // The extra bits follow the code within at.
                        const auto value = static_cast<std::int32_t>(
                            (at >> (kFastBits - length - size)) & ((1U << size) - 1U));
                        std::int32_t difference = 0;
                        if (size != 0) {
                            difference =
                                value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
                        }
                        lookup.whole.at(at) =
                            (difference * 256) + static_cast<std::int32_t>(length + size);
                    }
                }
            }
            ++code;
        }
        code <<= 1;
    }
    return true;
}

std::optional<Headers> read_headers(std::span<const std::uint8_t> data, std::string& error) {
    if (data.size() < 4 || data[0] != 0xFF || data[1] != kSoi) {
        error = "not a JPEG";
        return std::nullopt;
    }
    Headers h;
    bool have_frame = false;
    std::size_t at = 2;
    std::array<std::uint8_t, kMaxComponents> ids{};
    while (true) {
        // Fill bytes (0xFF) may come before a marker.
        while (at < data.size() && data[at] == 0xFF && at + 1 < data.size() &&
               data[at + 1] == 0xFF) {
            ++at;
        }
        if (at + 4 > data.size() || data[at] != 0xFF) {
            error = "a lossless JPEG cut short in its headers";
            return std::nullopt;
        }
        const std::uint8_t marker = data[at + 1];
        if (marker == kEoi) {
            error = "a lossless JPEG without a scan";
            return std::nullopt;
        }
        const std::uint32_t length = be16(data, at + 2);
        if (length < 2 || at + 2 + length > data.size()) {
            error = "a lossless JPEG with a broken segment";
            return std::nullopt;
        }
        const std::span<const std::uint8_t> segment = data.subspan(at + 4, length - 2);
        at += 2 + length;
        if (marker == kDht) {
            std::size_t p = 0;
            while (p < segment.size()) {
                if (p + 17 > segment.size()) {
                    error = "a lossless JPEG with a broken Huffman table";
                    return std::nullopt;
                }
                const std::uint32_t id = segment[p] & 0x0FU;
                if ((segment[p] >> 4) != 0 || id > 3) {
                    error = "a lossless JPEG with an AC Huffman table";
                    return std::nullopt;
                }
                const std::span<const std::uint8_t> counts = segment.subspan(p + 1, 16);
                std::size_t total = 0;
                for (const std::uint8_t count : counts) {
                    total += count;
                }
                if (total > 256 || p + 17 + total > segment.size()) {
                    error = "a lossless JPEG with a broken Huffman table";
                    return std::nullopt;
                }
                h.lookups.at(id) = std::make_unique<Lookup>();
                if (!build(counts, segment.subspan(p + 17, total), *h.lookups.at(id))) {
                    error = "a lossless JPEG with impossible Huffman codes";
                    return std::nullopt;
                }
                p += 17 + total;
            }
        } else if (marker == kSof3) {
            if (segment.size() < 6) {
                error = "a lossless JPEG with a broken frame";
                return std::nullopt;
            }
            h.frame.precision = segment[0];
            h.frame.height = be16(segment, 1);
            h.frame.width = be16(segment, 3);
            h.frame.components = segment[5];
            if (h.frame.precision < 2 || h.frame.precision > 16 || h.frame.width == 0 ||
                h.frame.height == 0 || h.frame.components == 0 ||
                h.frame.components > kMaxComponents ||
                segment.size() < 6 + (std::size_t{h.frame.components} * 3) ||
                std::uint64_t{h.frame.width} * h.frame.components > kMaxLine) {
                error = "a lossless JPEG frame of a kind not read here";
                return std::nullopt;
            }
            for (std::uint32_t c = 0; c < h.frame.components; ++c) {
                ids.at(c) = segment[6 + (c * 3)];
                if (segment[7 + (c * 3)] != 0x11) {
                    error = "a lossless JPEG with subsampled components";
                    return std::nullopt;
                }
            }
            have_frame = true;
        } else if (marker == kDri) {
            if (segment.size() < 2) {
                error = "a lossless JPEG with a broken restart interval";
                return std::nullopt;
            }
            h.restart_interval = be16(segment, 0);
        } else if (marker == kSos) {
            if (!have_frame || segment.empty() || segment[0] != h.frame.components ||
                segment.size() < 1 + (std::size_t{segment[0]} * 2) + 3) {
                error = "a lossless JPEG whose scan is not one of all its components";
                return std::nullopt;
            }
            for (std::uint32_t c = 0; c < h.frame.components; ++c) {
                if (segment[1 + (c * 2)] != ids.at(c)) {
                    error = "a lossless JPEG whose scan is not one of all its components";
                    return std::nullopt;
                }
                h.table.at(c) = static_cast<std::uint8_t>(segment[2 + (c * 2)] >> 4);
                if (h.table.at(c) > 3 || h.lookups.at(h.table.at(c)) == nullptr) {
                    error = "a lossless JPEG without the Huffman table it uses";
                    return std::nullopt;
                }
            }
            const std::size_t rest = 1 + (std::size_t{h.frame.components} * 2);
            h.predictor = segment[rest];
            h.point_transform = segment[rest + 2] & 0x0FU;
            if (h.predictor < 1 || h.predictor > 7 || h.point_transform >= h.frame.precision) {
                error = "a lossless JPEG with an unknown predictor";
                return std::nullopt;
            }
            if (h.restart_interval != 0 && h.restart_interval % h.frame.width != 0) {
                error = "a lossless JPEG restarting inside lines";
                return std::nullopt;
            }
            h.scan = at;
            return h;
        } else if ((marker >= 0xC0 && marker <= 0xCF && marker != kDht && marker != 0xC8 &&
                    marker != 0xCC)) {
            error = "a JPEG that is not lossless";
            return std::nullopt;
        }
    }
}

// The entropy-coded bits: 0xFF 0x00 is a 0xFF byte; any other marker ends the data, after which
// (as after the end of the data) only zeros come.
class Bits {
public:
    Bits(std::span<const std::uint8_t> data, std::size_t at) : data_{data}, at_{at} {}

    // One difference: a Huffman code of lookup, then its extra bits. False for a code that is not
    // in the table.
    bool difference(const Lookup& lookup, std::int32_t& out) {
        if (count_ < 32) {  // a code and its extra bits are at most 32 bits
            fill();
        }
        const auto top = static_cast<std::size_t>(buffer_ >> (64 - kFastBits));
        const std::int32_t whole = lookup.whole[top];  // NOLINT: top < 2^kFastBits
        if (whole != 0) {
            const auto taken = static_cast<std::uint32_t>(whole & 0xFF);
            buffer_ <<= taken;
            count_ -= static_cast<int>(taken);
            out = (whole - static_cast<std::int32_t>(taken)) / 256;  // exact
            return true;
        }
        std::uint32_t entry = lookup.fast[top];  // NOLINT: top < 2^kFastBits
        if (entry == 0) {
            // A long code: the canonical way, length by length.
            const auto bits16 = static_cast<std::int32_t>(buffer_ >> 48);
            for (std::uint32_t length = kFastBits + 1; length <= 16; ++length) {
                const std::int32_t code = bits16 >> (16 - length);
                if (code <= lookup.max_code.at(length)) {
                    const std::int32_t index = code + lookup.offset.at(length);
                    if (index < 0 || index > 255) {
                        return false;
                    }
                    entry = (length << 8) | lookup.symbols.at(static_cast<std::size_t>(index));
                    break;
                }
            }
        }
        const std::uint32_t length = entry >> 8;
        const std::uint32_t size = entry & 0xFFU;
        if (length == 0 || size > 16) {
            return false;
        }
        buffer_ <<= length;
        count_ -= static_cast<int>(length);
        if (size == 0) {
            out = 0;
            return true;
        }
        if (size == 16) {
            out = -32768;
            return true;
        }
        const auto value = static_cast<std::int32_t>(buffer_ >> (64 - size));
        buffer_ <<= size;
        count_ -= static_cast<int>(size);
        // Values below 2^(size-1) are negative.
        out = value < (1 << (size - 1)) ? value - (1 << size) + 1 : value;
        return true;
    }

    // Past the next restart marker, bits from the byte after it.
    void restart() {
        buffer_ = 0;
        count_ = 0;
        marker_ = false;
        while (at_ + 1 < data_.size()) {
            if (data_[at_] == 0xFF && data_[at_ + 1] >= 0xD0 && data_[at_ + 1] <= 0xD7) {
                at_ += 2;
                return;
            }
            ++at_;
        }
        at_ = data_.size();
    }

private:
    void fill() {
        // Fast: as many whole bytes as fit, from 8 that hold no 0xFF.
        if (!marker_ && at_ + 8 <= data_.size()) {
            std::uint64_t word = 0;
            std::memcpy(&word, data_.data() + at_, sizeof word);
            const std::uint64_t inverse = ~word;
            const bool any_ff =
                ((inverse - 0x0101010101010101ULL) & ~inverse & 0x8080808080808080ULL) != 0;
            if (!any_ff) {
                if constexpr (std::endian::native == std::endian::little) {
                    word = __builtin_bswap64(word);
                }
                const int bytes = (63 - count_) / 8;
                buffer_ |= (word >> (64 - (8 * bytes))) << (64 - count_ - (8 * bytes));
                at_ += static_cast<std::size_t>(bytes);
                count_ += 8 * bytes;
                return;
            }
        }
        while (count_ <= 56) {
            std::uint64_t byte = 0;
            if (!marker_ && at_ < data_.size()) {
                byte = data_[at_];
                if (byte == 0xFF) {
                    if (at_ + 1 < data_.size() && data_[at_ + 1] == 0x00) {
                        at_ += 2;
                    } else {
                        marker_ = true;  // stays at the marker
                        byte = 0;
                    }
                } else {
                    ++at_;
                }
            }
            buffer_ |= byte << (56 - count_);
            count_ += 8;
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t at_;
    std::uint64_t buffer_{0};
    int count_{0};
    bool marker_{false};
};

}  // namespace

std::optional<LosslessFrame> lossless_frame(std::span<const std::uint8_t> data,
                                            std::string& error) {
    std::optional<Headers> headers = read_headers(data, error);
    if (!headers) {
        return std::nullopt;
    }
    return headers->frame;
}

bool decode_lossless(
    std::span<const std::uint8_t> data,
    const std::function<bool(std::uint32_t y, std::span<const std::uint16_t> samples)>& line,
    std::string& error) {
    const std::optional<Headers> headers = read_headers(data, error);
    if (!headers) {
        return false;
    }
    const Headers& h = *headers;
    // Every value costs a bit at least: a stream too short for its frame is not decoded (zeros
    // for millions of values from a few bytes).
    if (std::uint64_t{h.frame.width} * h.frame.height * h.frame.components >
        std::uint64_t{data.size() - h.scan} * 8) {
        error = "a lossless JPEG too short for its frame";
        return false;
    }
    const std::uint32_t components = h.frame.components;
    const std::size_t samples = std::size_t{h.frame.width} * components;
    std::array<const Lookup*, kMaxComponents> lookups{};
    for (std::uint32_t c = 0; c < components; ++c) {
        lookups.at(c) = h.lookups.at(h.table.at(c)).get();
    }
    std::vector<std::uint16_t> previous(samples);
    std::vector<std::uint16_t> current(samples);
    std::vector<std::uint16_t> shifted(h.point_transform != 0 ? samples : 0);
    const std::uint32_t restart_lines =
        h.restart_interval == 0 ? 0 : h.restart_interval / h.frame.width;
    const auto initial =
        static_cast<std::int32_t>(1U << (h.frame.precision - h.point_transform - 1));
    Bits bits{data, h.scan};

    for (std::uint32_t y = 0; y < h.frame.height; ++y) {
        bool first_line = y == 0;
        if (restart_lines != 0 && y != 0 && y % restart_lines == 0) {
            bits.restart();
            first_line = true;
        }
        std::uint16_t* out = current.data();
        const std::uint16_t* up = previous.data();
        std::int32_t difference = 0;
        // The first pixel: from the initial value on a first line, else from the one above.
        for (std::uint32_t c = 0; c < components; ++c) {
            if (!bits.difference(*lookups.at(c), difference)) {
                error = "a lossless JPEG with a broken code";
                return false;
            }
            out[c] = static_cast<std::uint16_t>((first_line ? initial : up[c]) + difference);
        }
        // The others: from the left on a first line or with predictor 1 (DNG's), else as the
        // predictor says.
        const bool left_only = first_line || h.predictor == 1;
        std::uint32_t c = 0;
        for (std::size_t i = components; i < samples; ++i) {
            if (!bits.difference(*lookups[c], difference)) {  // NOLINT: c < components
                error = "a lossless JPEG with a broken code";
                return false;
            }
            if (++c == components) {
                c = 0;
            }
            const std::int32_t a = out[i - components];
            std::int32_t prediction = a;
            if (!left_only) {
                const std::int32_t b = up[i];
                const std::int32_t cc = up[i - components];
                switch (h.predictor) {
                    case 2:
                        prediction = b;
                        break;
                    case 3:
                        prediction = cc;
                        break;
                    case 4:
                        prediction = a + b - cc;
                        break;
                    case 5:
                        prediction = a + ((b - cc) >> 1);
                        break;
                    case 6:
                        prediction = b + ((a - cc) >> 1);
                        break;
                    default:
                        prediction = (a + b) >> 1;
                        break;
                }
            }
            out[i] = static_cast<std::uint16_t>(prediction + difference);
        }
        if (h.point_transform != 0) {
            for (std::size_t i = 0; i < samples; ++i) {
                shifted[i] = static_cast<std::uint16_t>(current[i] << h.point_transform);
            }
            if (!line(y, shifted)) {
                return true;
            }
        } else if (!line(y, current)) {
            return true;
        }
        std::swap(previous, current);
    }
    return true;
}

}  // namespace rummage::formats::image::dng
