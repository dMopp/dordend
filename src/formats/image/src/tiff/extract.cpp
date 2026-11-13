#include "extract.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace rummage::formats::image::tiff {
namespace {

// The tags that describe an image's pixels (offsets and counts are written anew).
constexpr std::array<std::uint16_t, 21> kKept{tag::kNewSubfileType,
                                              tag::kImageWidth,
                                              tag::kImageLength,
                                              tag::kBitsPerSample,
                                              tag::kCompression,
                                              tag::kPhotometric,
                                              266 /* FillOrder */,
                                              tag::kSamplesPerPixel,
                                              tag::kRowsPerStrip,
                                              tag::kPlanarConfiguration,
                                              tag::kPredictor,
                                              tag::kColorMap,
                                              tag::kTileWidth,
                                              tag::kTileLength,
                                              tag::kExtraSamples,
                                              tag::kSampleFormat,
                                              tag::kJpegTables,
                                              529 /* YCbCrCoefficients */,
                                              530 /* YCbCrSubSampling */,
                                              531 /* YCbCrPositioning */,
                                              532 /* ReferenceBlackWhite */};
constexpr std::uint16_t kLong = 4;

class Writer {
public:
    explicit Writer(bool little) : little_{little} {}

    void u16(std::uint32_t v) {
        if (little_) {
            out.push_back(static_cast<std::uint8_t>(v));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        } else {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            out.push_back(static_cast<std::uint8_t>(v));
        }
    }

    void u32(std::uint32_t v) {
        if (little_) {
            u16(v & 0xFFFFU);
            u16(v >> 16);
        } else {
            u16(v >> 16);
            u16(v & 0xFFFFU);
        }
    }

    void set32(std::size_t at, std::uint32_t v) {
        Writer w{little_};
        w.u32(v);
        std::ranges::copy(w.out, out.begin() + static_cast<std::ptrdiff_t>(at));
    }

    std::vector<std::uint8_t> out;

private:
    bool little_;
};

}  // namespace

std::vector<std::uint8_t> extract(const Ifd& ifd, std::size_t max_bytes) {
    const Container& container = ifd.container();
    if (container.big_tiff()) {
        return {};
    }
    const bool tiled = ifd.has(tag::kTileOffsets);
    const std::uint16_t offsets_tag = tiled ? tag::kTileOffsets : tag::kStripOffsets;
    const std::uint16_t counts_tag = tiled ? tag::kTileByteCounts : tag::kStripByteCounts;
    const std::vector<std::uint64_t> offsets = ifd.numbers(offsets_tag, 1U << 16);
    const std::vector<std::uint64_t> counts = ifd.numbers(counts_tag, 1U << 16);
    if (offsets.empty() || offsets.size() != counts.size()) {
        return {};
    }
    std::uint64_t total = 0;
    for (const std::uint64_t count : counts) {
        total += count;
        if (count > max_bytes || total > max_bytes) {
            return {};
        }
    }

    // Header, then the IFD (its values that do not fit after it), then the chunks.
    struct Field {
        std::uint16_t tag;
        std::uint16_t type;
        std::uint32_t count;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<Field> fields;
    for (const std::uint16_t kept : kKept) {
        const Entry* entry = ifd.find(kept);
        if (entry == nullptr || entry->count > UINT32_MAX) {
            continue;
        }
        std::vector<std::uint8_t> bytes = ifd.bytes(kept);
        if (bytes.empty() && entry->count > 0) {
            continue;  // out of the data, or an unknown type
        }
        fields.push_back({.tag = kept,
                          .type = entry->type,
                          .count = static_cast<std::uint32_t>(entry->count),
                          .bytes = std::move(bytes)});
    }
    const auto chunks = static_cast<std::uint32_t>(offsets.size());
    fields.push_back({.tag = offsets_tag, .type = kLong, .count = chunks, .bytes = {}});
    fields.push_back({.tag = counts_tag, .type = kLong, .count = chunks, .bytes = {}});
    std::ranges::sort(fields, {}, &Field::tag);

    Writer w{container.little_endian()};
    w.out = container.little_endian() ? std::vector<std::uint8_t>{'I', 'I', 42, 0}
                                      : std::vector<std::uint8_t>{'M', 'M', 0, 42};
    w.u32(8);
    const std::size_t table = 8;
    const std::size_t after = table + 2 + (fields.size() * 12) + 4;
    w.out.resize(after);
    // Values too long for their entry, then the chunk offsets and counts.
    std::vector<std::size_t> value_at(fields.size(), 0);
    std::size_t offsets_at = 0;
    std::size_t counts_at = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        Field& f = fields[i];
        if (f.tag == offsets_tag || f.tag == counts_tag) {
            if (f.tag == offsets_tag) {
                offsets_at = w.out.size();
            } else {
                counts_at = w.out.size();
            }
            w.out.resize(w.out.size() + (std::size_t{chunks} * 4));
            continue;
        }
        if (f.bytes.size() > 4) {
            value_at[i] = w.out.size();
            w.out.insert(w.out.end(), f.bytes.begin(), f.bytes.end());
            if (w.out.size() % 2 != 0) {
                w.out.push_back(0);
            }
        }
    }
    for (std::uint32_t c = 0; c < chunks; ++c) {
        const std::vector<std::uint8_t> data =
            container.source().read(offsets[c], static_cast<std::size_t>(counts[c]));
        if (data.size() != counts[c]) {
            return {};
        }
        w.set32(offsets_at + (std::size_t{c} * 4), static_cast<std::uint32_t>(w.out.size()));
        w.set32(counts_at + (std::size_t{c} * 4), static_cast<std::uint32_t>(counts[c]));
        w.out.insert(w.out.end(), data.begin(), data.end());
        if (w.out.size() > UINT32_MAX) {
            return {};
        }
    }
    // The IFD itself.
    Writer entries{container.little_endian()};
    entries.u16(static_cast<std::uint32_t>(fields.size()));
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const Field& f = fields[i];
        entries.u16(f.tag);
        entries.u16(f.type);
        entries.u32(f.count);
        if (f.tag == offsets_tag || f.tag == counts_tag) {
            if (chunks == 1) {
                // One value sits in the entry: copied from where it was written above.
                const std::size_t from = f.tag == offsets_tag ? offsets_at : counts_at;
                entries.out.insert(entries.out.end(),
                                   w.out.begin() + static_cast<std::ptrdiff_t>(from),
                                   w.out.begin() + static_cast<std::ptrdiff_t>(from + 4));
            } else {
                entries.u32(
                    static_cast<std::uint32_t>(f.tag == offsets_tag ? offsets_at : counts_at));
            }
        } else if (f.bytes.size() <= 4) {
            std::array<std::uint8_t, 4> inline_value{};
            std::ranges::copy(f.bytes, inline_value.begin());
            entries.out.insert(entries.out.end(), inline_value.begin(), inline_value.end());
        } else {
            entries.u32(static_cast<std::uint32_t>(value_at[i]));
        }
    }
    entries.u32(0);  // no next IFD
    std::ranges::copy(entries.out, w.out.begin() + static_cast<std::ptrdiff_t>(table));
    return std::move(w.out);
}

}  // namespace rummage::formats::image::tiff
