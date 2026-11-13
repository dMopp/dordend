#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/formats/image/decode.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/picture/wuffs.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace picture = rummage::picture;
using rummage::test::TempDir;

namespace {

const fs::path kTiff = fs::path{RUMMAGE_TEST_DATA} / "image" / "tiff";

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, {}};
}

void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT: bytes to chars
              static_cast<std::streamsize>(bytes.size()));
}

// The largest difference of any channel between two pictures of the same size (the colour of
// fully transparent pixels does not count); 256 when they differ in size.
int largest_difference(const picture::Image& a, const picture::Image& b) {
    if (a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
        return 256;
    }
    int largest = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        const std::size_t alpha = i - (i % 4) + 3;
        if (a.rgba[alpha] == 0 && b.rgba[alpha] == 0) {
            continue;
        }
        largest = std::max(largest, std::abs(int{a.rgba[i]} - int{b.rgba[i]}));
    }
    return largest;
}

}  // namespace

// Every fixture against ImageMagick's own decoding of it (libtiff inside): tests/data/image/
// tiff/make.sh writes each NAME.tif with its NAME.png.
TEST_CASE("tiff: every kind of TIFF decodes exactly as libtiff does") {
    int checked = 0;
    for (const auto& entry : fs::directory_iterator{kTiff}) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".tif" || name.starts_with("broken")) {
            continue;
        }
        INFO(name);
        const image::Decoded ours = image::decode_file(entry.path(), {.apply_orientation = false});
        REQUIRE(ours.error.empty());
        CHECK(ours.format == image::Format::tiff);
        fs::path reference = entry.path();
        reference.replace_extension(".png");
        std::string error;
        const auto theirs = picture::decode_png(read_bytes(reference), error);
        REQUIRE(theirs.has_value());
        CHECK(largest_difference(ours.image, theirs.value_or(picture::Image{})) == 0);
        ++checked;
    }
    CHECK(checked == 36);
    // The first page of two; the Orientation tag reported, turned when asked.
    CHECK(image::decode_file(kTiff / "two-pages.tif").image.width == 37);
    const image::Decoded turned = image::decode_file(kTiff / "orient6.tif");
    CHECK(turned.orientation == 6);
    CHECK(turned.image.width == 29);
}

TEST_CASE("tiff: plain TIFFs are shown without exiv2, camera raws keep their previews") {
    const std::uint64_t before = image::embedded_preview_reads();
    const image::Loaded big = image::load(
        {.path = kTiff / "rgb16-zip-predictor.tif", .max_width = 1024, .max_height = 1024});
    REQUIRE(big.failure == image::Failure::none);
    CHECK_FALSE(big.from_preview);
    CHECK(big.full_width == 37);
    const image::Loaded turned =
        image::load({.path = kTiff / "orient6.tif", .max_width = 1024, .max_height = 1024});
    CHECK(turned.image.width == 29);  // upright
    CHECK(turned.full_width == 29);
    CHECK(image::embedded_preview_reads() == before);
}

namespace {

// A little-endian TIFF of uncompressed RGB images: IFD0 a reduced one (2x2 grey) whose SubIFD is
// the full-resolution one (4x3 red), as a film scanner writes it.
std::vector<std::uint8_t> reduced_first_with_sub_ifd() {
    std::vector<std::uint8_t> out{'I', 'I', 42, 0, 0, 0, 0, 0};
    const auto put16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto put32 = [&](std::uint32_t v) {
        put16(v & 0xFFFFU);
        put16(v >> 16);
    };
    const auto set32 = [&](std::size_t at, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) {
            out.at(at + i) = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    const auto pixels = [&](std::uint32_t count, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const auto at = static_cast<std::uint32_t>(out.size());
        for (std::uint32_t i = 0; i < count; ++i) {
            out.insert(out.end(), {r, g, b});
        }
        if (out.size() % 2 != 0) {
            out.push_back(0);
        }
        return at;
    };
    // An IFD of an RGB image width x height at strip; sub (0 none) its SubIFD's offset, written
    // later through the returned place.
    const auto ifd = [&](std::uint32_t subfile, std::uint32_t width, std::uint32_t height,
                         std::uint32_t strip, bool with_sub) {
        const auto at = static_cast<std::uint32_t>(out.size());
        const std::uint16_t count = with_sub ? 10 : 9;
        put16(count);
        const auto entry = [&](std::uint16_t tag, std::uint16_t type, std::uint32_t value) {
            put16(tag);
            put16(type);
            put32(1);
            put32(value);  // a SHORT sits in the low half (little-endian)
        };
        entry(254, 4, subfile);
        entry(256, 4, width);
        entry(257, 4, height);
        entry(258, 3, 8);  // one value for all samples
        entry(259, 3, 1);
        entry(262, 3, 2);
        entry(273, 4, strip);
        entry(277, 3, 3);
        entry(279, 4, width * height * 3);
        std::size_t sub_place = 0;
        if (with_sub) {
            put16(330);
            put16(4);
            put32(1);
            sub_place = out.size();
            put32(0);
        }
        put32(0);  // the end of the chain
        return std::pair{at, sub_place};
    };
    const std::uint32_t grey = pixels(4, 128, 128, 128);
    const std::uint32_t red = pixels(12, 255, 0, 0);
    const auto [first, sub_place] = ifd(1, 2, 2, grey, true);
    const auto [full, unused] = ifd(0, 4, 3, red, false);
    set32(4, first);
    set32(sub_place, full);
    return out;
}

}  // namespace

TEST_CASE("tiff: a reduced image first, the full one in a SubIFD: the full one is shown") {
    const TempDir dir;
    const fs::path path = dir.path / "scan.tif";
    write_bytes(path, reduced_first_with_sub_ifd());
    const image::Loaded loaded = image::load({.path = path, .max_width = 64, .max_height = 64});
    REQUIRE(loaded.failure == image::Failure::none);
    CHECK_FALSE(loaded.from_preview);
    CHECK(loaded.image.width == 4);
    CHECK(loaded.image.height == 3);
    CHECK(loaded.image.rgba[0] == 255);
    CHECK(loaded.image.rgba[1] == 0);
}

TEST_CASE("tiff: broken TIFFs are damaged, never a crash") {
    const TempDir dir;
    const std::vector<std::uint8_t> good = read_bytes(kTiff / "rgb8-none.tif");
    const auto load = [&](const std::vector<std::uint8_t>& bytes) {
        write_bytes(dir.path / "test.tif", bytes);
        return image::load({.path = dir.path / "test.tif", .max_width = 64, .max_height = 64});
    };
    REQUIRE(load(good).failure == image::Failure::none);
    CHECK(image::load({.path = kTiff / "broken-truncated.tif", .max_width = 64, .max_height = 64})
              .failure == image::Failure::damaged);
    // Found by the fuzzer: a TileWidth slipped into a TIFF of strips asked for gigabytes of tiles
    // (4.6 s); now refused at once.
    const auto start = std::chrono::steady_clock::now();
    CHECK(image::load({.path = kTiff / "broken-tile-tag.tif", .max_width = 64, .max_height = 64})
              .failure == image::Failure::damaged);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds{200});
    // Found by the fuzzer of the whole family (2e-3): 3 kB of one uncompressed strip claiming
    // 172 MP and 151 MP, decoded as zeros in 2.9 s and 0.9 s; now refused before allocating.
    for (const char* name : {"broken-huge-none.tif", "broken-huge-none-2.tif"}) {
        INFO(name);
        const auto at = std::chrono::steady_clock::now();
        CHECK(image::load({.path = kTiff / name, .max_width = 64, .max_height = 64}).failure !=
              image::Failure::none);
        CHECK(std::chrono::steady_clock::now() - at < std::chrono::milliseconds{200});
    }
    // Found by the fuzzer of the whole family: not a TIFF this reader shows, so its stored
    // previews are asked of exiv2, whose safe arithmetic throws std::overflow_error. A plain
    // reason, never an exception out of load().
    CHECK(image::load(
              {.path = kTiff / "broken-exiv2-overflow.tif", .max_width = 64, .max_height = 64})
              .failure != image::Failure::none);
    // Every single byte of the header and the IFD set to 0xFF in turn: never a crash, and a
    // picture or a plain reason each time.
    const std::vector<std::uint8_t> lzw = read_bytes(kTiff / "rgb8-lzw.tif");
    for (std::size_t i = 0; i < lzw.size(); ++i) {
        std::vector<std::uint8_t> bent = lzw;
        bent[i] = 0xFF;
        const image::Loaded loaded = load(bent);
        const bool shown = loaded.failure == image::Failure::none;
        CHECK(shown != loaded.image.empty());
    }
    // A size beyond the limit is refused before anything is allocated.
    std::vector<std::uint8_t> huge = good;
    const std::size_t width_entry = [&] {
        // Find ImageWidth (256, SHORT or LONG) in the first IFD, little-endian.
        const std::uint32_t ifd = std::uint32_t{good[4]} | (std::uint32_t{good[5]} << 8) |
                                  (std::uint32_t{good[6]} << 16) | (std::uint32_t{good[7]} << 24);
        const std::size_t count = std::size_t{good[ifd]} | (std::size_t{good[ifd + 1]} << 8);
        for (std::size_t e = 0; e < count; ++e) {
            const std::size_t at = ifd + 2 + (e * 12);
            if ((good[at] | (good[at + 1] << 8)) == 256) {
                return at;
            }
        }
        return std::size_t{0};
    }();
    REQUIRE(width_entry != 0);
    huge[width_entry + 2] = 4;  // LONG
    huge[width_entry + 4] = 1;  // count 1
    huge[width_entry + 8] = 0x00;
    huge[width_entry + 9] = 0x00;
    huge[width_entry + 10] = 0x00;
    huge[width_entry + 11] = 0x01;  // 0x01000000: 16 777 216 wide, past the 2^20 limit
    CHECK(load(huge).failure == image::Failure::damaged);
}
