#include "decode.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "codecs.hpp"
#include "rummage/picture/wuffs.hpp"

namespace rummage::formats::image::tiff {
namespace {

constexpr std::uint64_t kCfa = 32803;
constexpr std::uint64_t kLinearRaw = 34892;
constexpr std::uint64_t kWhiteIsZero = 0;
constexpr std::uint64_t kBlackIsZero = 1;
constexpr std::uint64_t kRgb = 2;
constexpr std::uint64_t kPalette = 3;
constexpr std::uint64_t kSeparated = 5;
constexpr std::uint64_t kYCbCr = 6;
constexpr std::uint64_t kJpegCompression = 7;
constexpr std::uint64_t kUnsignedInt = 1;
constexpr std::size_t kMaxSubIfds = 16;
constexpr std::uint64_t kFloat = 3;

// Limits on what a TIFF may claim before anything is allocated.
constexpr std::uint64_t kMaxSide = std::uint64_t{1} << 20;
constexpr std::uint64_t kMaxSamplesPerPixel = 8;
constexpr std::size_t kMaxChunkBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxChunks = std::size_t{1} << 22;

Decoded failure(Problem problem, std::string error) {
    Decoded result;
    result.format = Format::tiff;
    result.problem = problem;
    result.error = std::move(error);
    return result;
}

// A 16-bit IEEE half float as a float.
float half_to_float(std::uint16_t half) {
    const std::uint32_t sign = (half >> 15) & 1U;
    const std::uint32_t exponent = (half >> 10) & 0x1FU;
    const std::uint32_t mantissa = half & 0x3FFU;
    float value = 0.0F;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa == 0 ? INFINITY : NAN;
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -value : value;
}

std::uint8_t float_to_8(double value) {
    if (!(value > 0.0)) {  // also NaN
        return 0;
    }
    if (value >= 1.0) {
        return 255;
    }
    return static_cast<std::uint8_t>(std::lround(value * 255.0));
}

// What the chosen image is made of.
struct Layout {
    std::uint64_t width{0};
    std::uint64_t height{0};
    std::uint64_t samples{1};  // per pixel
    std::uint64_t bits{1};     // per sample
    std::uint64_t format{kUnsignedInt};
    std::uint64_t compression{kNone};
    std::uint64_t photometric{kBlackIsZero};
    bool planar{false};
    std::uint64_t predictor{1};
    std::uint64_t channels{1};  // colour channels the photometric uses
    bool alpha{false};          // the first extra sample is alpha
    bool associated{false};     // ...premultiplied
    // Strips or tiles.
    bool tiled{false};
    std::uint64_t chunk_width{0};
    std::uint64_t chunk_height{0};
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> counts;
    std::vector<std::uint16_t> colormap;    // palette: 3 x 2^bits, red then green then blue
    std::vector<std::uint8_t> jpeg_tables;  // JPEG: the tables every strip or tile shares
    std::uint8_t orientation{1};
};

// The layout of ifd, or the reason it cannot be read.
std::optional<Layout> layout_of(const Ifd& ifd, std::string& error, Problem& problem) {
    Layout l;
    problem = Problem::damaged;
    l.width = ifd.number(tag::kImageWidth).value_or(0);
    l.height = ifd.number(tag::kImageLength).value_or(0);
    if (l.width == 0 || l.height == 0 || l.width > kMaxSide || l.height > kMaxSide) {
        error = "a TIFF without a sensible size";
        return std::nullopt;
    }
    l.samples = ifd.number(tag::kSamplesPerPixel).value_or(1);
    if (l.samples == 0 || l.samples > kMaxSamplesPerPixel) {
        error = "a TIFF with too many samples per pixel";
        return std::nullopt;
    }
    const std::vector<std::uint64_t> bits = ifd.numbers(tag::kBitsPerSample, kMaxSamplesPerPixel);
    l.bits = bits.empty() ? 1 : bits.front();
    const std::vector<std::uint64_t> formats = ifd.numbers(tag::kSampleFormat, kMaxSamplesPerPixel);
    l.format = formats.empty() ? kUnsignedInt : formats.front();
    problem = Problem::unknown_format;  // valid TIFF from here on, maybe just not read here
    if (std::ranges::any_of(bits, [&](std::uint64_t b) { return b != l.bits; }) ||
        std::ranges::any_of(formats, [&](std::uint64_t f) { return f != l.format; })) {
        error = "a TIFF whose samples differ in size or kind";
        return std::nullopt;
    }
    const bool integer = l.format == kUnsignedInt &&
                         (l.bits == 1 || l.bits == 2 || l.bits == 4 || l.bits == 8 || l.bits == 16);
    const bool floating = l.format == kFloat && (l.bits == 16 || l.bits == 32 || l.bits == 64);
    if (!integer && !floating) {
        error = "a TIFF with " + std::to_string(l.bits) + "-bit samples of format " +
                std::to_string(l.format);
        return std::nullopt;
    }
    l.compression = ifd.number(tag::kCompression).value_or(kNone);
    const bool jpeg = l.compression == kJpegCompression;
    if (!supported_compression(l.compression) && !jpeg) {
        error = "a TIFF compressed with scheme " + std::to_string(l.compression);
        return std::nullopt;
    }
    l.photometric = ifd.number(tag::kPhotometric).value_or(l.samples >= 3 ? kRgb : kBlackIsZero);
    switch (l.photometric) {
        case kWhiteIsZero:
        case kBlackIsZero:
            l.channels = 1;
            break;
        case kRgb:
            l.channels = 3;
            break;
        case kPalette:
            l.channels = 1;
            if (!integer || l.bits > 8) {
                error = "a palette TIFF with more than 8 bits an index";
                return std::nullopt;
            }
            break;
        case kSeparated:
            l.channels = 4;
            break;
        case kYCbCr:
            l.channels = 3;  // only inside JPEG, whose decoder gives RGB
            break;
        default:
            error = "a TIFF with photometric interpretation " + std::to_string(l.photometric);
            return std::nullopt;
    }
    if (jpeg &&
        (!integer || l.bits != 8 || l.photometric == kPalette || l.photometric == kSeparated ||
         (ifd.number(tag::kPlanarConfiguration).value_or(1) == 2 && l.samples > 1))) {
        error = "a JPEG-compressed TIFF of a kind not read here";
        return std::nullopt;
    }
    if (l.photometric == kYCbCr && !jpeg) {
        error = "an uncompressed YCbCr TIFF";
        return std::nullopt;
    }
    if (l.samples < l.channels) {
        error = "a TIFF with fewer samples than its colours need";
        problem = Problem::damaged;
        return std::nullopt;
    }
    if (l.samples > l.channels) {
        const std::vector<std::uint64_t> extra =
            ifd.numbers(tag::kExtraSamples, kMaxSamplesPerPixel);
        // 1: associated (premultiplied) alpha, 2: unassociated. 0 (unspecified) is not alpha.
        const std::uint64_t first = extra.empty() ? 0 : extra.front();
        l.alpha = first == 1 || first == 2;
        l.associated = first == 1;
    }
    l.planar = ifd.number(tag::kPlanarConfiguration).value_or(1) == 2 && l.samples > 1;
    l.predictor = ifd.number(tag::kPredictor).value_or(1);
    const bool horizontal = l.predictor == 2 && l.bits >= 8 && integer;
    const bool floating_point = l.predictor == 3 && floating;
    if (l.predictor != 1 && !horizontal && !floating_point) {
        error = "a TIFF with predictor " + std::to_string(l.predictor);
        return std::nullopt;
    }
    const std::uint64_t orientation = ifd.number(tag::kOrientation).value_or(1);
    l.orientation =
        orientation >= 1 && orientation <= 8 ? static_cast<std::uint8_t>(orientation) : 1;
    problem = Problem::damaged;
    const std::uint64_t planes = l.planar ? l.samples : 1;
    std::uint64_t per_plane = 0;
    if (ifd.has(tag::kTileWidth)) {
        l.tiled = true;
        l.chunk_width = ifd.number(tag::kTileWidth).value_or(0);
        l.chunk_height = ifd.number(tag::kTileLength).value_or(0);
        // Tiles are multiples of 16 and cover the image: never larger than it, rounded up to
        // 16 a side (a tile tag slipped into a TIFF of strips would otherwise ask for gigabytes).
        const auto rounded = [](std::uint64_t side) { return (side + 15) / 16 * 16; };
        if (l.chunk_width == 0 || l.chunk_height == 0 || l.chunk_width > rounded(l.width) ||
            l.chunk_height > rounded(l.height)) {
            error = "a TIFF whose tiles do not fit its size";
            return std::nullopt;
        }
        per_plane = ((l.width + l.chunk_width - 1) / l.chunk_width) *
                    ((l.height + l.chunk_height - 1) / l.chunk_height);
        if (per_plane * planes > kMaxChunks) {
            error = "a TIFF with too many tiles";
            return std::nullopt;
        }
        l.offsets = ifd.numbers(tag::kTileOffsets, kMaxChunks);
        l.counts = ifd.numbers(tag::kTileByteCounts, kMaxChunks);
    } else {
        l.chunk_width = l.width;
        l.chunk_height = std::clamp<std::uint64_t>(
            ifd.number(tag::kRowsPerStrip).value_or(l.height), 1, l.height);
        per_plane = (l.height + l.chunk_height - 1) / l.chunk_height;
        if (per_plane * planes > kMaxChunks) {
            error = "a TIFF with too many strips";
            return std::nullopt;
        }
        l.offsets = ifd.numbers(tag::kStripOffsets, kMaxChunks);
        l.counts = ifd.numbers(tag::kStripByteCounts, kMaxChunks);
        // An uncompressed single strip may leave out its byte count.
        if (l.counts.empty() && l.offsets.size() == 1 && l.compression == kNone) {
            l.counts = {ifd.container().source().size() -
                        std::min(l.offsets[0], ifd.container().source().size())};
        }
    }
    if (l.offsets.size() < per_plane * planes || l.counts.size() < l.offsets.size()) {
        error = "a TIFF whose strips or tiles are missing";
        return std::nullopt;
    }
    if (jpeg) {
        l.jpeg_tables = ifd.bytes(tag::kJpegTables, std::size_t{1} << 16);
    }
    if (l.photometric == kPalette) {
        const std::vector<std::uint64_t> map =
            ifd.numbers(tag::kColorMap, 3 * (std::size_t{1} << l.bits));
        if (map.size() != 3 * (std::size_t{1} << l.bits)) {
            error = "a palette TIFF without its colour map";
            return std::nullopt;
        }
        l.colormap.reserve(map.size());
        for (const std::uint64_t value : map) {
            l.colormap.push_back(static_cast<std::uint16_t>(value));
        }
    }
    return l;
}

// A sample of a chunk's row as 0-255 (palette indexes as they are).
struct Reader {
    const Layout& l;
    std::span<const std::uint8_t> row;

    [[nodiscard]] std::uint8_t at(std::uint64_t index) const {
        switch (l.bits) {
            case 1:
            case 2:
            case 4: {
                const std::uint64_t bit = index * l.bits;
                const std::uint8_t byte = row[static_cast<std::size_t>(bit / 8)];
                const auto shift = static_cast<unsigned>(8 - l.bits - (bit % 8));
                const unsigned value = (byte >> shift) & ((1U << l.bits) - 1U);
                if (l.photometric == kPalette) {
                    return static_cast<std::uint8_t>(value);
                }
                return static_cast<std::uint8_t>((value * 255U) / ((1U << l.bits) - 1U));
            }
            case 8:
                return row[static_cast<std::size_t>(index)];
            case 16: {
                std::uint16_t value = 0;
                std::memcpy(&value, row.data() + (index * 2), 2);
                if (l.format == kFloat) {
                    return float_to_8(static_cast<double>(half_to_float(value)));
                }
                return static_cast<std::uint8_t>(((std::uint32_t{value} * 255U) + 32767U) / 65535U);
            }
            case 32: {
                auto value = 0.0F;
                std::memcpy(&value, row.data() + (index * 4), 4);
                return float_to_8(static_cast<double>(value));
            }
            default: {
                double value = 0;
                std::memcpy(&value, row.data() + (index * 8), 8);
                return float_to_8(value);
            }
        }
    }
};

void swap_samples(std::span<std::uint8_t> data, std::uint64_t bits) {
    const auto size = static_cast<std::size_t>(bits / 8);
    for (std::size_t i = 0; i + size <= data.size(); i += size) {
        std::reverse(data.begin() + static_cast<std::ptrdiff_t>(i),
                     data.begin() + static_cast<std::ptrdiff_t>(i + size));
    }
}

// A JPEG strip or tile: the shared tables (JPEGTables, without their end) then the chunk
// (without its start) make one JPEG, decoded by wuffs to RGB; its samples go where the chunk
// sits. Extra samples (rare with JPEG) are left opaque. False when it does not decode or is too
// small for the place it fills.
bool place_jpeg(const Layout& l, std::span<const std::uint8_t> packed, std::uint64_t top,
                std::uint64_t left, std::uint64_t visible_width, std::uint64_t visible_rows,
                std::vector<std::uint8_t>& samples) {
    std::vector<std::uint8_t> stream;
    const std::vector<std::uint8_t>& tables = l.jpeg_tables;
    if (tables.size() >= 4 && tables[0] == 0xFF && tables[1] == 0xD8 && packed.size() >= 2 &&
        packed[0] == 0xFF && packed[1] == 0xD8) {
        stream.reserve(tables.size() + packed.size());
        stream.insert(stream.end(), tables.begin(), tables.end() - 2);
        stream.insert(stream.end(), packed.begin() + 2, packed.end());
    } else {
        stream.assign(packed.begin(), packed.end());
    }
    const picture::WuffsDecoded decoded = picture::decode_wuffs(stream, std::uint64_t{1} << 28);
    if (!decoded.error.empty() || decoded.image.width < visible_width ||
        decoded.image.height < visible_rows) {
        return false;
    }
    const picture::Image& chunk = decoded.image;
    for (std::uint64_t y = 0; y < visible_rows; ++y) {
        const std::uint8_t* in = chunk.rgba.data() + (y * chunk.width * 4);
        std::uint8_t* out = samples.data() + (((top + y) * l.width + left) * l.samples);
        for (std::uint64_t x = 0; x < visible_width; ++x, in += 4, out += l.samples) {
            for (std::uint64_t c = 0; c < l.samples; ++c) {
                out[c] = c < l.channels ? in[c] : 255;
            }
        }
    }
    return true;
}

}  // namespace

bool is_camera_raw(const Source& source) {
    const std::vector<std::uint8_t> head =
        source.read(0, std::min<std::uint64_t>(10, source.size()));
    if (head.size() >= 10 && head[8] == 'C' && head[9] == 'R') {
        return true;  // Canon CR2
    }
    const std::optional<Container> container = Container::open(source);
    if (!container) {
        return false;
    }
    const auto raw = [](const Ifd& ifd) {
        const std::uint64_t photometric = ifd.number(tag::kPhotometric).value_or(0);
        // Sensor depths no ordinary TIFF uses (Samsung's SRW leaves out the CFA tag).
        const std::uint64_t bits = ifd.number(tag::kBitsPerSample).value_or(8);
        return photometric == kCfa || photometric == kLinearRaw || ifd.has(tag::kDngVersion) ||
               bits == 10 || bits == 12 || bits == 14;
    };
    for (const Ifd& ifd : container->chain()) {
        if (raw(ifd)) {
            return true;
        }
        for (const std::uint64_t sub : ifd.numbers(tag::kSubIfds, 16)) {
            const std::optional<Ifd> child = container->ifd(sub);
            if (child && raw(*child)) {
                return true;
            }
        }
    }
    return false;
}

Decoded decode(const Source& source, const DecodeOptions& options) {
    const std::optional<Container> container = Container::open(source);
    if (!container) {
        return failure(Problem::unknown_format, "not a TIFF");
    }
    const std::vector<Ifd> chain = container->chain();
    if (chain.empty()) {
        return failure(Problem::damaged, "a TIFF without an image");
    }
    // The first full-resolution image of the chain, else of the SubIFDs (a scanner's TIFF may
    // keep a reduced one first and the scan in a SubIFD); a file of reduced ones only shows
    // its first.
    const auto full = [](const Ifd& candidate) {
        const std::uint64_t photometric = candidate.number(tag::kPhotometric).value_or(0);
        return (candidate.number(tag::kNewSubfileType).value_or(0) & 1U) == 0 &&
               candidate.has(tag::kImageWidth) && photometric != kCfa &&
               photometric != kLinearRaw;  // sensor data is not a picture to show
    };
    std::optional<Ifd> chosen;
    if (const auto it = std::ranges::find_if(chain, full); it != chain.end()) {
        chosen = *it;
    }
    for (std::size_t i = 0; !chosen && i < chain.size(); ++i) {
        for (const std::uint64_t sub : chain[i].numbers(tag::kSubIfds, kMaxSubIfds)) {
            if (std::optional<Ifd> child = container->ifd(sub); child && full(*child)) {
                chosen = std::move(child);
                break;
            }
        }
    }
    const Ifd& ifd = chosen ? *chosen : chain.front();
    std::string error;
    Problem problem = Problem::damaged;
    const std::optional<Layout> found = layout_of(ifd, error, problem);
    if (!found) {
        return failure(problem, error);
    }
    const Layout& l = *found;
    if (l.width * l.height > options.max_pixels) {
        return failure(Problem::too_large, picture::kTooLarge);
    }
    // Every sample as 0-255 first (palette indexes kept), then the colours.
    const std::uint64_t pixels = l.width * l.height;
    // All the chunks together decompress to about the picture's own size (more only for the
    // padding of tiles at the edges): at most four times it, or 64 MB for small ones.
    {
        const std::uint64_t across_all = (l.width + l.chunk_width - 1) / l.chunk_width;
        const std::uint64_t down_all = (l.height + l.chunk_height - 1) / l.chunk_height;
        const std::uint64_t per_row_samples = l.chunk_width * (l.planar ? 1 : l.samples);
        const std::uint64_t chunk_bytes = (((per_row_samples * l.bits) + 7) / 8) * l.chunk_height;
        const std::uint64_t total =
            chunk_bytes * across_all * down_all * (l.planar ? l.samples : 1);
        const std::uint64_t picture_bytes = (pixels * l.samples * l.bits + 7) / 8;
        if (total > std::max<std::uint64_t>(std::uint64_t{64} << 20, picture_bytes * 4)) {
            return failure(Problem::damaged, "a TIFF whose strips or tiles outgrow its size");
        }
        // And the file holds enough data for them, before anything is allocated (found by the
        // fuzzer: 3 kB of one uncompressed strip, no byte count, claiming 172 MP, decoded as
        // zeros in 2.9 s). Uncompressed: at least half the bytes (a file cut short still shows
        // its top); compressed: what the scheme could at most expand to.
        std::uint64_t present = 0;
        for (std::size_t i = 0; i < l.offsets.size() && i < l.counts.size(); ++i) {
            const std::uint64_t offset = std::min(l.offsets[i], source.size());
            present += std::min(l.counts[i], source.size() - offset);
        }
        std::uint64_t ratio = 0;  // the scheme's largest expansion
        switch (l.compression) {
            case kNone:
                ratio = 2;
                break;
            case kPackBits:
                ratio = 64;
                break;
            case kDeflate:
            case kDeflateOld:
                ratio = 1100;
                break;
            case kLzw:
                ratio = 2800;  // 4096 bytes from a 12-bit code
                break;
            default:
                ratio = 8192;  // JPEG: a few bits for a flat 8x8 block
                break;
        }
        if (present * ratio < total) {
            return failure(Problem::damaged, "a TIFF whose data is too short for its size");
        }
    }
    std::vector<std::uint8_t> samples(static_cast<std::size_t>(pixels * l.samples));
    const std::uint64_t planes = l.planar ? l.samples : 1;
    const std::uint64_t across = (l.width + l.chunk_width - 1) / l.chunk_width;
    const std::uint64_t down = (l.height + l.chunk_height - 1) / l.chunk_height;
    const std::uint64_t per_row = l.planar ? 1 : l.samples;  // samples per pixel in a chunk
    const std::uint64_t row_samples = l.chunk_width * per_row;
    const std::uint64_t row_bytes = ((row_samples * l.bits) + 7) / 8;
    for (std::uint64_t plane = 0; plane < planes; ++plane) {
        for (std::uint64_t cy = 0; cy < down; ++cy) {
            for (std::uint64_t cx = 0; cx < across; ++cx) {
                const std::uint64_t index = (plane * across * down) + (cy * across) + cx;
                const std::uint64_t top = cy * l.chunk_height;
                // A strip may end short at the bottom; a tile is always whole.
                const std::uint64_t rows =
                    l.tiled ? l.chunk_height : std::min(l.chunk_height, l.height - top);
                const std::uint64_t expected = rows * row_bytes;
                if (expected > kMaxChunkBytes) {
                    return failure(Problem::too_large, "a TIFF strip or tile too large");
                }
                const std::uint64_t offset = l.offsets[static_cast<std::size_t>(index)];
                const std::uint64_t count = l.counts[static_cast<std::size_t>(index)];
                if (count > kMaxChunkBytes || !source.has(offset, count)) {
                    return failure(Problem::damaged, "a TIFF cut short");
                }
                const std::vector<std::uint8_t> packed =
                    source.read(offset, static_cast<std::size_t>(count));
                const std::uint64_t left = cx * l.chunk_width;
                const std::uint64_t visible_width = std::min(l.chunk_width, l.width - left);
                const std::uint64_t visible_rows = std::min(rows, l.height - top);
                if (l.compression == kJpegCompression) {
                    if (!place_jpeg(l, packed, top, left, visible_width, visible_rows, samples)) {
                        return failure(Problem::damaged, "a TIFF whose JPEG data does not decode");
                    }
                    continue;
                }
                std::optional<std::vector<std::uint8_t>> chunk =
                    unpack(l.compression, packed, static_cast<std::size_t>(expected));
                if (!chunk) {
                    return failure(Problem::damaged, "a TIFF whose data does not decompress");
                }
                if (l.predictor == 3) {
                    undo_floating_point(*chunk, static_cast<std::size_t>(row_samples),
                                        static_cast<std::size_t>(per_row),
                                        static_cast<std::size_t>(l.bits / 8));
                } else {
                    if (l.bits >= 16 && !container->little_endian()) {
                        swap_samples(*chunk, l.bits);
                    }
                    if (l.predictor == 2) {
                        undo_horizontal(*chunk, static_cast<std::size_t>(row_samples),
                                        static_cast<std::size_t>(per_row),
                                        static_cast<std::size_t>(l.bits / 8));
                    }
                }
                for (std::uint64_t y = 0; y < visible_rows; ++y) {
                    const Reader reader{
                        l, std::span{*chunk}.subspan(static_cast<std::size_t>(y * row_bytes),
                                                     static_cast<std::size_t>(row_bytes))};
                    std::uint8_t* out = samples.data() + (((top + y) * l.width + left) * l.samples);
                    const std::uint64_t row_count = visible_width * l.samples;
                    if (!l.planar && l.bits == 8) {
                        // The common case, an 8-bit export: the samples as they are.
                        std::memcpy(out, reader.row.data(), static_cast<std::size_t>(row_count));
                    } else if (!l.planar && l.bits == 16 && l.format == kUnsignedInt) {
                        // 16-bit exports and scans: one tight loop, no per-sample switch.
                        const std::uint8_t* in = reader.row.data();
                        for (std::uint64_t i = 0; i < row_count; ++i, in += 2) {
                            const std::uint32_t value =
                                std::uint32_t{in[0]} | (std::uint32_t{in[1]} << 8);
                            out[i] = static_cast<std::uint8_t>(((value * 255U) + 32767U) / 65535U);
                        }
                    } else {
                        for (std::uint64_t x = 0; x < visible_width; ++x) {
                            if (l.planar) {
                                out[(x * l.samples) + plane] = reader.at(x);
                            } else {
                                for (std::uint64_t s = 0; s < l.samples; ++s) {
                                    out[(x * l.samples) + s] = reader.at((x * l.samples) + s);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Decoded result;
    result.format = Format::tiff;
    result.orientation = l.orientation;
    Image& image = result.image;
    image.width = static_cast<std::uint32_t>(l.width);
    image.height = static_cast<std::uint32_t>(l.height);
    image.rgba.resize(static_cast<std::size_t>(pixels * 4));
    for (std::uint64_t i = 0; i < pixels; ++i) {
        const std::uint8_t* s = samples.data() + (i * l.samples);
        std::uint8_t* out = image.rgba.data() + (i * 4);
        switch (l.photometric) {
            case kWhiteIsZero:
                out[0] = out[1] = out[2] = static_cast<std::uint8_t>(255 - s[0]);
                break;
            case kBlackIsZero:
                out[0] = out[1] = out[2] = s[0];
                break;
            case kPalette: {
                const std::size_t entries = std::size_t{1} << l.bits;
                for (std::size_t c = 0; c < 3; ++c) {
                    const std::uint32_t value = l.colormap[(c * entries) + s[0]];
                    out[c] = static_cast<std::uint8_t>(((value * 255U) + 32767U) / 65535U);
                }
                break;
            }
            case kYCbCr:  // decoded to RGB by the JPEG decoder
                out[0] = s[0];
                out[1] = s[1];
                out[2] = s[2];
                break;
            case kSeparated:
                for (std::size_t c = 0; c < 3; ++c) {
                    out[c] =
                        static_cast<std::uint8_t>(((255U - s[c]) * (255U - s[3]) + 127U) / 255U);
                }
                break;
            default:
                out[0] = s[0];
                out[1] = s[1];
                out[2] = s[2];
                break;
        }
        out[3] = 255;
        if (l.alpha) {
            const std::uint8_t alpha = s[l.channels];
            out[3] = alpha;
            if (l.associated && alpha > 0 && alpha < 255) {
                for (std::size_t c = 0; c < 3; ++c) {
                    out[c] = static_cast<std::uint8_t>(
                        std::min(255U, ((out[c] * 255U) + (alpha / 2U)) / alpha));
                }
            }
        }
    }
    return result;
}

std::optional<std::span<const std::uint8_t>> jpeg_in_tiff(std::span<const std::uint8_t> data) {
    constexpr std::uint64_t old_jpeg = 6;
    constexpr std::uint64_t jpeg = 7;
    const Source source{data};
    const std::optional<Container> container = Container::open(source);
    if (!container) {
        return std::nullopt;
    }
    const std::optional<Ifd> ifd = container->ifd(container->first_ifd());
    if (!ifd) {
        return std::nullopt;
    }
    const std::uint64_t compression = ifd->number(tag::kCompression).value_or(kNone);
    if (compression != jpeg && compression != old_jpeg) {
        return std::nullopt;
    }
    const std::vector<std::uint64_t> offsets = ifd->numbers(tag::kStripOffsets, 2);
    const std::vector<std::uint64_t> counts = ifd->numbers(tag::kStripByteCounts, 2);
    if (offsets.size() != 1 || counts.size() != 1 || counts[0] < 4 ||
        !source.has(offsets[0], counts[0])) {
        return std::nullopt;  // several strips are JPEGs that share tables: not one image
    }
    const std::span<const std::uint8_t> strip =
        data.subspan(static_cast<std::size_t>(offsets[0]), static_cast<std::size_t>(counts[0]));
    if (strip[0] != 0xFF || strip[1] != 0xD8) {
        return std::nullopt;
    }
    return strip;
}

}  // namespace rummage::formats::image::tiff
