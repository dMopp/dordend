#include "raw.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

#include "ljpeg.hpp"

namespace rummage::formats::image::dng {
namespace {

constexpr std::uint64_t kUncompressed = 1;
constexpr std::uint64_t kLosslessJpeg = 7;
constexpr std::uint64_t kMaxSide = 65535;
constexpr std::uint64_t kMaxPixels = 200'000'000;  // as DecodeOptions
constexpr std::uint64_t kMaxChunks = std::uint64_t{1} << 20;
constexpr std::size_t kMaxChunkBytes = std::size_t{1} << 30;
constexpr std::uint32_t kBandRows = 64;  // of a strip taller than that
constexpr std::uint64_t kMaxBlackRepeat = 64;

// The main image's IFD: full resolution (NewSubfileType 0), CFA or linear raw, the largest.
std::optional<tiff::Ifd> main_ifd(const tiff::Container& container) {
    std::optional<tiff::Ifd> best;
    std::uint64_t best_area = 0;
    const auto consider = [&](const tiff::Ifd& ifd) {
        const std::uint64_t photometric = ifd.number(tiff::tag::kPhotometric).value_or(0);
        if (ifd.number(tiff::tag::kNewSubfileType).value_or(0) != 0 ||
            (photometric != kCfa && photometric != kLinearRaw)) {
            return;
        }
        const std::uint64_t area = ifd.number(tiff::tag::kImageWidth).value_or(0) *
                                   ifd.number(tiff::tag::kImageLength).value_or(0);
        if (area > best_area) {
            best = ifd;
            best_area = area;
        }
    };
    for (const tiff::Ifd& ifd : container.chain()) {
        consider(ifd);
        for (const std::uint64_t sub : ifd.numbers(tiff::tag::kSubIfds, 16)) {
            if (const std::optional<tiff::Ifd> child = container.ifd(sub)) {
                consider(*child);
            }
        }
    }
    return best;
}

bool fail(std::string& error, Problem& problem, Problem kind, std::string why) {
    error = std::move(why);
    problem = kind;
    return false;
}

bool read_pattern(const tiff::Ifd& ifd, Raw& raw, std::string& error, Problem& problem) {
    const std::vector<std::uint64_t> dims = ifd.numbers(tiff::tag::kCfaRepeatPatternDim, 2);
    if (dims.size() != 2 || dims[0] == 0 || dims[1] == 0 || dims[0] * dims[1] > 64) {
        return fail(error, problem, Problem::damaged, "a DNG without a sensible CFA pattern");
    }
    raw.pattern_height = static_cast<std::uint32_t>(dims[0]);
    raw.pattern_width = static_cast<std::uint32_t>(dims[1]);
    const std::size_t sites = std::size_t{raw.pattern_width} * raw.pattern_height;
    const std::vector<std::uint64_t> pattern = ifd.numbers(tiff::tag::kCfaPattern, sites);
    if (pattern.size() != sites) {
        return fail(error, problem, Problem::damaged, "a DNG without a sensible CFA pattern");
    }
    std::vector<std::uint64_t> planes = ifd.numbers(tiff::tag::kCfaPlaneColor, 8);
    if (planes.empty()) {
        planes = {0, 1, 2};
    }
    for (std::size_t i = 0; i < sites; ++i) {
        if (pattern[i] >= planes.size() || planes[pattern[i]] > 6) {
            return fail(error, problem, Problem::damaged, "a DNG with an unknown CFA colour");
        }
        raw.pattern.at(i) = static_cast<std::uint8_t>(planes[pattern[i]]);
    }
    if (ifd.number(tiff::tag::kCfaLayout).value_or(1) != 1) {
        return fail(error, problem, Problem::unknown_format,
                    "a DNG whose CFA is not a rectangular grid");
    }
    return true;
}

void read_levels(const tiff::Ifd& ifd, Raw& raw) {
    const std::vector<std::uint64_t> table = ifd.numbers(tiff::tag::kLinearizationTable, 65536);
    raw.linearization.assign(table.begin(), table.end());
    for (std::size_t i = 0; i < table.size(); ++i) {
        raw.linearization[i] = static_cast<std::uint16_t>(std::min<std::uint64_t>(table[i], 65535));
    }

    const std::vector<std::uint64_t> repeat = ifd.numbers(tiff::tag::kBlackLevelRepeatDim, 2);
    if (repeat.size() == 2 && repeat[0] >= 1 && repeat[1] >= 1 && repeat[0] <= kMaxBlackRepeat &&
        repeat[1] <= kMaxBlackRepeat) {
        raw.black_rows = static_cast<std::uint32_t>(repeat[0]);
        raw.black_columns = static_cast<std::uint32_t>(repeat[1]);
    }
    const std::size_t blacks = std::size_t{raw.black_rows} * raw.black_columns * raw.samples;
    std::vector<double> black = ifd.reals(tiff::tag::kBlackLevel, blacks);
    if (black.size() == 1) {
        black.assign(blacks, black.front());
    }
    if (black.size() != blacks || !std::ranges::all_of(black, [](double b) {
            return std::isfinite(b) && b >= 0.0 && b < 65536.0;
        })) {
        raw.black_rows = 1;
        raw.black_columns = 1;
        black.assign(raw.samples, 0.0);
    }
    raw.black = std::move(black);

    const auto finite = [](const std::vector<double>& values) {
        return std::ranges::all_of(values, [](double v) { return std::isfinite(v); });
    };
    const std::uint32_t active_width = raw.active[3] - raw.active[1];
    const std::uint32_t active_height = raw.active[2] - raw.active[0];
    raw.black_delta_h = ifd.reals(tiff::tag::kBlackLevelDeltaH, active_width);
    if (raw.black_delta_h.size() != active_width || !finite(raw.black_delta_h)) {
        raw.black_delta_h.clear();
    }
    raw.black_delta_v = ifd.reals(tiff::tag::kBlackLevelDeltaV, active_height);
    if (raw.black_delta_v.size() != active_height || !finite(raw.black_delta_v)) {
        raw.black_delta_v.clear();
    }

    const double full = std::ldexp(1.0, static_cast<int>(raw.bits)) - 1.0;
    std::vector<double> white = ifd.reals(tiff::tag::kWhiteLevel, raw.samples);
    for (std::uint32_t s = 0; s < raw.samples; ++s) {
        const double value =
            white.empty() ? full : white[std::min<std::size_t>(s, white.size() - 1)];
        raw.white.at(s) = std::isfinite(value) && value > 0.0 && value <= 65535.0 ? value : full;
    }
}

void read_areas(const tiff::Ifd& ifd, Raw& raw) {
    raw.active = {0, 0, raw.height, raw.width};
    const std::vector<std::uint64_t> active = ifd.numbers(tiff::tag::kActiveArea, 4);
    if (active.size() == 4 && active[0] < active[2] && active[1] < active[3] &&
        active[2] <= raw.height && active[3] <= raw.width) {
        for (std::size_t i = 0; i < 4; ++i) {
            raw.active.at(i) = static_cast<std::uint32_t>(active[i]);
        }
    }
    const double width = raw.active[3] - raw.active[1];
    const double height = raw.active[2] - raw.active[0];
    raw.crop = {0.0, 0.0, width, height};
    // Either tag may be missing: the origin is then 0, 0 and the size the rest of the area (DNG
    // specification's defaults).
    std::vector<double> origin = ifd.reals(tiff::tag::kDefaultCropOrigin, 2);
    if (origin.size() != 2) {
        origin = {0.0, 0.0};
    }
    std::vector<double> size = ifd.reals(tiff::tag::kDefaultCropSize, 2);
    if (size.size() != 2) {
        size = {width - origin[0], height - origin[1]};
    }
    if (origin[0] >= 0.0 && origin[1] >= 0.0 && size[0] >= 1.0 && size[1] >= 1.0 &&
        origin[0] + size[0] <= width && origin[1] + size[1] <= height) {
        raw.crop = {origin[0], origin[1], size[0], size[1]};
    }
}

// Unpacks one row of count values of bits each from in (missing ones are 0): 8 and 16 bits as
// bytes and words in the file's byte order, other depths packed with the first bit highest.
void unpack_row(std::span<const std::uint8_t> in, std::uint32_t bits, bool little,
                std::span<std::uint16_t> out) {
    if (bits == 8) {
        const std::size_t n = std::min(in.size(), out.size());
        std::copy_n(in.begin(), n, out.begin());
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), 0);
        return;
    }
    if (bits == 16) {
        const std::size_t n = std::min(in.size() / 2, out.size());
        if (little == (std::endian::native == std::endian::little)) {
            std::memcpy(out.data(), in.data(), n * 2);
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), 0);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = in[2 * i];
            const std::uint32_t b = in[(2 * i) + 1];
            out[i] = static_cast<std::uint16_t>(little ? a | (b << 8) : (a << 8) | b);
        }
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), 0);
        return;
    }
    std::uint32_t buffer = 0;
    std::uint32_t count = 0;
    std::size_t at = 0;
    for (std::uint16_t& value : out) {
        while (count < bits) {
            buffer = (buffer << 8) | (at < in.size() ? in[at] : 0U);
            ++at;
            count += 8;
        }
        count -= bits;
        value = static_cast<std::uint16_t>((buffer >> count) & ((1U << bits) - 1U));
    }
}

// Where the image's strips or tiles are.
struct Chunks {
    bool tiled{false};
    std::uint32_t width{0};   // of a chunk
    std::uint32_t height{0};  // of a chunk
    std::uint32_t across{1};
    std::uint32_t down{1};
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> counts;
};

std::optional<Chunks> chunks_of(const Raw& raw, std::string& error) {
    const tiff::Ifd& ifd = raw.ifd;
    Chunks c;
    if (ifd.has(tiff::tag::kTileWidth)) {
        c.tiled = true;
        const std::uint64_t w = ifd.number(tiff::tag::kTileWidth).value_or(0);
        const std::uint64_t h = ifd.number(tiff::tag::kTileLength).value_or(0);
        const auto rounded = [](std::uint64_t side) { return (side + 15) / 16 * 16; };
        if (w == 0 || h == 0 || w > rounded(raw.width) || h > rounded(raw.height)) {
            error = "a DNG whose tiles do not fit its size";
            return std::nullopt;
        }
        c.width = static_cast<std::uint32_t>(w);
        c.height = static_cast<std::uint32_t>(h);
        c.offsets = ifd.numbers(tiff::tag::kTileOffsets, kMaxChunks);
        c.counts = ifd.numbers(tiff::tag::kTileByteCounts, kMaxChunks);
    } else {
        c.width = raw.width;
        c.height = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            ifd.number(tiff::tag::kRowsPerStrip).value_or(raw.height), 1, raw.height));
        c.offsets = ifd.numbers(tiff::tag::kStripOffsets, kMaxChunks);
        c.counts = ifd.numbers(tiff::tag::kStripByteCounts, kMaxChunks);
    }
    c.across = (raw.width + c.width - 1) / c.width;
    c.down = (raw.height + c.height - 1) / c.height;
    const std::uint64_t needed = std::uint64_t{c.across} * c.down;
    if (needed > kMaxChunks || c.offsets.size() < needed || c.counts.size() < needed) {
        error = "a DNG whose strips or tiles are missing";
        return std::nullopt;
    }
    return c;
}

// Whether every strip or tile is in the file and large enough for what it holds: uncompressed,
// all its rows; lossless JPEG, a byte for every 8 values at least (each costs a bit or more), so
// that a small file cannot ask for millions of values decoded from nothing. Checked before
// anything is allocated for the picture.
bool plausible(const Raw& raw, const Chunks& c, std::string& error) {
    const tiff::Source& source = raw.ifd.container().source();
    const std::uint64_t row_values = std::uint64_t{c.width} * raw.samples;
    const std::uint64_t row_bytes = ((row_values * raw.bits) + 7) / 8;
    for (std::uint32_t down = 0; down < c.down; ++down) {
        // A strip holds its rows; a tile always its whole height.
        const std::uint64_t rows =
            c.tiled ? c.height : std::min(c.height, raw.height - (down * c.height));
        const std::uint64_t needed =
            raw.compression == kUncompressed ? rows * row_bytes : ((rows * row_values) + 7) / 8;
        for (std::uint32_t across = 0; across < c.across; ++across) {
            const std::size_t i = (std::size_t{down} * c.across) + across;
            if (c.counts[i] < needed || c.counts[i] > kMaxChunkBytes ||
                !source.has(c.offsets[i], c.counts[i])) {
                error = "a DNG whose data is not all in the file";
                return false;
            }
        }
    }
    return true;
}

// The bytes of chunk i, or the reason they cannot be read.
std::optional<std::vector<std::uint8_t>> chunk_bytes(const Raw& raw, const Chunks& c, std::size_t i,
                                                     std::string& error) {
    const tiff::Source& source = raw.ifd.container().source();
    if (c.counts[i] == 0 || c.counts[i] > kMaxChunkBytes ||
        !source.has(c.offsets[i], c.counts[i])) {
        error = "a DNG whose data is not all in the file";
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes =
        source.read(c.offsets[i], static_cast<std::size_t>(c.counts[i]));
    if (bytes.empty()) {
        error = "cannot read the DNG's data";
        return std::nullopt;
    }
    return bytes;
}

// Fills chunk (chunk width x rows x samples values, row by row) from compressed bytes.
bool decode_chunk(const Raw& raw, const Chunks& c, std::span<const std::uint8_t> bytes,
                  std::uint32_t rows, std::span<std::uint16_t> chunk, std::string& error) {
    const std::size_t row_values = std::size_t{c.width} * raw.samples;
    if (raw.compression == kUncompressed) {
        const std::size_t row_bytes = ((row_values * raw.bits) + 7) / 8;
        const bool little = raw.ifd.container().little_endian();
        for (std::uint32_t y = 0; y < rows; ++y) {
            const std::size_t from = std::min(bytes.size(), y * row_bytes);
            const std::size_t length = std::min(bytes.size() - from, row_bytes);
            unpack_row(bytes.subspan(from, length), raw.bits, little,
                       chunk.subspan(y * row_values, row_values));
        }
        return true;
    }
    // Lossless JPEG: its samples, in order, fill the chunk row by row (a row of the JPEG may be
    // half a row of the chunk with two components, or any other split).
    std::size_t at = 0;
    const std::size_t total = std::size_t{rows} * row_values;
    const bool decoded = decode_lossless(
        bytes,
        [&](std::uint32_t, std::span<const std::uint16_t> samples) {
            const std::size_t n = std::min(samples.size(), total - at);
            std::copy_n(samples.begin(), n, chunk.begin() + static_cast<std::ptrdiff_t>(at));
            at += n;
            return at < total;
        },
        error);
    if (!decoded) {
        return false;
    }
    std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(at),
              chunk.begin() + static_cast<std::ptrdiff_t>(total), 0);
    return true;
}

}  // namespace

std::optional<Raw> find_raw(const tiff::Container& container, std::string& error,
                            Problem& problem) {
    const std::vector<tiff::Ifd> chain = container.chain(1);
    if (chain.empty() || !chain.front().has(tiff::tag::kDngVersion)) {
        fail(error, problem, Problem::unknown_format, "not a DNG");
        return std::nullopt;
    }
    std::optional<tiff::Ifd> ifd = main_ifd(container);
    if (!ifd) {
        fail(error, problem, Problem::unknown_format, "a DNG without a raw image");
        return std::nullopt;
    }
    Raw raw{*ifd};
    const std::uint64_t width = ifd->number(tiff::tag::kImageWidth).value_or(0);
    const std::uint64_t height = ifd->number(tiff::tag::kImageLength).value_or(0);
    if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide) {
        fail(error, problem, Problem::damaged, "a DNG without a sensible size");
        return std::nullopt;
    }
    raw.width = static_cast<std::uint32_t>(width);
    raw.height = static_cast<std::uint32_t>(height);
    raw.cfa = ifd->number(tiff::tag::kPhotometric).value_or(0) == kCfa;
    const std::uint64_t samples = ifd->number(tiff::tag::kSamplesPerPixel).value_or(1);
    if (samples == 0 || samples > 4 || (raw.cfa && samples != 1)) {
        fail(error, problem, Problem::unknown_format,
             "a DNG with " + std::to_string(samples) + " samples a pixel");
        return std::nullopt;
    }
    raw.samples = static_cast<std::uint32_t>(samples);
    if (width * height > kMaxPixels) {
        fail(error, problem, Problem::too_large, picture::kTooLarge);
        return std::nullopt;
    }
    const std::vector<std::uint64_t> bits = ifd->numbers(tiff::tag::kBitsPerSample, 4);
    const std::vector<std::uint64_t> formats = ifd->numbers(tiff::tag::kSampleFormat, 4);
    if (bits.empty() || bits.front() < 1 || bits.front() > 16 ||
        std::ranges::any_of(bits, [&](std::uint64_t b) { return b != bits.front(); }) ||
        std::ranges::any_of(formats, [](std::uint64_t f) { return f != 1; })) {
        fail(error, problem, Problem::unknown_format,
             "a DNG of samples other than 1 to 16-bit integers");
        return std::nullopt;
    }
    raw.bits = static_cast<std::uint32_t>(bits.front());
    raw.compression = ifd->number(tiff::tag::kCompression).value_or(kUncompressed);
    if (raw.compression != kUncompressed && raw.compression != kLosslessJpeg) {
        fail(error, problem, Problem::unknown_format,
             "a DNG compressed with scheme " + std::to_string(raw.compression));
        return std::nullopt;
    }
    if (raw.samples > 1 && ifd->number(tiff::tag::kPlanarConfiguration).value_or(1) != 1) {
        fail(error, problem, Problem::unknown_format, "a DNG stored in planes");
        return std::nullopt;
    }
    if (raw.cfa && !read_pattern(*ifd, raw, error, problem)) {
        return std::nullopt;
    }
    read_areas(*ifd, raw);
    read_levels(*ifd, raw);
    const std::optional<Chunks> chunks = chunks_of(raw, error);
    if (!chunks || !plausible(raw, *chunks, error)) {
        problem = Problem::damaged;
        return std::nullopt;
    }
    problem = Problem::none;
    return raw;
}

bool read_rows(const Raw& raw, const Band& band, std::string& error) {
    const std::optional<Chunks> found = chunks_of(raw, error);
    if (!found) {
        return false;
    }
    const Chunks& c = *found;
    const std::size_t row_values = std::size_t{raw.width} * raw.samples;
    const std::size_t chunk_row_values = std::size_t{c.width} * raw.samples;
    std::vector<std::uint16_t> values;
    const auto give = [&](std::uint32_t first_row, std::uint32_t rows) {
        const std::span<std::uint16_t> part{values.data(), std::size_t{rows} * row_values};
        if (!raw.linearization.empty()) {
            const std::size_t last = raw.linearization.size() - 1;
            for (std::uint16_t& value : part) {
                value = raw.linearization[std::min<std::size_t>(value, last)];
            }
        }
        return band(first_row, rows, part);
    };

    if (!c.tiled && raw.compression == kUncompressed) {
        // Uncompressed strips are read a band at a time: a whole strip may be the whole sensor.
        const std::size_t row_bytes = ((row_values * raw.bits) + 7) / 8;
        const tiff::Source& source = raw.ifd.container().source();
        const bool little = raw.ifd.container().little_endian();
        values.resize(std::size_t{std::min(kBandRows, c.height)} * row_values);
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t strip = 0; strip < c.down; ++strip) {
            const std::uint32_t top = strip * c.height;
            const std::uint32_t rows = std::min(c.height, raw.height - top);
            if (std::uint64_t{rows} * row_bytes > c.counts[strip] ||
                !source.has(c.offsets[strip], std::uint64_t{rows} * row_bytes)) {
                error = "a DNG whose data is not all in the file";
                return false;
            }
            for (std::uint32_t done = 0; done < rows;) {
                const std::uint32_t n = std::min(kBandRows, rows - done);
                bytes.resize(std::max(bytes.size(), n * row_bytes));  // zeroed once, then kept
                if (!source.read_into(c.offsets[strip] + (std::uint64_t{done} * row_bytes),
                                      std::span{bytes}.first(n * row_bytes))) {
                    error = "cannot read the DNG's data";
                    return false;
                }
                for (std::uint32_t y = 0; y < n; ++y) {
                    unpack_row(std::span{bytes}.subspan(y * row_bytes, row_bytes), raw.bits, little,
                               std::span{values}.subspan(y * row_values, row_values));
                }
                if (!give(top + done, n)) {
                    return true;
                }
                done += n;
            }
        }
        return true;
    }

    if (!c.tiled) {
        // Lossless JPEG strips: the decoder's lines fill bands of at most kBandRows rows.
        values.resize(std::size_t{std::min(kBandRows, c.height)} * row_values);
        for (std::uint32_t strip = 0; strip < c.down; ++strip) {
            const std::uint32_t top = strip * c.height;
            const std::uint32_t rows = std::min(c.height, raw.height - top);
            const std::optional<std::vector<std::uint8_t>> bytes =
                chunk_bytes(raw, c, strip, error);
            if (!bytes) {
                return false;
            }
            std::uint32_t done = 0;  // rows given
            std::size_t at = 0;      // values in the band being filled
            bool stopped = false;
            const auto band_size = [&] { return std::min(kBandRows, rows - done); };
            const bool decoded = decode_lossless(
                *bytes,
                [&](std::uint32_t, std::span<const std::uint16_t> samples) {
                    std::size_t i = 0;
                    while (i < samples.size() && done < rows) {
                        const std::uint32_t n = band_size();
                        const std::size_t room = (std::size_t{n} * row_values) - at;
                        const std::size_t k = std::min(samples.size() - i, room);
                        std::copy_n(samples.begin() + static_cast<std::ptrdiff_t>(i), k,
                                    values.begin() + static_cast<std::ptrdiff_t>(at));
                        at += k;
                        i += k;
                        if (k == room) {
                            if (!give(top + done, n)) {
                                stopped = true;
                                return false;
                            }
                            done += n;
                            at = 0;
                        }
                    }
                    return done < rows;
                },
                error);
            if (!decoded) {
                return false;
            }
            if (stopped) {
                return true;
            }
            // Data that ended early: the rest is 0.
            while (done < rows) {
                const std::uint32_t n = band_size();
                std::fill(values.begin() + static_cast<std::ptrdiff_t>(at),
                          values.begin() + static_cast<std::ptrdiff_t>(n * row_values), 0);
                if (!give(top + done, n)) {
                    return true;
                }
                done += n;
                at = 0;
            }
        }
        return true;
    }

    // Tiles: a row of them at a time.
    std::vector<std::uint16_t> tile(std::size_t{c.height} * chunk_row_values);
    for (std::uint32_t down = 0; down < c.down; ++down) {
        const std::uint32_t top = down * c.height;
        const std::uint32_t rows = std::min(c.height, raw.height - top);
        values.assign(std::size_t{rows} * row_values, 0);
        for (std::uint32_t across = 0; across < c.across; ++across) {
            const std::size_t index = (std::size_t{down} * c.across) + across;
            const std::optional<std::vector<std::uint8_t>> bytes =
                chunk_bytes(raw, c, index, error);
            if (!bytes || !decode_chunk(raw, c, *bytes, c.height, tile, error)) {
                return false;
            }
            const std::uint32_t left = across * c.width;
            const std::size_t visible =
                std::size_t{std::min(c.width, raw.width - left)} * raw.samples;
            for (std::uint32_t y = 0; y < rows; ++y) {
                std::copy_n(
                    tile.begin() + static_cast<std::ptrdiff_t>(y * chunk_row_values), visible,
                    values.begin() + static_cast<std::ptrdiff_t>(
                                         (y * row_values) + (std::size_t{left} * raw.samples)));
            }
        }
        if (!give(top, rows)) {
            return true;
        }
    }
    return true;
}

std::optional<std::vector<std::uint16_t>> read_all(const Raw& raw, std::string& error) {
    std::vector<std::uint16_t> all(std::size_t{raw.width} * raw.height * raw.samples);
    const std::size_t row_values = std::size_t{raw.width} * raw.samples;
    const bool read = read_rows(
        raw,
        [&](std::uint32_t first_row, std::uint32_t, std::span<const std::uint16_t> values) {
            std::ranges::copy(values,
                              all.begin() + static_cast<std::ptrdiff_t>(first_row * row_values));
            return true;
        },
        error);
    if (!read) {
        return std::nullopt;
    }
    return all;
}

}  // namespace rummage::formats::image::dng
