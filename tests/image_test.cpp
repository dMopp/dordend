#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/formats/image/decode.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/formats/image/photo.hpp"
#include "rummage/formats/image/preview.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/picture/scale.hpp"

namespace image = rummage::formats::image;
namespace picture = rummage::picture;
using image::Format;
using picture::Image;

namespace {

const std::filesystem::path kData = std::filesystem::path{RUMMAGE_TEST_DATA} / "image";

struct Rgba {
    int r, g, b, a;
};

// The blocks fixtures (tests/data/image/make.sh): 48x32, six 16x16 blocks.
//   red    green  blue
//   white* black  yellow   (* alpha 128 where the format has alpha)
constexpr std::array<Rgba, 6> kBlocks{{{255, 0, 0, 255},
                                       {0, 255, 0, 255},
                                       {0, 0, 255, 255},
                                       {255, 255, 255, 128},
                                       {0, 0, 0, 255},
                                       {255, 255, 0, 255}}};

Rgba pixel(const Image& picture, std::size_t x, std::size_t y) {
    const std::size_t at = ((y * picture.width) + x) * 4;
    return {picture.rgba[at], picture.rgba[at + 1], picture.rgba[at + 2], picture.rgba[at + 3]};
}

// The center of each block matches within tolerance (JPEG is lossy); with alpha false the
// half-transparent block is checked as white over white (the opaque formats).
void check_blocks(const Image& picture, int tolerance, bool alpha) {
    REQUIRE(picture.width == 48);
    REQUIRE(picture.height == 32);
    for (std::size_t block = 0; block < kBlocks.size(); ++block) {
        Rgba expected = kBlocks.at(block);
        if (!alpha) {
            expected.a = 255;
        }
        const Rgba actual = pixel(picture, ((block % 3) * 16) + 8, ((block / 3) * 16) + 8);
        INFO("block " << block);
        CHECK(std::abs(actual.r - expected.r) <= tolerance);
        CHECK(std::abs(actual.g - expected.g) <= tolerance);
        CHECK(std::abs(actual.b - expected.b) <= tolerance);
        CHECK(actual.a == expected.a);
    }
}

// A width x height image whose pixel i has red = i.
Image numbered(std::uint32_t width, std::uint32_t height) {
    Image picture{.width = width, .height = height, .rgba = {}};
    picture.rgba.resize(std::size_t{width} * height * 4);
    for (std::size_t i = 0; i < std::size_t{width} * height; ++i) {
        picture.rgba[i * 4] = static_cast<std::uint8_t>(i);
        picture.rgba[(i * 4) + 3] = 255;
    }
    return picture;
}

std::vector<int> reds(const Image& picture) {
    std::vector<int> out;
    for (std::size_t i = 0; i < picture.rgba.size(); i += 4) {
        out.push_back(picture.rgba[i]);
    }
    return out;
}

}  // namespace

TEST_CASE("image: formats are recognized by their first bytes") {
    CHECK(image::decode_file(kData / "blocks.png").format == Format::png);
    CHECK(image::decode_file(kData / "blocks.jpg").format == Format::jpeg);
    CHECK(image::decode_file(kData / "blocks.gif").format == Format::gif);
    CHECK(image::decode_file(kData / "blocks.webp").format == Format::webp);
    CHECK(image::decode_file(kData / "blocks.jxl").format == Format::jxl);
    const std::string text = "just some text, not an image";
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    CHECK(image::sniff(bytes) == Format::unknown);
    CHECK(image::sniff({}) == Format::unknown);
}

TEST_CASE("image: every format decodes to the same pixels") {
    for (const char* name : {"blocks.png", "blocks.webp", "blocks.jxl"}) {
        INFO(name);
        const image::Decoded decoded = image::decode_file(kData / name);
        REQUIRE(decoded.error.empty());
        check_blocks(decoded.image, 0, true);
    }
    const image::Decoded gif = image::decode_file(kData / "blocks.gif");
    REQUIRE(gif.error.empty());
    check_blocks(gif.image, 0, false);
    const image::Decoded jpeg = image::decode_file(kData / "blocks.jpg");
    REQUIRE(jpeg.error.empty());
    check_blocks(jpeg.image, 6, false);
    CHECK(jpeg.orientation == 1);
    // An animated WebP: its first frame.
    const image::Decoded animated = image::decode_file(kData / "blocks-anim.webp");
    REQUIRE(animated.error.empty());
    check_blocks(animated.image, 0, true);
}

TEST_CASE("image: the EXIF orientation of a JPEG turns the pixels, unless asked not to") {
    const image::Decoded turned = image::decode_file(kData / "blocks-orient6.jpg");
    REQUIRE(turned.error.empty());
    CHECK(turned.orientation == 6);
    REQUIRE(turned.image.width == 32);
    REQUIRE(turned.image.height == 48);
    // A quarter clockwise: the left column, bottom up, is now the top row.
    CHECK(pixel(turned.image, 8, 8).r > 240);  // white
    CHECK(pixel(turned.image, 8, 8).b > 240);
    CHECK(pixel(turned.image, 24, 8).r > 240);  // red
    CHECK(pixel(turned.image, 24, 8).g < 16);
    CHECK(pixel(turned.image, 24, 40).b > 240);  // blue at the bottom right
    CHECK(pixel(turned.image, 24, 40).r < 16);

    const image::Decoded stored =
        image::decode_file(kData / "blocks-orient6.jpg", {.apply_orientation = false});
    CHECK(stored.orientation == 6);
    check_blocks(stored.image, 6, false);
}

TEST_CASE("image: the eight orientations") {
    const Image picture = numbered(3, 2);  // 0 1 2 / 3 4 5
    const auto turned = [&](std::uint8_t orientation) {
        return picture::apply_orientation(picture, orientation);
    };
    CHECK(reds(turned(1)) == std::vector<int>{0, 1, 2, 3, 4, 5});
    CHECK(reds(turned(2)) == std::vector<int>{2, 1, 0, 5, 4, 3});
    CHECK(reds(turned(3)) == std::vector<int>{5, 4, 3, 2, 1, 0});
    CHECK(reds(turned(4)) == std::vector<int>{3, 4, 5, 0, 1, 2});
    CHECK(reds(turned(5)) == std::vector<int>{0, 3, 1, 4, 2, 5});
    CHECK(reds(turned(6)) == std::vector<int>{3, 0, 4, 1, 5, 2});
    CHECK(reds(turned(7)) == std::vector<int>{5, 2, 4, 1, 3, 0});
    CHECK(reds(turned(8)) == std::vector<int>{2, 5, 1, 4, 0, 3});
    CHECK(turned(6).width == 2);
    CHECK(turned(6).height == 3);
    CHECK(reds(turned(9)) == reds(picture));  // not an orientation: as it is
}

TEST_CASE("image: EXIF orientation in both byte orders, and garbage") {
    // TIFF header, IFD0 at 8 with one entry: tag 0x0112, SHORT, count 1, value 8.
    const std::vector<std::uint8_t> little{'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01,
                                           3,   0,   1,  0, 0, 0, 8, 0, 0, 0, 0,    0};
    const std::vector<std::uint8_t> big{'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12,
                                        0,   3,   0, 0,  0, 1, 0, 3, 0, 0, 0,    0};
    CHECK(image::exif_orientation(little) == 8);
    CHECK(image::exif_orientation(big) == 3);
    std::vector<std::uint8_t> prefixed{'E', 'x', 'i', 'f', 0, 0};
    prefixed.insert(prefixed.end(), big.begin(), big.end());
    CHECK(image::exif_orientation(prefixed) == 3);

    std::vector<std::uint8_t> bad_offset = little;
    bad_offset[4] = 200;  // IFD past the end
    CHECK(image::exif_orientation(bad_offset) == 1);
    const std::vector<std::uint8_t> cut(little.begin(), little.begin() + 15);
    CHECK(image::exif_orientation(cut) == 1);
    CHECK(image::exif_orientation({}) == 1);
}

TEST_CASE("image: bad input fails with a reason, never a crash") {
    const image::Decoded truncated = image::decode_file(kData / "truncated.jpg");
    CHECK_FALSE(truncated.error.empty());
    CHECK(truncated.image.empty());

    const image::Decoded missing = image::decode_file(kData / "no-such-file.png");
    CHECK_FALSE(missing.error.empty());

    const std::vector<std::uint8_t> header_only{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const image::Decoded png = image::decode(header_only);
    CHECK(png.format == Format::png);
    CHECK_FALSE(png.error.empty());

    for (const char* name : {"blocks.png", "blocks.jpg", "blocks.webp", "blocks.jxl"}) {
        INFO(name);
        const image::Decoded too_large =
            image::decode_file(kData / name, {.max_pixels = 1000});  // 48x32 = 1536
        CHECK_FALSE(too_large.error.empty());
        CHECK(too_large.image.empty());
    }
}

TEST_CASE("image: scale_to_fit keeps proportions, averages, and never enlarges") {
    Image gray{.width = 300, .height = 200, .rgba = {}};
    gray.rgba.resize(std::size_t{300} * 200 * 4);
    for (std::size_t i = 0; i < gray.rgba.size(); i += 4) {
        const bool white = (i / 4) % 2 == 0;  // alternate black and white columns
        gray.rgba[i] = gray.rgba[i + 1] = gray.rgba[i + 2] = white ? 255 : 0;
        gray.rgba[i + 3] = 255;
    }
    const Image small = picture::scale_to_fit(gray, 128, 128);
    CHECK(small.width == 128);
    CHECK(small.height == 85);
    // Stripes average to mid gray, through the halving and the area filter alike.
    CHECK(std::abs(pixel(small, 64, 40).r - 128) <= 2);
    CHECK(pixel(small, 64, 40).a == 255);
    const Image tiny = picture::scale_to_fit(gray, 7, 1000);  // ratio 1:42.9, through halvings
    CHECK(tiny.width == 7);
    CHECK(tiny.height == 5);
    CHECK(std::abs(pixel(tiny, 3, 2).r - 128) <= 2);

    const Image same = picture::scale_to_fit(gray, 1000, 1000);
    CHECK(same.width == 300);
    CHECK(same.height == 200);
}

TEST_CASE("image: transparent pixels do not darken what they are averaged with") {
    // Red, and transparent black: the average is half-transparent red, not dark red.
    Image half{.width = 2, .height = 1, .rgba = {255, 0, 0, 255, 0, 0, 0, 0}};
    const Image one = picture::scale_to_fit(half, 1, 1);
    REQUIRE(one.width == 1);
    CHECK(pixel(one, 0, 0).r == 255);
    CHECK(std::abs(pixel(one, 0, 0).a - 128) <= 1);
}

TEST_CASE("image: PNG written with text chunks reads back the same") {
    const image::Decoded blocks = image::decode_file(kData / "blocks.png");
    REQUIRE(blocks.error.empty());
    const picture::PngText text{{"Thumb::URI", "file:///a%20b.jpg"},
                                {"Thumb::MTime", "1700000000"}};
    const std::vector<std::uint8_t> png = picture::encode_png(blocks.image, text);
    REQUIRE_FALSE(png.empty());
    CHECK(picture::png_text(png) == text);
    const image::Decoded back = image::decode(png);
    REQUIRE(back.error.empty());
    CHECK(back.image.rgba == blocks.image.rgba);  // lossless, alpha included
    CHECK(picture::png_text(std::vector<std::uint8_t>{1, 2, 3}).empty());
}

TEST_CASE("image: an embedded EXIF thumbnail is found, with the main image's orientation") {
    const image::EmbeddedPreview preview =
        image::embedded_preview(kData / "with-thumbnail.jpg", 16);
    CHECK(preview.error.empty());
    REQUIRE_FALSE(preview.data.empty());
    CHECK_FALSE(preview.tiff);
    CHECK(preview.width == 192);
    CHECK(preview.height == 128);
    CHECK(preview.orientation == 6);
    const image::Decoded decoded = image::decode_preview(preview);
    REQUIRE(decoded.error.empty());
    REQUIRE(decoded.image.width == 192);
    CHECK(pixel(decoded.image, 32, 32).r > 240);  // red block, top left
    CHECK(pixel(decoded.image, 32, 32).g < 16);

    // None in a plain file, and an error only when the file cannot be read.
    const image::EmbeddedPreview none = image::embedded_preview(kData / "blocks.png", 16);
    CHECK(none.data.empty());
    CHECK(none.error.empty());
    const image::EmbeddedPreview missing = image::embedded_preview(kData / "nothing.jpg", 16);
    CHECK(missing.data.empty());
    CHECK_FALSE(missing.error.empty());
}

TEST_CASE("image: an uncompressed TIFF preview is read, strip by strip") {
    const image::EmbeddedPreview preview = image::embedded_preview(kData / "tiff-preview.tif", 16);
    CHECK(preview.error.empty());
    REQUIRE_FALSE(preview.data.empty());
    CHECK(preview.tiff);
    CHECK(preview.width == 96);
    CHECK(preview.height == 64);
    CHECK_FALSE(preview.crop);
    const image::Decoded decoded = image::decode_preview(preview);
    REQUIRE(decoded.error.empty());
    REQUIRE(decoded.image.width == 96);
    REQUIRE(decoded.image.height == 64);
    // Each block is 32x32: the centers, and the last rows (in the short last strip).
    for (std::size_t block = 0; block < kBlocks.size(); ++block) {
        const Rgba expected = kBlocks.at(block);
        const Rgba actual = pixel(decoded.image, ((block % 3) * 32) + 16, ((block / 3) * 32) + 16);
        INFO("block " << block);
        CHECK(actual.r == expected.r);
        CHECK(actual.g == expected.g);
        CHECK(actual.b == expected.b);
        CHECK(actual.a == 255);
    }
    CHECK(pixel(decoded.image, 80, 63).r == 255);  // yellow, bottom right
    CHECK(pixel(decoded.image, 80, 63).b == 0);

    // A cut TIFF fails with a reason; a crop that does not fit leaves the preview whole.
    image::EmbeddedPreview cut = preview;
    cut.data.resize(cut.data.size() / 2);
    CHECK_FALSE(image::decode_preview(cut).error.empty());
    image::EmbeddedPreview cropped = preview;
    cropped.crop = image::PreviewCrop{.x = 32, .y = 0, .width = 64, .height = 32};
    const image::Decoded part = image::decode_preview(cropped);
    REQUIRE(part.image.width == 64);
    REQUIRE(part.image.height == 32);
    CHECK(pixel(part.image, 16, 16).g == 255);  // green, now top left
    CHECK(pixel(part.image, 16, 16).r == 0);
    cropped.crop = image::PreviewCrop{.x = 64, .y = 0, .width = 64, .height = 32};
    CHECK(image::decode_preview(cropped).image.width == 96);
}

TEST_CASE("image: a photo's shot from EXIF and its upright size; no sidecar rating") {
    const picture::PhotoInfo shot = image::read_photo_info(kData / "shot.jpg");
    CHECK(shot.width == 32);  // 48x32 stored, orientation 6
    CHECK(shot.height == 48);
    CHECK(shot.camera == "OM-1");  // the maker's long name left out
    CHECK(shot.lens == "OLYMPUS M.12-100mm F4.0");
    CHECK(shot.exposure_time == "1/2000 s");
    CHECK(shot.aperture == "F5.6");
    CHECK(shot.iso == "200");
    CHECK(shot.focal_length == "25.0 mm");
    CHECK(shot.taken == "2026-08-31 16:34");
    // shot.jpg.xmp next to it rates it 3: not read since 2e-4 (editors' sidecars are a
    // plugin's; rummage's own rating comes with ROADMAP 5).
    CHECK_FALSE(shot.rating);

    const picture::PhotoInfo plain = image::read_photo_info(kData / "blocks.png");
    CHECK(plain.width == 48);
    CHECK(plain.camera.empty());
    CHECK_FALSE(plain.rating);
    const picture::PhotoInfo missing = image::read_photo_info(kData / "nothing.jpg");
    CHECK(missing.width == 0);
}

// Binary PGM and PPM, built here: 8 and 16 bits, comments in the header, broken ones refused.
TEST_CASE("image: PGM and PPM") {
    const auto bytes = [](std::string_view header, std::initializer_list<std::uint8_t> pixels) {
        std::vector<std::uint8_t> data(header.begin(), header.end());
        data.insert(data.end(), pixels);
        return data;
    };
    const std::vector<std::uint8_t> ppm =
        bytes("P6\n# made by hand\n2 1\n255\n", {255, 0, 0, 0, 128, 255});
    CHECK(image::sniff(ppm) == Format::pnm);
    const image::Decoded rgb = image::decode(ppm);
    REQUIRE(rgb.error.empty());
    CHECK(rgb.format == Format::pnm);
    CHECK(rgb.image.width == 2);
    CHECK(rgb.image.height == 1);
    CHECK(rgb.image.rgba == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 128, 255, 255});
    // 16-bit samples are big-endian; maxval 1000 scales to 0-255.
    const image::Decoded gray = image::decode(bytes("P5 1 1 1000 ", {0x01, 0xF4}));
    REQUIRE(gray.error.empty());
    CHECK(gray.image.rgba == std::vector<std::uint8_t>{128, 128, 128, 255});
    CHECK_FALSE(image::decode(bytes("P6\n2 1\n255\n", {255, 0, 0})).error.empty());  // cut short
    CHECK_FALSE(image::decode(bytes("P6\n2 x\n255\n", {})).error.empty());           // no height
    CHECK_FALSE(image::decode(bytes("P6\n1 1\n70000\n", {0, 0, 0, 0, 0, 0})).error.empty());
    CHECK_FALSE(image::decode(bytes("P6\n1 1\n255", {})).error.empty());  // no pixels at all
    CHECK(image::decode(bytes("P6\n99999 99999\n255\n", {}), {.max_pixels = 1000}).error ==
          "image too large");
    CHECK(image::sniff(bytes("P3\n1 1\n255\n", {})) == Format::unknown);  // ASCII: not ours
}

// Raw files of many cameras (rawsamples.ch and others; not in the repository): the folder named
// by RUMMAGE_RAW_SAMPLES, when it is set. The checks name the files they need.
namespace {
const std::filesystem::path kRawSamples = [] {
    const char* folder = std::getenv("RUMMAGE_RAW_SAMPLES");
    return std::filesystem::path{folder != nullptr ? folder : ""};
}();

// The samples that carry a preview rummage reads.
constexpr std::array<const char*, 23> kWithPreview{
    "_AUS2917.NEF",
    "_DSC5410.nef",
    "_DSF2161.RAF",
    "_SNY1264.ARW",
    "RAW_EPSON_RD1.ERF",
    "RAW_FUJI_X-A2.RAF",
    "RAW_FUJI_XPRO1.RAF",
    "RAW_HASSELBLAD_H3D39II.3FR",
    "RAW_HASSELBLAD_IXPRESS_CF132.3FR",
    "RAW_LEICA_M240.DNG",
    "RAW_LEICA_M8.DNG",
    "RAW_PANASONIC_DMC-GX-7_11.RW2",
    "RAW_PANASONIC_DMC-GX-7_169.RW2",
    "RAW_PANASONIC_DMC-GX-7_32.RW2",
    "RAW_PANASONIC_DMC-GX-7_43.RW2",
    "RAW_PANASONIC_LUMIX_DMC-LX7.RW2",
    "RAW_SAMSUNG_EX2F.SRW",
    "RAW_SAMSUNG_GX20.DNG",
    "RAW_SONY_A900.ARW",
    "RAW_SONY_ILCA-77M2.ARW",
    // Unread by exiv2: no preview (checked below).
    "RAW_SIGMA_DP2-QUATTRO.X3F",
    "RAW_LEICA_DLUX3.RAW",
    "RAW_PANASONIC_FZ8.RAW",
};
constexpr std::size_t kUnread = 3;  // the last ones above
}  // namespace

TEST_CASE("image: raw samples of other cameras" *
          doctest::skip(kRawSamples.empty() || !std::filesystem::is_directory(kRawSamples))) {
    for (std::size_t i = 0; i < kWithPreview.size() - kUnread; ++i) {
        INFO(std::string{kWithPreview.at(i)});
        REQUIRE(std::filesystem::exists(kRawSamples / kWithPreview.at(i)));
        for (const std::uint32_t side : {128U, 256U, 1024U, 2880U}) {
            INFO("side " << side);
            const image::EmbeddedPreview preview =
                image::embedded_preview(kRawSamples / kWithPreview.at(i), side);
            CHECK(preview.error.empty());
            REQUIRE_FALSE(preview.data.empty());
            const image::Decoded decoded = image::decode_preview(preview);
            REQUIRE(decoded.error.empty());
            CHECK(decoded.image.width == preview.width);
            CHECK(decoded.image.height == preview.height);
        }
    }
    const auto preview = [&](const char* name, std::uint32_t side) {
        return image::embedded_preview(kRawSamples / name, side);
    };
    // Only a TIFF preview in a 3FR and in the M8's DNG.
    const image::EmbeddedPreview hasselblad = preview("RAW_HASSELBLAD_H3D39II.3FR", 256);
    CHECK(hasselblad.tiff);
    CHECK(hasselblad.width == 320);
    CHECK(preview("RAW_LEICA_M8.DNG", 256).tiff);
    // DNG previews compressed as JPEG inside a TIFF: the JPEG is taken out (the GX20's is
    // full size).
    const image::EmbeddedPreview m240 = preview("RAW_LEICA_M240.DNG", 128);
    CHECK_FALSE(m240.tiff);
    CHECK(m240.width == 160);
    const image::EmbeddedPreview gx20 = preview("RAW_SAMSUNG_GX20.DNG", 1024);
    CHECK_FALSE(gx20.tiff);
    CHECK(gx20.width == 4672);
    CHECK(gx20.height == 3104);
    // Samsung EX2F's 640x360 JPEG stops before its end: the next one is taken.
    CHECK(preview("RAW_SAMSUNG_EX2F.SRW", 128).width == 4000);
    // A 4:3 EXIF thumbnail of a 3:2 photo has black bars: the next preview is taken instead.
    CHECK(preview("_AUS2917.NEF", 128).width == 640);
    CHECK(preview("RAW_SONY_A900.ARW", 128).width == 1616);
    CHECK(preview("RAW_PANASONIC_DMC-GX-7_11.RW2", 128).width == 1920);
    CHECK(preview("RAW_PANASONIC_DMC-GX-7_43.RW2", 128).width == 160);
    // Full-size previews of newer bodies are found when asked for a large one.
    CHECK(preview("_SNY1264.ARW", 2880).width == 7008);
    CHECK(preview("_AUS2917.NEF", 2880).width == 8256);
    // The GX7 stores its previews cropped to the aspect ratio shot: nothing to crop.
    CHECK_FALSE(preview("RAW_PANASONIC_DMC-GX-7_169.RW2", 1024).crop);
    CHECK(preview("RAW_PANASONIC_DMC-GX-7_169.RW2", 1024).height == 1080);
    // Formats exiv2 does not read: no preview, and Quick Look says so plainly.
    for (std::size_t i = kWithPreview.size() - kUnread; i < kWithPreview.size(); ++i) {
        INFO(std::string{kWithPreview.at(i)});
        REQUIRE(std::filesystem::exists(kRawSamples / kWithPreview.at(i)));
        CHECK(preview(kWithPreview.at(i), 256).data.empty());
        CHECK(image::load(
                  {.path = kRawSamples / kWithPreview.at(i), .max_width = 800, .max_height = 600})
                  .failure == picture::Failure::no_readable_preview);
    }
    // Hasselblad's CFV sample is a plain 8-bit PPM.
    const image::Decoded cfv = image::decode_file(kRawSamples / "RAW_HASSELBLAD_CFV.PPM");
    REQUIRE(cfv.error.empty());
    CHECK(cfv.format == Format::pnm);
    CHECK(cfv.image.width == 4096);
}

// The GX8 crops its 4:3 preview to the aspect ratio shot (rawsamples.ch's GX8 files, when the
// folder has them).
TEST_CASE("image: Panasonic GX8 crop" *
          doctest::skip(kRawSamples.empty() ||
                        !std::filesystem::exists(kRawSamples / "RAW_PANASONIC_GX8_169.RW2"))) {
    const auto preview = [&](const char* name, std::uint32_t side) {
        return image::embedded_preview(kRawSamples / name, side);
    };
    const image::EmbeddedPreview wide = preview("RAW_PANASONIC_GX8_169.RW2", 1024);
    REQUIRE(wide.crop);
    CHECK(wide.width == 1920);
    CHECK(wide.height == 1082);
    CHECK(wide.full_width == 5184);
    CHECK(wide.full_height == 2920);
    const image::EmbeddedPreview square = preview("RAW_PANASONIC_GX8_11.RW2", 1024);
    CHECK(square.width == 1440);
    CHECK(square.height == 1440);
    CHECK_FALSE(preview("RAW_PANASONIC_GX8_43.RW2", 1024).crop);
}
