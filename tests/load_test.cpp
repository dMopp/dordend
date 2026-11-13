#include "rummage/formats/image/load.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "rummage/picture/load.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace picture = rummage::picture;
namespace thumbs = rummage::thumbs;
using rummage::test::TempDir;
using picture::Failure;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out{path, std::ios::binary};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

picture::Loaded load(const fs::path& path, std::uint32_t side) {
    return image::load({.path = path, .max_width = side, .max_height = side});
}

}  // namespace

TEST_CASE("load: every format through the one path, the same for thumbnails") {
    const TempDir dir;
    write_bytes(dir.path / "blocks.ppm", std::string{"P6\n2 1\n255\n\xff\x00\x00\x00\x80\xff", 17});
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    for (const fs::path& file :
         {kData / "blocks.png", kData / "blocks.jpg", kData / "blocks.gif", kData / "blocks.webp",
          kData / "blocks.jxl", kData / "blocks-orient6.jpg", kData / "shot.jpg",
          kData / "with-thumbnail.jpg", kData / "tiff-preview.tif", dir.path / "blocks.ppm"}) {
        INFO(file.filename().string());
        const picture::Loaded loaded = load(file, 128);
        REQUIRE(loaded.failure == Failure::none);
        CHECK(loaded.error().empty());
        CHECK_FALSE(loaded.image.empty());
        CHECK(loaded.image.width <= 128);
        CHECK(loaded.image.height <= 128);
        const thumbs::Thumbnail thumbnail = cache.make(file, thumbs::Size::normal);
        REQUIRE(thumbnail.source != thumbs::Source::failed);
        CHECK(thumbnail.image.rgba == loaded.image.rgba);
        CHECK((thumbnail.source == thumbs::Source::preview) == loaded.from_preview);
    }
    // A plain TIFF (here one reduced-resolution image only) is decoded by rummage's own TIFF
    // reader, not taken as a camera's preview.
    CHECK_FALSE(load(kData / "tiff-preview.tif", 1024).from_preview);
    // Turned upright, and the full size says so.
    const picture::Loaded turned = load(kData / "blocks-orient6.jpg", 1024);
    CHECK(turned.full_width == 32);
    CHECK(turned.full_height == 48);
    CHECK(turned.image.width == 32);
}

TEST_CASE("load: the bytes decide, not the name") {
    const TempDir dir;
    fs::copy_file(kData / "blocks.png", dir.path / "really a png.jpg");
    fs::copy_file(kData / "blocks.jpg", dir.path / "really a jpeg.png");
    fs::copy_file(kData / "blocks.webp", dir.path / "really a webp.orf");
    for (const char* name : {"really a png.jpg", "really a jpeg.png", "really a webp.orf"}) {
        INFO(name);
        const picture::Loaded loaded = load(dir.path / name, 64);
        CHECK(loaded.failure == Failure::none);
        CHECK(loaded.image.width == 48);
    }
}

TEST_CASE("load: each failure has its own plain reason") {
    const TempDir dir;
    write_bytes(dir.path / "empty.orf", "");
    write_bytes(dir.path / "noise.orf", "not a photo at all, just some words in a file");
    write_bytes(dir.path / "huge.ppm", "P6\n20000 20000\n255\n");
    write_bytes(dir.path / "fake.x3f", std::string{"FOVb\0\0\x04\0", 8});
    fs::copy_file(kData / "truncated.jpg", dir.path / "truncated.jpg");
    CHECK(load(dir.path / "empty.orf", 128).failure == Failure::damaged);
    CHECK(load(dir.path / "noise.orf", 128).failure == Failure::unknown_format);
    CHECK(load(dir.path / "huge.ppm", 128).failure == Failure::too_large);
    CHECK(load(dir.path / "truncated.jpg", 128).failure == Failure::damaged);
    CHECK(load(dir.path / "missing.jpg", 128).failure == Failure::unreadable);
    const picture::Loaded x3f = load(dir.path / "fake.x3f", 128);
    CHECK(x3f.failure == Failure::no_readable_preview);
    CHECK(x3f.error() == "No preview rummage can show in this file");
    CHECK_FALSE(x3f.detail.empty());  // the technical reason, for the log
    // One sentence each, all different; none has none.
    std::set<std::string_view> sentences;
    for (const Failure failure : {Failure::unknown_format, Failure::no_readable_preview,
                                  Failure::too_large, Failure::damaged, Failure::unreadable}) {
        CHECK_FALSE(picture::failure_message(failure).empty());
        sentences.insert(picture::failure_message(failure));
    }
    CHECK(sentences.size() == 5);
    CHECK(picture::failure_message(Failure::none).empty());
}

TEST_CASE("load: exiv2 only for files that can carry a stored preview") {
    const TempDir dir;
    write_bytes(dir.path / "a.ppm", std::string{"P5 1 1 255 "} + "\x80");
    const std::uint64_t before = image::embedded_preview_reads();
    for (const fs::path& file : {kData / "blocks.png", kData / "blocks.gif", kData / "blocks.webp",
                                 kData / "blocks.jxl", kData / "blocks.jpg", dir.path / "a.ppm"}) {
        INFO(file.filename().string());
        CHECK(load(file, 128).failure == Failure::none);
    }
    CHECK(image::embedded_preview_reads() == before);               // a JPEG without EXIF included
    CHECK(load(kData / "shot.jpg", 128).failure == Failure::none);  // a camera's JPEG
    CHECK(load(kData / "tiff-preview.tif", 128).failure == Failure::none);  // a plain TIFF: ours
    CHECK(image::embedded_preview_reads() == before + 1);
}
