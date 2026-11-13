// DNG sensor data (ROADMAP 2d-1): the lossless JPEG decoder against libjpeg-turbo's, and DNGs
// built here (strips, tiles, packed depths, linearization, levels, damage); their development
// (2d-2) and opcodes (2d-4).
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "dng/colour.hpp"
#include "dng/develop.hpp"
#include "dng/ljpeg.hpp"
#include "dng/opcodes.hpp"
#include "dng/raw.hpp"
#include "rummage/formats/image/load.hpp"
#include "temp_dir.hpp"
#include "tiff/container.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace dng = rummage::formats::image::dng;
namespace tiff = rummage::formats::image::tiff;

namespace {

// The value of an optional that must be there (the test stops here when it is not).
template <typename T>
const T& got(const std::optional<T>& value) {
    REQUIRE(value.has_value());
    return *value;  // NOLINT(bugprone-unchecked-optional-access): REQUIRE above
}

const fs::path kFixtures = fs::path{RUMMAGE_TEST_DATA} / "image" / "dng";

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

// A PGM or PPM as djpeg writes it: width, height, channels and the samples.
struct Pnm {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t channels{1};
    std::vector<std::uint16_t> samples;
};

Pnm read_pnm(const fs::path& path) {
    const std::vector<std::uint8_t> bytes = read_bytes(path);
    Pnm pnm;
    std::size_t at = 2;
    pnm.channels = bytes.at(1) == '6' ? 3 : 1;
    const auto number = [&] {
        while (bytes.at(at) == ' ' || bytes.at(at) == '\n') {
            ++at;
        }
        std::uint32_t value = 0;
        while (bytes.at(at) >= '0' && bytes.at(at) <= '9') {
            value = (value * 10) + (bytes.at(at++) - '0');
        }
        return value;
    };
    pnm.width = number();
    pnm.height = number();
    const std::uint32_t maximum = number();
    ++at;  // the one white space before the samples
    const std::size_t count = std::size_t{pnm.width} * pnm.height * pnm.channels;
    for (std::size_t i = 0; i < count; ++i) {
        if (maximum > 255) {
            pnm.samples.push_back(
                static_cast<std::uint16_t>((bytes.at(at) << 8) | bytes.at(at + 1)));
            at += 2;
        } else {
            pnm.samples.push_back(bytes.at(at++));
        }
    }
    return pnm;
}

std::optional<std::vector<std::uint16_t>> decode_all(std::span<const std::uint8_t> data,
                                                     std::string& error) {
    std::vector<std::uint16_t> all;
    std::uint32_t expected = 0;
    const bool decoded = dng::decode_lossless(
        data,
        [&](std::uint32_t y, std::span<const std::uint16_t> samples) {
            CHECK(y == expected++);
            all.insert(all.end(), samples.begin(), samples.end());
            return true;
        },
        error);
    if (!decoded) {
        return std::nullopt;
    }
    return all;
}

// A little-endian TIFF written field by field: blobs first, then IFDs (each followed by the
// values that do not fit in its entries).
class TiffWriter {
public:
    struct Field {
        std::uint16_t tag;
        std::uint16_t type;
        std::uint32_t count;
        std::vector<std::uint8_t> bytes;
    };

    static Field shorts(std::uint16_t tag, const std::vector<std::uint32_t>& values) {
        Field f{
            .tag = tag, .type = 3, .count = static_cast<std::uint32_t>(values.size()), .bytes = {}};
        for (const std::uint32_t v : values) {
            f.bytes.push_back(static_cast<std::uint8_t>(v));
            f.bytes.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        return f;
    }

    static Field longs(std::uint16_t tag, const std::vector<std::uint32_t>& values) {
        Field f{
            .tag = tag, .type = 4, .count = static_cast<std::uint32_t>(values.size()), .bytes = {}};
        for (const std::uint32_t v : values) {
            for (int shift = 0; shift < 32; shift += 8) {
                f.bytes.push_back(static_cast<std::uint8_t>(v >> shift));
            }
        }
        return f;
    }

    static Field bytes(std::uint16_t tag, const std::vector<std::uint8_t>& values) {
        return {.tag = tag,
                .type = 1,
                .count = static_cast<std::uint32_t>(values.size()),
                .bytes = values};
    }

    static Field rationals(std::uint16_t tag,
                           const std::vector<std::pair<std::uint32_t, std::uint32_t>>& values) {
        Field f = longs(tag, {});
        f.type = 5;
        f.count = static_cast<std::uint32_t>(values.size());
        for (const auto& [numerator, denominator] : values) {
            for (const std::uint32_t v : {numerator, denominator}) {
                for (int shift = 0; shift < 32; shift += 8) {
                    f.bytes.push_back(static_cast<std::uint8_t>(v >> shift));
                }
            }
        }
        return f;
    }

    // Signed fractions over 10^7 (so values within +-214).
    static Field srationals(std::uint16_t tag, const std::vector<double>& values) {
        Field f = longs(tag, {});
        f.type = 10;
        f.count = static_cast<std::uint32_t>(values.size());
        for (const double value : values) {
            const auto numerator = static_cast<std::int32_t>(std::lround(value * 1e7));
            for (const std::uint32_t v : {static_cast<std::uint32_t>(numerator), 10'000'000U}) {
                for (int shift = 0; shift < 32; shift += 8) {
                    f.bytes.push_back(static_cast<std::uint8_t>(v >> shift));
                }
            }
        }
        return f;
    }

    TiffWriter() : file_{'I', 'I', 42, 0, 0, 0, 0, 0} {}

    std::uint32_t blob(std::span<const std::uint8_t> data) {
        const auto at = static_cast<std::uint32_t>(file_.size());
        file_.insert(file_.end(), data.begin(), data.end());
        if (file_.size() % 2 != 0) {
            file_.push_back(0);
        }
        return at;
    }

    std::uint32_t ifd(std::vector<Field> fields) {
        std::ranges::sort(fields, {}, &Field::tag);
        const auto at = static_cast<std::uint32_t>(file_.size());
        const std::size_t table = 2 + (fields.size() * 12) + 4;
        file_.resize(file_.size() + table);
        put16(at, static_cast<std::uint32_t>(fields.size()));
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const Field& f = fields[i];
            const std::size_t entry = at + 2 + (i * 12);
            put16(entry, f.tag);
            put16(entry + 2, f.type);
            put32(entry + 4, f.count);
            if (f.bytes.size() <= 4) {
                std::ranges::copy(f.bytes, file_.begin() + static_cast<std::ptrdiff_t>(entry + 8));
            } else {
                put32(entry + 8, blob(f.bytes));
            }
        }
        return at;
    }

    std::vector<std::uint8_t> finish(std::uint32_t first_ifd) {
        put32(4, first_ifd);
        return file_;
    }

private:
    void put16(std::size_t at, std::uint32_t v) {
        file_.at(at) = static_cast<std::uint8_t>(v);
        file_.at(at + 1) = static_cast<std::uint8_t>(v >> 8);
    }

    void put32(std::size_t at, std::uint32_t v) {
        put16(at, v & 0xFFFFU);
        put16(at + 2, v >> 16);
    }

    std::vector<std::uint8_t> file_;
};

using W = TiffWriter;
namespace tag = tiff::tag;

// A DNG: a 16x16 thumbnail in IFD0, the raw image (fields) in its SubIFD, the data first.
std::vector<std::uint8_t> make_dng(const std::vector<std::vector<std::uint8_t>>& chunks,
                                   std::vector<W::Field> fields, bool tiled,
                                   std::vector<W::Field> more_ifd0 = {}) {
    W w;
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> counts;
    for (const std::vector<std::uint8_t>& chunk : chunks) {
        offsets.push_back(w.blob(chunk));
        counts.push_back(static_cast<std::uint32_t>(chunk.size()));
    }
    fields.push_back(W::longs(tiled ? tag::kTileOffsets : tag::kStripOffsets, offsets));
    fields.push_back(W::longs(tiled ? tag::kTileByteCounts : tag::kStripByteCounts, counts));
    const std::uint32_t sub = w.ifd(std::move(fields));
    const std::vector<std::uint8_t> thumb(std::size_t{16} * 16 * 3, 128);
    const std::uint32_t thumb_at = w.blob(thumb);
    std::vector<W::Field> ifd0{
        W::longs(tag::kNewSubfileType, {1}),
        W::shorts(tag::kImageWidth, {16}),
        W::shorts(tag::kImageLength, {16}),
        W::shorts(tag::kBitsPerSample, {8, 8, 8}),
        W::shorts(tag::kCompression, {1}),
        W::shorts(tag::kPhotometric, {2}),
        W::longs(tag::kStripOffsets, {thumb_at}),
        W::shorts(tag::kSamplesPerPixel, {3}),
        W::longs(tag::kStripByteCounts, {static_cast<std::uint32_t>(thumb.size())}),
        W::longs(tag::kSubIfds, {sub}),
        W::bytes(tag::kDngVersion, {1, 4, 0, 0}),
    };
    ifd0.insert(ifd0.end(), more_ifd0.begin(), more_ifd0.end());
    const std::uint32_t first = w.ifd(std::move(ifd0));
    return w.finish(first);
}

std::vector<W::Field> cfa_fields(std::uint32_t width, std::uint32_t height, std::uint32_t bits,
                                 std::uint32_t compression) {
    return {
        W::longs(tag::kNewSubfileType, {0}),
        W::longs(tag::kImageWidth, {width}),
        W::longs(tag::kImageLength, {height}),
        W::shorts(tag::kBitsPerSample, {bits}),
        W::shorts(tag::kCompression, {compression}),
        W::shorts(tag::kPhotometric, {static_cast<std::uint32_t>(dng::kCfa)}),
        W::shorts(tag::kSamplesPerPixel, {1}),
        W::shorts(tag::kCfaRepeatPatternDim, {2, 2}),
        W::bytes(tag::kCfaPattern, {0, 1, 1, 2}),
    };
}

// What a sensor might record at (x, y), within bits.
std::uint16_t site(std::uint32_t x, std::uint32_t y, std::uint32_t bits) {
    return static_cast<std::uint16_t>(((x * 7919U) + (y * 104729U) + (x * y)) &
                                      ((1U << bits) - 1U));
}

// Rows of values packed at bits each, highest bit first, every row starting on a byte.
std::vector<std::uint8_t> pack(std::uint32_t width, std::uint32_t y0, std::uint32_t rows,
                               std::uint32_t bits) {
    std::vector<std::uint8_t> out;
    for (std::uint32_t y = y0; y < y0 + rows; ++y) {
        std::uint64_t buffer = 0;
        std::uint32_t count = 0;
        for (std::uint32_t x = 0; x < width; ++x) {
            buffer = (buffer << bits) | site(x, y, bits);
            count += bits;
            while (count >= 8) {
                count -= 8;
                out.push_back(static_cast<std::uint8_t>(buffer >> count));
            }
        }
        if (count > 0) {
            out.push_back(static_cast<std::uint8_t>(buffer << (8 - count)));
        }
    }
    return out;
}

std::optional<dng::Raw> open_raw(const std::vector<std::uint8_t>& file, std::string& error,
                                 image::Problem& problem) {
    const tiff::Source source{file};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    REQUIRE(container.has_value());
    return dng::find_raw(got(container), error, problem);
}

}  // namespace

TEST_CASE("dng: lossless JPEG decodes as libjpeg-turbo does") {
    int checked = 0;
    for (const auto& entry : fs::directory_iterator{kFixtures}) {
        if (entry.path().extension() != ".ljpg") {
            continue;
        }
        INFO(entry.path().filename().string());
        const std::vector<std::uint8_t> data = read_bytes(entry.path());
        const Pnm expected = read_pnm(fs::path{entry.path()}.replace_extension(".pnm"));
        std::string error;
        const std::optional<dng::LosslessFrame> frame = dng::lossless_frame(data, error);
        REQUIRE(frame.has_value());
        CHECK(got(frame).width == expected.width);
        CHECK(got(frame).height == expected.height);
        CHECK(got(frame).components == expected.channels);
        const std::optional<std::vector<std::uint16_t>> decoded = decode_all(data, error);
        REQUIRE_MESSAGE(decoded.has_value(), error);
        CHECK(got(decoded) == expected.samples);
        ++checked;
    }
    CHECK(checked == 12);
}

TEST_CASE("dng: broken lossless JPEGs are refused or decode to something, never further") {
    const std::vector<std::uint8_t> good = read_bytes(kFixtures / "gray16-p1.ljpg");
    std::string error;
    CHECK_FALSE(dng::lossless_frame(std::vector<std::uint8_t>{0xFF, 0xD8}, error));
    CHECK_FALSE(dng::lossless_frame(std::vector<std::uint8_t>{1, 2, 3, 4, 5}, error));
    // Every cut: refused in the headers, or decoded with zeros where the data ended.
    for (std::size_t cut = 0; cut < good.size(); cut += 7) {
        const std::span<const std::uint8_t> part{good.data(), cut};
        std::size_t lines = 0;
        error.clear();
        const bool decoded = dng::decode_lossless(
            part,
            [&](std::uint32_t, std::span<const std::uint16_t>) {
                ++lines;
                return true;
            },
            error);
        CHECK((decoded ? lines == 29 : !error.empty()));
    }
    // Every byte flipped: no crash, and lines are all of the frame's width.
    for (std::size_t i = 0; i < good.size(); ++i) {
        std::vector<std::uint8_t> bad = good;
        bad[i] ^= 0x5A;
        std::string why;
        std::optional<std::uint32_t> width;
        if (const auto frame = dng::lossless_frame(bad, why)) {
            width = got(frame).width * got(frame).components;
        }
        (void)dng::decode_lossless(
            bad,
            [&](std::uint32_t, std::span<const std::uint16_t> samples) {
                CHECK(width.has_value());
                CHECK(samples.size() == width.value_or(0));
                return true;
            },
            why);
    }
    // A frame of 65535 x 65535 claimed by a few bytes: refused, not decoded as zeros.
    std::vector<std::uint8_t> huge = good;
    for (std::size_t i = 2; i + 8 < huge.size(); ++i) {
        if (huge[i] == 0xFF && huge[i + 1] == 0xC3) {
            huge[i + 5] = 0xFF;  // height
            huge[i + 6] = 0xFF;
            huge[i + 7] = 0xFF;  // width
            huge[i + 8] = 0xFF;
            break;
        }
    }
    int lines = 0;
    CHECK_FALSE(dng::decode_lossless(
        huge,
        [&](std::uint32_t, std::span<const std::uint16_t>) {
            ++lines;
            return true;
        },
        error));
    CHECK(lines == 0);
    CHECK(error == "a lossless JPEG too short for its frame");
    // A JPEG that is not lossless.
    const std::vector<std::uint8_t> baseline =
        read_bytes(fs::path{RUMMAGE_TEST_DATA} / "image" / "shot.jpg");
    CHECK_FALSE(dng::lossless_frame(baseline, error));
    CHECK(error == "a JPEG that is not lossless");
}

TEST_CASE("dng: uncompressed sensor data at 8, 10, 12, 14 and 16 bits") {
    constexpr std::uint32_t width = 37;
    constexpr std::uint32_t height = 29;
    for (const std::uint32_t bits : {8U, 10U, 12U, 14U, 16U}) {
        INFO("bits " << bits);
        // Strips of 5 rows, the last one short.
        std::vector<std::vector<std::uint8_t>> strips;
        for (std::uint32_t y = 0; y < height; y += 5) {
            const std::uint32_t rows = std::min(5U, height - y);
            if (bits == 16) {
                std::vector<std::uint8_t> strip;
                for (std::uint32_t r = y; r < y + rows; ++r) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        strip.push_back(static_cast<std::uint8_t>(site(x, r, 16)));
                        strip.push_back(static_cast<std::uint8_t>(site(x, r, 16) >> 8));
                    }
                }
                strips.push_back(std::move(strip));
            } else {
                strips.push_back(pack(width, y, rows, bits));
            }
        }
        std::vector<W::Field> fields = cfa_fields(width, height, bits, 1);
        fields.push_back(W::shorts(tag::kRowsPerStrip, {5}));
        const std::vector<std::uint8_t> file = make_dng(strips, std::move(fields), false);
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Raw> raw = open_raw(file, error, problem);
        REQUIRE_MESSAGE(raw.has_value(), error);
        CHECK(got(raw).width == width);
        CHECK(got(raw).height == height);
        CHECK(got(raw).cfa);
        CHECK(got(raw).pattern_width == 2);
        CHECK(got(raw).pattern_height == 2);
        CHECK(got(raw).pattern[0] == 0);
        CHECK(got(raw).pattern[1] == 1);
        CHECK(got(raw).pattern[3] == 2);
        CHECK(got(raw).white[0] == doctest::Approx((1U << bits) - 1U));
        CHECK(got(raw).active == std::array<std::uint32_t, 4>{0, 0, height, width});
        const std::optional<std::vector<std::uint16_t>> values = dng::read_all(got(raw), error);
        REQUIRE_MESSAGE(values.has_value(), error);
        bool same = true;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                same = same && got(values)[(y * width) + x] == site(x, y, bits);
            }
        }
        CHECK(same);
    }
}

TEST_CASE("dng: levels, areas and the linearization table") {
    constexpr std::uint32_t width = 20;
    constexpr std::uint32_t height = 10;
    std::vector<W::Field> fields = cfa_fields(width, height, 8, 1);
    std::vector<std::uint32_t> table(256);
    for (std::uint32_t i = 0; i < 256; ++i) {
        table[i] = (i * 4) + 1;
    }
    fields.push_back(W::shorts(tag::kLinearizationTable, table));
    fields.push_back(W::shorts(tag::kBlackLevelRepeatDim, {2, 2}));
    fields.push_back(W::rationals(tag::kBlackLevel, {{64, 1}, {129, 2}, {65, 1}, {66, 1}}));
    fields.push_back(W::shorts(tag::kWhiteLevel, {1000}));
    fields.push_back(W::shorts(tag::kActiveArea, {1, 2, 9, 18}));
    fields.push_back(W::rationals(tag::kDefaultCropOrigin, {{1, 1}, {1, 2}}));
    fields.push_back(W::rationals(tag::kDefaultCropSize, {{14, 1}, {7, 1}}));
    std::vector<std::uint8_t> strip(std::size_t{width} * height);
    for (std::size_t i = 0; i < strip.size(); ++i) {
        strip[i] = static_cast<std::uint8_t>(i);
    }
    const std::vector<std::uint8_t> file = make_dng({strip}, std::move(fields), false);
    std::string error;
    image::Problem problem{};
    const std::optional<dng::Raw> raw = open_raw(file, error, problem);
    REQUIRE_MESSAGE(raw.has_value(), error);
    CHECK(got(raw).black_rows == 2);
    CHECK(got(raw).black_columns == 2);
    CHECK(got(raw).black == std::vector<double>{64.0, 64.5, 65.0, 66.0});
    CHECK(got(raw).white[0] == doctest::Approx(1000.0));
    CHECK(got(raw).active == std::array<std::uint32_t, 4>{1, 2, 9, 18});
    CHECK(got(raw).crop == std::array<double, 4>{1.0, 0.5, 14.0, 7.0});
    const std::optional<std::vector<std::uint16_t>> values = dng::read_all(got(raw), error);
    REQUIRE(values.has_value());
    for (std::size_t i = 0; i < got(values).size(); ++i) {
        CHECK(got(values)[i] == ((i % 256) * 4) + 1);
    }
}

TEST_CASE("dng: lossless JPEG in tiles and in strips") {
    const std::vector<std::uint8_t> gray = read_bytes(kFixtures / "gray16-p6.ljpg");
    const Pnm gray_pnm = read_pnm(kFixtures / "gray16-p6.pnm");
    SUBCASE("2x2 tiles of 37x29, the right and bottom ones cut") {
        constexpr std::uint32_t width = 60;
        constexpr std::uint32_t height = 50;
        std::vector<W::Field> fields = cfa_fields(width, height, 16, 7);
        fields.push_back(W::shorts(tag::kTileWidth, {37}));
        fields.push_back(W::shorts(tag::kTileLength, {29}));
        const std::vector<std::uint8_t> file =
            make_dng({gray, gray, gray, gray}, std::move(fields), true);
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Raw> raw = open_raw(file, error, problem);
        REQUIRE_MESSAGE(raw.has_value(), error);
        std::vector<std::uint32_t> bands;
        const bool read = dng::read_rows(
            got(raw),
            [&](std::uint32_t first, std::uint32_t rows, std::span<const std::uint16_t> values) {
                bands.push_back(first);
                CHECK(values.size() == std::size_t{rows} * width);
                for (std::uint32_t y = 0; y < rows; ++y) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        const std::uint32_t ty = (first + y) % 29;
                        const std::uint32_t tx = x % 37;
                        REQUIRE(values[(y * width) + x] == gray_pnm.samples[(ty * 37) + tx]);
                    }
                }
                return true;
            },
            error);
        CHECK(read);
        CHECK(bands == std::vector<std::uint32_t>{0, 29});
    }
    SUBCASE("linear raw: two strips of three components") {
        const std::vector<std::uint8_t> rgb = read_bytes(kFixtures / "rgb14-p6-restart.ljpg");
        const Pnm rgb_pnm = read_pnm(kFixtures / "rgb14-p6-restart.pnm");
        const std::vector<W::Field> fields{
            W::longs(tag::kNewSubfileType, {0}),
            W::longs(tag::kImageWidth, {37}),
            W::longs(tag::kImageLength, {58}),
            W::shorts(tag::kBitsPerSample, {14, 14, 14}),
            W::shorts(tag::kCompression, {7}),
            W::shorts(tag::kPhotometric, {static_cast<std::uint32_t>(dng::kLinearRaw)}),
            W::shorts(tag::kSamplesPerPixel, {3}),
            W::shorts(tag::kRowsPerStrip, {29}),
        };
        const std::vector<std::uint8_t> file = make_dng({rgb, rgb}, fields, false);
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Raw> raw = open_raw(file, error, problem);
        REQUIRE_MESSAGE(raw.has_value(), error);
        CHECK_FALSE(got(raw).cfa);
        CHECK(got(raw).samples == 3);
        CHECK(got(raw).white[2] == doctest::Approx(16383.0));
        const std::optional<std::vector<std::uint16_t>> values = dng::read_all(got(raw), error);
        REQUIRE_MESSAGE(values.has_value(), error);
        std::vector<std::uint16_t> twice = rgb_pnm.samples;
        twice.insert(twice.end(), rgb_pnm.samples.begin(), rgb_pnm.samples.end());
        CHECK(got(values) == twice);
    }
    SUBCASE("a strip of 29 rows given in one band, and a reader that stops") {
        std::vector<W::Field> fields = cfa_fields(37, 29, 16, 7);
        const std::vector<std::uint8_t> file = make_dng({gray}, std::move(fields), false);
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Raw> raw = open_raw(file, error, problem);
        REQUIRE(raw.has_value());
        int calls = 0;
        CHECK(dng::read_rows(
            got(raw),
            [&](std::uint32_t, std::uint32_t rows, std::span<const std::uint16_t>) {
                CHECK(rows == 29);
                ++calls;
                return false;
            },
            error));
        CHECK(calls == 1);
    }
}

TEST_CASE("dng: what is not read, and damage") {
    std::string error;
    image::Problem problem{};
    std::vector<std::uint8_t> strip(std::size_t{37} * 29 * 2);
    SUBCASE("a TIFF that is not a DNG") {
        W w;
        const std::uint32_t at = w.blob(strip);
        const std::uint32_t first =
            w.ifd({W::shorts(tag::kImageWidth, {37}), W::shorts(tag::kImageLength, {29}),
                   W::longs(tag::kStripOffsets, {at})});
        CHECK_FALSE(open_raw(w.finish(first), error, problem));
        CHECK(problem == image::Problem::unknown_format);
        CHECK(error == "not a DNG");
    }
    SUBCASE("a compression not read (Deflate)") {
        const std::vector<std::uint8_t> file = make_dng({strip}, cfa_fields(37, 29, 16, 8), false);
        CHECK_FALSE(open_raw(file, error, problem));
        CHECK(problem == image::Problem::unknown_format);
        CHECK(error == "a DNG compressed with scheme 8");
    }
    SUBCASE("a CFA without its pattern") {
        std::vector<W::Field> fields = cfa_fields(37, 29, 16, 1);
        fields.pop_back();
        const std::vector<std::uint8_t> file = make_dng({strip}, std::move(fields), false);
        CHECK_FALSE(open_raw(file, error, problem));
        CHECK(problem == image::Problem::damaged);
    }
    SUBCASE("a strip shorter than its rows") {
        strip.resize(100);
        const std::vector<std::uint8_t> file = make_dng({strip}, cfa_fields(37, 29, 16, 1), false);
        CHECK_FALSE(open_raw(file, error, problem));
        CHECK(problem == image::Problem::damaged);
        CHECK(error == "a DNG whose data is not all in the file");
    }
    SUBCASE("lossless JPEG too small for what it claims: refused before anything is decoded") {
        // 30 bytes cannot code 37 x 29 values (a bit each at least).
        const std::vector<std::uint8_t> gray = read_bytes(kFixtures / "gray16-p1.ljpg");
        const std::vector<std::uint8_t> head{gray.begin(), gray.begin() + 30};
        const std::vector<std::uint8_t> file = make_dng({head}, cfa_fields(37, 29, 16, 7), false);
        CHECK_FALSE(open_raw(file, error, problem));
        CHECK(error == "a DNG whose data is not all in the file");
    }
    SUBCASE("a file cut anywhere: refused or read, never past its end") {
        const std::vector<std::uint8_t> gray = read_bytes(kFixtures / "gray16-p1.ljpg");
        std::vector<W::Field> fields = cfa_fields(37, 29, 16, 7);
        const std::vector<std::uint8_t> file = make_dng({gray}, std::move(fields), false);
        for (std::size_t cut = 8; cut < file.size(); cut += 13) {
            const std::vector<std::uint8_t> part{file.begin(),
                                                 file.begin() + static_cast<std::ptrdiff_t>(cut)};
            const tiff::Source source{part};
            const std::optional<tiff::Container> container = tiff::Container::open(source);
            REQUIRE(container.has_value());
            if (const std::optional<dng::Raw> raw = dng::find_raw(got(container), error, problem)) {
                (void)dng::read_all(got(raw), error);
            }
        }
    }
}

namespace {

// XYZ (D65) to linear sRGB: a camera whose colour matrix it is sees in linear sRGB.
const std::vector<double> kXyzToSrgb{3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
                                     0.0415560, 0.0556434,  -0.2040259, 1.0572252};
// Linear sRGB to XYZ (D50), Bradford-adapted: the forward matrix of the same camera.
const std::vector<double> kSrgbToD50{0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786,
                                     0.0606169, 0.0139322, 0.0971045, 0.7141733};

// The colour of a DNG whose first IFD has fields (and the usual thumbnail).
std::optional<dng::Colour> colour_with(std::vector<W::Field> fields, std::string& error) {
    const std::vector<std::uint8_t> file =
        make_dng({std::vector<std::uint8_t>(std::size_t{4} * 4 * 2)}, cfa_fields(4, 4, 16, 1),
                 false, std::move(fields));
    const tiff::Source source{file};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    const std::vector<tiff::Ifd> chain = got(container).chain(1);
    REQUIRE(chain.size() == 1);
    return dng::colour_of(chain.front(), error);
}

void check_identity(const dng::Matrix& m) {
    for (std::size_t i = 0; i < 9; ++i) {
        INFO("element " << i);
        // Within 0.002 of the identity (scale 1: also near the zeros).
        CHECK(m.at(i) == doctest::Approx(i % 4 == 0 ? 1.0 : 0.0).scale(1.0).epsilon(2e-3));
    }
}

}  // namespace

TEST_CASE("dng: colour from the file's matrices") {
    std::string error;
    SUBCASE("colour matrix: a camera that sees linear sRGB gives the identity") {
        const dng::Colour colour =
            got(colour_with({W::srationals(tag::kColorMatrix1, kXyzToSrgb),
                             W::shorts(tag::kCalibrationIlluminant1, {21}),
                             W::srationals(tag::kAsShotNeutral, {1.0, 1.0, 1.0})},
                            error));
        check_identity(colour.camera_to_srgb);
        CHECK(colour.temperature == doctest::Approx(6504).epsilon(0.01));
        CHECK(colour.exposure == 1.0);
        CHECK(colour.tone_curve.empty());
    }
    SUBCASE("forward matrix: the same camera, the same identity") {
        const dng::Colour colour =
            got(colour_with({W::srationals(tag::kColorMatrix1, kXyzToSrgb),
                             W::shorts(tag::kCalibrationIlluminant1, {21}),
                             W::srationals(tag::kForwardMatrix1, kSrgbToD50),
                             W::srationals(tag::kAsShotNeutral, {1.0, 1.0, 1.0})},
                            error));
        check_identity(colour.camera_to_srgb);
    }
    SUBCASE("a neutral of the camera is white, both ways") {
        for (const bool forward : {false, true}) {
            INFO("forward " << forward);
            std::vector<W::Field> fields{W::srationals(tag::kColorMatrix1, kXyzToSrgb),
                                         W::shorts(tag::kCalibrationIlluminant1, {21}),
                                         W::srationals(tag::kAsShotNeutral, {0.5, 1.0, 0.25})};
            if (forward) {
                fields.push_back(W::srationals(tag::kForwardMatrix1, kSrgbToD50));
            }
            const dng::Colour colour = got(colour_with(std::move(fields), error));
            CHECK(colour.neutral == dng::Vector{0.5, 1.0, 0.25});
            using dng::operator*;  // on std::array: not found by argument-dependent lookup
            const dng::Vector white = colour.camera_to_srgb * colour.neutral;
            for (const double c : white) {
                CHECK(c == doctest::Approx(1.0).epsilon(1e-3));
            }
        }
    }
    SUBCASE("two illuminants: the matrix of the one nearer the shot's white") {
        // Under standard light A the camera would be twice as sensitive: a D65 neutral must pick
        // the D65 matrix, and give the identity again.
        std::vector<double> doubled = kXyzToSrgb;
        for (double& v : doubled) {
            v *= 2.0;
        }
        const dng::Colour colour =
            got(colour_with({W::srationals(tag::kColorMatrix1, doubled),
                             W::shorts(tag::kCalibrationIlluminant1, {17}),
                             W::srationals(tag::kColorMatrix2, kXyzToSrgb),
                             W::shorts(tag::kCalibrationIlluminant2, {21}),
                             W::srationals(tag::kAsShotNeutral, {1.0, 1.0, 1.0})},
                            error));
        check_identity(colour.camera_to_srgb);
    }
    SUBCASE("exposure and tone curve") {
        const dng::Colour colour =
            got(colour_with({W::srationals(tag::kColorMatrix1, kXyzToSrgb),
                             W::srationals(tag::kBaselineExposure, {1.0}),
                             W::srationals(tag::kBaselineExposureOffset, {-0.5})},
                            error));
        CHECK(colour.exposure == doctest::Approx(std::sqrt(2.0)));
    }
    SUBCASE("no colour matrix: no colour") {
        CHECK_FALSE(colour_with({W::srationals(tag::kAsShotNeutral, {1.0, 1.0, 1.0})}, error));
        CHECK(error == "a DNG without a 3-colour ColorMatrix1");
    }
    CHECK(dng::colour_temperature(0.3127, 0.3290) == doctest::Approx(6504).epsilon(0.005));
    CHECK(dng::colour_temperature(0.4476, 0.4074) == doctest::Approx(2856).epsilon(0.005));
    CHECK(dng::illuminant_temperature(17) == 2850.0);
    CHECK(dng::illuminant_temperature(255) == 0.0);
}

namespace {

constexpr std::uint32_t kBlack = 1000;
constexpr std::uint32_t kWhite = 9000;

// A 40x30 RGGB sensor, 16-bit in one strip, every site value(colour, row) above black.
std::vector<std::uint8_t> sensor(
    const std::function<double(std::size_t colour, std::uint32_t row)>& value) {
    std::vector<std::uint8_t> strip;
    for (std::uint32_t y = 0; y < 30; ++y) {
        for (std::uint32_t x = 0; x < 40; ++x) {
            const std::size_t colour = (y % 2) + (x % 2);  // R G / G B
            const auto v = static_cast<std::uint32_t>(std::lround(value(colour, y)));
            strip.push_back(static_cast<std::uint8_t>(v));
            strip.push_back(static_cast<std::uint8_t>(v >> 8));
        }
    }
    return strip;
}

std::vector<W::Field> sensor_fields(std::vector<W::Field> more = {}) {
    std::vector<W::Field> fields = cfa_fields(40, 30, 16, 1);
    fields.push_back(W::shorts(tag::kBlackLevel, {kBlack}));
    fields.push_back(W::shorts(tag::kWhiteLevel, {kWhite}));
    fields.insert(fields.end(), more.begin(), more.end());
    return fields;
}

std::vector<W::Field> camera_fields(double red, double blue, std::uint32_t orientation = 1) {
    return {W::srationals(tag::kColorMatrix1, kXyzToSrgb),
            W::shorts(tag::kCalibrationIlluminant1, {21}),
            W::srationals(tag::kAsShotNeutral, {red, 1.0, blue}),
            W::shorts(tag::kOrientation, {orientation})};
}

std::optional<dng::Developed> develop_file(const std::vector<std::uint8_t>& file,
                                           const dng::DevelopOptions& options, std::string& error) {
    const tiff::Source source{file};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    image::Problem problem{};
    return dng::develop(got(container), options, error, problem);
}

// Whether every pixel of picture is (r, g, b), give or take one.
bool all_of(const rummage::picture::Image& picture, int r, int g, int b) {
    for (std::size_t i = 0; i < picture.rgba.size(); i += 4) {
        if (std::abs(picture.rgba[i] - r) > 1 || std::abs(picture.rgba[i + 1] - g) > 1 ||
            std::abs(picture.rgba[i + 2] - b) > 1 || picture.rgba[i + 3] != 255) {
            return false;
        }
    }
    return !picture.rgba.empty();
}

}  // namespace

TEST_CASE("dng: developed at half size, levels, balance and crop") {
    std::string error;
    // 18 % grey seen through a neutral of (0.5, 1, 0.25): linear 0.18, sRGB 118.
    const auto grey = [](std::size_t colour, std::uint32_t) {
        const std::array<double, 3> neutral{0.5, 1.0, 0.25};
        return kBlack + (0.18 * (kWhite - kBlack) * neutral.at(colour));
    };
    SUBCASE("18 % grey") {
        const dng::Developed developed = got(develop_file(
            make_dng({sensor(grey)}, sensor_fields(), false, camera_fields(0.5, 0.25, 6)),
            {.look = false}, error));
        CHECK(developed.image.width == 20);
        CHECK(developed.image.height == 15);
        CHECK(developed.full_width == 40);
        CHECK(developed.full_height == 30);
        CHECK(developed.orientation == 6);
        CHECK(all_of(developed.image, 118, 118, 118));
    }
    SUBCASE("the look keeps it grey") {
        const dng::Developed developed = got(develop_file(
            make_dng({sensor(grey)}, sensor_fields(), false, camera_fields(0.5, 0.25)), {}, error));
        const std::uint8_t level = developed.image.rgba[0];
        CHECK(all_of(developed.image, level, level, level));
    }
    SUBCASE("a sensor full in every colour stays white") {
        const dng::Developed developed =
            got(develop_file(make_dng({sensor([](std::size_t, std::uint32_t) { return kWhite; })},
                                      sensor_fields(), false, camera_fields(0.5, 0.25)),
                             {.look = false}, error));
        CHECK(all_of(developed.image, 255, 255, 255));
    }
    SUBCASE("a black for each row") {
        std::vector<double> deltas(30);
        for (std::uint32_t y = 0; y < 30; ++y) {
            deltas[y] = 3.0 * y;
        }
        const dng::Developed developed = got(develop_file(
            // Scaled between the row's black and white, as the DNG specification says.
            make_dng({sensor([&](std::size_t colour, std::uint32_t row) {
                         const std::array<double, 3> neutral{0.5, 1.0, 0.25};
                         const double black = kBlack + deltas[row];
                         return black + (0.18 * (kWhite - black) * neutral.at(colour));
                     })},
                     sensor_fields({W::srationals(tag::kBlackLevelDeltaV, deltas)}), false,
                     camera_fields(0.5, 0.25)),
            {.look = false}, error));
        CHECK(all_of(developed.image, 118, 118, 118));
    }
    SUBCASE("active area and default crop") {
        const dng::Developed developed = got(develop_file(
            make_dng({sensor(grey)},
                     sensor_fields({W::shorts(tag::kActiveArea, {2, 4, 28, 36}),
                                    W::rationals(tag::kDefaultCropOrigin, {{3, 1}, {1, 1}}),
                                    W::rationals(tag::kDefaultCropSize, {{26, 1}, {22, 1}})}),
                     false, camera_fields(0.5, 0.25)),
            {.look = false}, error));
        CHECK(developed.image.width == 13);
        CHECK(developed.image.height == 11);
        CHECK(developed.full_width == 26);
        CHECK(developed.full_height == 22);
        CHECK(all_of(developed.image, 118, 118, 118));
    }
    SUBCASE("a default crop with only its size, or only its origin (the other as by default)") {
        const dng::Developed sized = got(develop_file(
            make_dng({sensor(grey)},
                     sensor_fields({W::rationals(tag::kDefaultCropSize, {{36, 1}, {26, 1}})}),
                     false, camera_fields(0.5, 0.25)),
            {.look = false}, error));
        CHECK(sized.full_width == 36);
        CHECK(sized.full_height == 26);
        const dng::Developed placed = got(develop_file(
            make_dng({sensor(grey)},
                     sensor_fields({W::rationals(tag::kDefaultCropOrigin, {{4, 1}, {2, 1}})}),
                     false, camera_fields(0.5, 0.25)),
            {.look = false}, error));
        CHECK(placed.full_width == 36);
        CHECK(placed.full_height == 28);
        CHECK(all_of(placed.image, 118, 118, 118));
    }
    SUBCASE("for a small box, averaged by area to fit it") {
        // 20x15 at half size into 10x5: a third, 7x5 (as scale_to_fit would make it).
        const dng::Developed developed = got(
            develop_file(make_dng({sensor(grey)}, sensor_fields(), false, camera_fields(0.5, 0.25)),
                         {.look = false, .max_width = 10, .max_height = 5}, error));
        CHECK(developed.image.width == 7);
        CHECK(developed.image.height == 5);
        CHECK(all_of(developed.image, 118, 118, 118));
    }
    SUBCASE("averaged by area where a block starts right on an output pixel's edge") {
        // 18 blocks into 14: block 9 starts where output 7 does, but 9 / (18 / 14) comes out
        // just under 7 in doubles; it once went whole into output 6 (a line of pixels too bright
        // and one too dark, as across a DJI's 4000 blocks into 1920).
        const dng::Developed developed = got(develop_file(
            make_dng({sensor(grey)},
                     sensor_fields({W::rationals(tag::kDefaultCropOrigin, {{0, 1}, {0, 1}}),
                                    W::rationals(tag::kDefaultCropSize, {{36, 1}, {30, 1}})}),
                     false, camera_fields(0.5, 0.25)),
            {.look = false, .max_width = 14, .max_height = 100}, error));
        CHECK(developed.image.width == 14);
        CHECK(all_of(developed.image, 118, 118, 118));
    }
    SUBCASE("not developed: no colour, or a pattern other than Bayer") {
        CHECK_FALSE(develop_file(make_dng({sensor(grey)}, sensor_fields(), false), {}, error));
        CHECK(error == "a DNG without a 3-colour ColorMatrix1");
        std::vector<W::Field> fields = sensor_fields();
        std::ranges::find(fields, tag::kCfaPattern, &W::Field::tag)->bytes = {0, 1, 1, 1};
        CHECK_FALSE(develop_file(
            make_dng({sensor(grey)}, std::move(fields), false, camera_fields(0.5, 0.25)), {},
            error));
        CHECK(error == "a DNG whose colour filter is not a 2x2 Bayer pattern");
    }
}

TEST_CASE("dng: load() develops a DNG whose previews are too small") {
    const rummage::test::TempDir dir;
    const auto grey = [](std::size_t colour, std::uint32_t) {
        const std::array<double, 3> neutral{0.5, 1.0, 0.25};
        return kBlack + (0.18 * (kWhite - kBlack) * neutral.at(colour));
    };
    const std::vector<std::uint8_t> file =
        make_dng({sensor(grey)}, sensor_fields(), false, camera_fields(0.5, 0.25));
    const fs::path path = dir.path / "grey.dng";
    {
        std::ofstream out{path, std::ios::binary};
        out.write(reinterpret_cast<const char*>(file.data()),  // NOLINT: bytes to chars
                  static_cast<std::streamsize>(file.size()));
    }
    // 16x16 preview, asked 64: developed (20x15, the look on).
    const image::Loaded large = image::load({.path = path, .max_width = 64, .max_height = 64});
    CHECK(large.failure == image::Failure::none);
    CHECK_FALSE(large.from_preview);
    CHECK(large.image.width == 20);
    CHECK(large.image.height == 15);
    CHECK(large.full_width == 40);
    CHECK(large.full_height == 30);
    // Asked 16: the preview is enough.
    const image::Loaded small = image::load({.path = path, .max_width = 16, .max_height = 16});
    CHECK(small.failure == image::Failure::none);
    CHECK(small.from_preview);
    CHECK(small.image.width == 16);
}

namespace {

// Opcode lists as a DNG stores them: big-endian whatever the file's byte order.
class ListWriter {
public:
    static void u32(std::vector<std::uint8_t>& to, std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            to.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }

    static void f64(std::vector<std::uint8_t>& to, double v) {
        const auto bits = std::bit_cast<std::uint64_t>(v);
        u32(to, static_cast<std::uint32_t>(bits >> 32));
        u32(to, static_cast<std::uint32_t>(bits));
    }

    static void f32(std::vector<std::uint8_t>& to, float v) {
        u32(to, std::bit_cast<std::uint32_t>(v));
    }

    ListWriter& add(std::uint32_t id, std::uint32_t flags, const std::vector<std::uint8_t>& body) {
        u32(ops_, id);
        u32(ops_, 0x01030000);
        u32(ops_, flags);
        u32(ops_, static_cast<std::uint32_t>(body.size()));
        ops_.insert(ops_.end(), body.begin(), body.end());
        ++count_;
        return *this;
    }

    [[nodiscard]] std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        u32(out, count_);
        out.insert(out.end(), ops_.begin(), ops_.end());
        return out;
    }

private:
    std::vector<std::uint8_t> ops_;
    std::uint32_t count_{0};
};

constexpr std::uint32_t kGainMapId = 9;
constexpr std::uint32_t kWarpId = 1;

struct GainMapSpec {
    std::array<std::uint32_t, 4> area{0, 0, 30, 40};
    std::uint32_t plane{0};
    std::uint32_t planes{3};
    std::uint32_t row_pitch{1};
    std::uint32_t column_pitch{1};
    std::uint32_t points_v{1};
    std::uint32_t points_h{1};
    double spacing_v{1.0};
    double spacing_h{1.0};
    std::uint32_t map_planes{1};
    std::vector<float> gains{2.0F};
};

std::vector<std::uint8_t> gain_map(const GainMapSpec& m) {
    std::vector<std::uint8_t> b;
    for (const std::uint32_t v : m.area) {
        ListWriter::u32(b, v);
    }
    for (const std::uint32_t v :
         {m.plane, m.planes, m.row_pitch, m.column_pitch, m.points_v, m.points_h}) {
        ListWriter::u32(b, v);
    }
    for (const double v : {m.spacing_v, m.spacing_h, 0.0, 0.0}) {
        ListWriter::f64(b, v);
    }
    ListWriter::u32(b, m.map_planes);
    for (const float g : m.gains) {
        ListWriter::f32(b, g);
    }
    return b;
}

// WarpRectilinear with planes x (radial k0-k3, tangential) and the centre.
std::vector<std::uint8_t> warp(const std::vector<std::array<double, 6>>& planes, double cx = 0.5,
                               double cy = 0.5) {
    std::vector<std::uint8_t> b;
    ListWriter::u32(b, static_cast<std::uint32_t>(planes.size()));
    for (const std::array<double, 6>& k : planes) {
        for (const double v : k) {
            ListWriter::f64(b, v);
        }
    }
    ListWriter::f64(b, cx);
    ListWriter::f64(b, cy);
    return b;
}

W::Field opcode_list(std::uint16_t which, const ListWriter& list) {
    W::Field f = W::bytes(which, list.bytes());
    f.type = 7;  // UNDEFINED
    return f;
}

// 18 % grey through a neutral of (0.5, 1, 0.25), rows from first_bright on at 50 % instead.
std::vector<std::uint8_t> grey_sensor(std::uint32_t first_bright = 30) {
    return sensor([=](std::size_t colour, std::uint32_t row) {
        const std::array<double, 3> neutral{0.5, 1.0, 0.25};
        const double level = row >= first_bright ? 0.5 : 0.18;
        return kBlack + (level * (kWhite - kBlack) * neutral.at(colour));
    });
}

// Whether the pixels of columns [x0, x1) and rows [y0, y1) are (r, g, b), give or take one.
bool region_is(const rummage::picture::Image& picture, std::uint32_t x0, std::uint32_t x1,
               std::uint32_t y0, std::uint32_t y1, int r, int g, int b) {
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::uint8_t* p =
                picture.rgba.data() + ((std::size_t{y} * picture.width + x) * 4);
            if (std::abs(p[0] - r) > 1 || std::abs(p[1] - g) > 1 || std::abs(p[2] - b) > 1) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("dng: opcode lists") {
    std::string error;
    SUBCASE("gain maps by list; ones dropped; bad pixel fixes and optional unknowns skipped") {
        dng::Opcodes opcodes;
        ListWriter two;
        two.add(kGainMapId, 0, gain_map({.gains = {1.0F}}))
            .add(kGainMapId, 0, gain_map({.row_pitch = 2, .column_pitch = 2}))
            .add(4, 0, std::vector<std::uint8_t>(8))   // FixBadPixelsConstant
            .add(5, 0, std::vector<std::uint8_t>(12))  // FixBadPixelsList
            .add(77, 1, {})                            // unknown, optional
            .add(3, 2, std::vector<std::uint8_t>(8));  // FixVignetteRadial, skippable for previews
        REQUIRE(dng::read_list(two.bytes(), 2, opcodes, error));
        REQUIRE(opcodes.before_demosaic.size() == 1);
        CHECK(opcodes.before_demosaic[0].row_pitch == 2);
        ListWriter three;
        three.add(kGainMapId, 0, gain_map({}))
            .add(
                kWarpId, 0,
                warp({{1, 0.1, 0, 0, 0, 0}, {0.9, 0.2, 0.3, 0.4, 0.01, 0.02}, {1, 0.3, 0, 0, 0, 0}},
                     0.4, 0.6));
        REQUIRE(dng::read_list(three.bytes(), 3, opcodes, error));
        CHECK(opcodes.after_demosaic.size() == 1);
        const dng::Warp& w = got(opcodes.warp);  // green's coefficients of three planes
        CHECK(w.radial == std::array{0.9, 0.2, 0.3, 0.4});
        CHECK(w.tangential == std::array{0.01, 0.02});
        CHECK(w.centre_x == 0.4);
        CHECK(w.centre_y == 0.6);
    }
    SUBCASE("refused: what must be applied and is not known, damage, a gain map after the warp") {
        const auto refused = [&](const ListWriter& list, int number) {
            dng::Opcodes opcodes;
            return !dng::read_list(list.bytes(), number, opcodes, error);
        };
        CHECK(refused(ListWriter{}.add(2, 0, std::vector<std::uint8_t>(8)), 3));
        CHECK(error ==
              "a DNG asking for WarpFisheye in opcode list 3, which rummage does not apply");
        CHECK(refused(ListWriter{}.add(kGainMapId, 0, gain_map({})), 1));
        CHECK(error == "a DNG asking for GainMap in opcode list 1, which rummage does not apply");
        CHECK(refused(ListWriter{}.add(kWarpId, 0, warp({{1, 0, 0, 0, 0, 0}})), 2));
        CHECK(refused(ListWriter{}
                          .add(kWarpId, 0, warp({{1, 0, 0, 0, 0, 0}}))
                          .add(kGainMapId, 0, gain_map({})),
                      3));
        CHECK(error == "a DNG with a GainMap after its WarpRectilinear");
        std::vector<std::uint8_t> short_map = gain_map({});
        short_map.resize(short_map.size() - 1);
        CHECK(refused(ListWriter{}.add(kGainMapId, 0, short_map), 3));
        CHECK(error == "a DNG with a damaged GainMap");
        CHECK(refused(ListWriter{}.add(kGainMapId, 0, gain_map({.gains = {-1.0F}})), 3));
        CHECK(refused(ListWriter{}.add(kGainMapId, 0, gain_map({.gains = {NAN}})), 3));
        CHECK(refused(
            ListWriter{}.add(kGainMapId, 0,
                             gain_map({.points_v = 2, .spacing_v = 0.0, .gains = {2.0F, 2.0F}})),
            3));
        CHECK(refused(ListWriter{}.add(kWarpId, 0, warp({{NAN, 0, 0, 0, 0, 0}})), 3));
        std::vector<std::uint8_t> cut = ListWriter{}.add(kGainMapId, 0, gain_map({})).bytes();
        cut.resize(cut.size() - 3);
        dng::Opcodes opcodes;
        CHECK_FALSE(dng::read_list(cut, 3, opcodes, error));
        CHECK(error == "a DNG with a damaged opcode list");
    }
    SUBCASE("a gain map between its points, and its edge beyond them") {
        dng::Opcodes opcodes;
        REQUIRE(dng::read_list(
            ListWriter{}
                .add(kGainMapId, 0,
                     gain_map({.points_v = 2, .points_h = 2, .gains = {1.0F, 2.0F, 3.0F, 4.0F}}))
                .bytes(),
            3, opcodes, error));
        const dng::GainMap& map = opcodes.after_demosaic.at(0);
        CHECK(map.at(0.0, 0.0, 0) == doctest::Approx(1.0));
        CHECK(map.at(0.5, 0.5, 0) == doctest::Approx(2.5));
        CHECK(map.at(1.0, 0.25, 2) == doctest::Approx(3.25));
        CHECK(map.at(-1.0, 7.0, 0) == doctest::Approx(2.0));
    }
    SUBCASE("the warp: from the optical centre, radially") {
        const dng::Warp half{
            .radial = {0.5, 0.0, 0.0, 0.0}, .tangential = {}, .centre_x = 0.5, .centre_y = 0.5};
        const dng::WarpMap map{half, 40.0, 30.0};
        CHECK(map.source(20.0, 15.0) == std::array{20.0, 15.0});
        CHECK(map.source(0.0, 0.0) == std::array{10.0, 7.5});
        CHECK(map.source(40.0, 15.0) == std::array{30.0, 15.0});
        // r^2 = 1 at the corners: 1 + 1 + 1 + 1 = 4 times as far.
        const dng::Warp radial{
            .radial = {1.0, 1.0, 1.0, 1.0}, .tangential = {}, .centre_x = 0.5, .centre_y = 0.5};
        const std::array<double, 2> corner = dng::WarpMap{radial, 40.0, 30.0}.source(40.0, 30.0);
        CHECK(corner[0] == doctest::Approx(20.0 + (4 * 20.0)));
        CHECK(corner[1] == doctest::Approx(15.0 + (4 * 15.0)));
    }
}

TEST_CASE("dng: developed with the file's opcodes") {
    std::string error;
    SUBCASE("a gain map after demosaic on the right half: twice as bright there") {
        ListWriter list;
        list.add(kGainMapId, 0, gain_map({.area = {0, 20, 30, 40}}));
        const std::vector<std::uint8_t> file =
            make_dng({grey_sensor()}, sensor_fields({opcode_list(tag::kOpcodeList3, list)}), false,
                     camera_fields(0.5, 0.25));
        const dng::Developed developed = got(develop_file(file, {.look = false}, error));
        CHECK(region_is(developed.image, 0, 10, 0, 15, 118, 118, 118));
        CHECK(region_is(developed.image, 10, 20, 0, 15, 162, 162, 162));
        // Left out on request.
        const dng::Developed plain =
            got(develop_file(file, {.look = false, .opcodes = false}, error));
        CHECK(all_of(plain.image, 118, 118, 118));
    }
    // A gain map before demosaic develops as the sensor with those sites multiplied would.
    const auto scaled_sensor = [](std::array<double, 3> gains) {
        return sensor([=](std::size_t colour, std::uint32_t) {
            const std::array<double, 3> neutral{0.5, 1.0, 0.25};
            return kBlack + (0.18 * gains.at(colour) * (kWhite - kBlack) * neutral.at(colour));
        });
    };
    const auto same = [](const rummage::picture::Image& a, const rummage::picture::Image& b) {
        if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.rgba.size(); ++i) {
            if (std::abs(a.rgba[i] - b.rgba[i]) > 1) {
                return false;
            }
        }
        return true;
    };
    SUBCASE("a gain map before demosaic on the red sites only") {
        ListWriter list;
        list.add(kGainMapId, 0, gain_map({.planes = 1, .row_pitch = 2, .column_pitch = 2}));
        const dng::Developed developed = got(develop_file(
            make_dng({grey_sensor()}, sensor_fields({opcode_list(tag::kOpcodeList2, list)}), false,
                     camera_fields(0.5, 0.25)),
            {.look = false}, error));
        const dng::Developed expected =
            got(develop_file(make_dng({scaled_sensor({2.0, 1.0, 1.0})}, sensor_fields(), false,
                                      camera_fields(0.5, 0.25)),
                             {.look = false}, error));
        CHECK(same(developed.image, expected.image));
        CHECK(developed.image.rgba[0] > developed.image.rgba[1] + 20);
    }
    SUBCASE("a gain map before demosaic on one green of the two: their average") {
        ListWriter list;
        list.add(kGainMapId, 0,
                 gain_map({.area = {0, 1, 30, 40},
                           .planes = 1,
                           .row_pitch = 2,
                           .column_pitch = 2,
                           .gains = {3.0F}}));
        const dng::Developed developed = got(develop_file(
            make_dng({grey_sensor()}, sensor_fields({opcode_list(tag::kOpcodeList2, list)}), false,
                     camera_fields(0.5, 0.25)),
            {.look = false}, error));
        const dng::Developed expected =  // (3 + 1) / 2
            got(develop_file(make_dng({scaled_sensor({1.0, 2.0, 1.0})}, sensor_fields(), false,
                                      camera_fields(0.5, 0.25)),
                             {.look = false}, error));
        CHECK(same(developed.image, expected.image));
    }
    SUBCASE("the lens warp: twice as large from the centre") {
        // Bright from row 8 (the picture's row 4) down; at twice the size, from row 0.
        ListWriter list;
        list.add(kWarpId, 0, warp({{0.5, 0, 0, 0, 0, 0}}));
        const std::vector<std::uint8_t> file =
            make_dng({grey_sensor(8)}, sensor_fields({opcode_list(tag::kOpcodeList3, list)}), false,
                     camera_fields(0.5, 0.25));
        const dng::Developed plain =
            got(develop_file(file, {.look = false, .opcodes = false}, error));
        CHECK(region_is(plain.image, 0, 20, 0, 4, 118, 118, 118));
        CHECK(region_is(plain.image, 0, 20, 4, 15, 188, 188, 188));
        const dng::Developed developed = got(develop_file(file, {.look = false}, error));
        CHECK(region_is(developed.image, 0, 20, 1, 15, 188, 188, 188));
        CHECK(developed.image.width == 20);
        CHECK(developed.image.height == 15);
    }
    SUBCASE("an opcode rummage does not apply: not developed (the previews are shown)") {
        ListWriter list;
        list.add(2, 0, std::vector<std::uint8_t>(8));  // WarpFisheye
        const std::vector<std::uint8_t> file =
            make_dng({grey_sensor()}, sensor_fields({opcode_list(tag::kOpcodeList3, list)}), false,
                     camera_fields(0.5, 0.25));
        CHECK_FALSE(develop_file(file, {}, error));
        CHECK(error ==
              "a DNG asking for WarpFisheye in opcode list 3, which rummage does not apply");
        CHECK(develop_file(file, {.opcodes = false}, error));
        const rummage::test::TempDir dir;
        const fs::path path = dir.path / "fisheye.dng";
        {
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(file.data()),  // NOLINT: bytes to chars
                      static_cast<std::streamsize>(file.size()));
        }
        const image::Loaded loaded = image::load({.path = path, .max_width = 64, .max_height = 64});
        CHECK(loaded.failure == image::Failure::none);
        CHECK(loaded.from_preview);
    }
}
