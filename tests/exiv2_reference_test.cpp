// rummage's own metadata readers (ROADMAP 2e) against the exiv2 code they replace
// (exiv2_reference.cpp): the same embedded previews (a JPEG's bytes, a TIFF one's pixels, size,
// crop, orientation, full size; why none was found is only a log line, not compared)
// and the same photo information, file by file. On the fixtures here, every file of the
// RUMMAGE_RAW_SAMPLES folder and of the folders in RUMMAGE_EXTRA_SAMPLES (colon-separated: the
// author's own photos, never in the repository), when they are set. Built only when exiv2 is
// installed (tests only: never linked into rummage or its helper).
#include "exiv2_reference.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/formats/image/photo.hpp"
#include "rummage/formats/image/preview.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;

namespace {

const fs::path kFixtures = fs::path{RUMMAGE_TEST_DATA} / "image";

// The files to compare: the fixtures, then every regular file of the sample folders.
std::vector<fs::path> files() {
    std::vector<fs::path> out;
    std::vector<fs::path> folders{kFixtures};
    if (const char* raw = std::getenv("RUMMAGE_RAW_SAMPLES"); raw != nullptr && *raw != '\0') {
        folders.emplace_back(raw);
    }
    if (const char* extra = std::getenv("RUMMAGE_EXTRA_SAMPLES"); extra != nullptr) {
        std::string_view rest{extra};
        while (!rest.empty()) {
            const std::size_t colon = rest.find(':');
            if (colon != 0) {
                folders.emplace_back(std::string{rest.substr(0, colon)});
            }
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        }
    }
    for (const fs::path& folder : folders) {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator{folder, error}) {
            const std::string extension = entry.path().extension().string();
            if (entry.is_regular_file() && extension != ".xmp" && extension != ".sh" &&
                extension != ".7z") {
                out.push_back(entry.path());
            }
        }
    }
    return out;
}

struct Fd {
    explicit Fd(const fs::path& path) : fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC)} {}

    ~Fd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    int fd;
};

void same_preview(const image::EmbeddedPreview& ours, const image::EmbeddedPreview& theirs) {
    // A JPEG is the file's bytes, the same; a TIFF preview is copied out of the file as a TIFF
    // of its own, by each in its way: the same pixels.
    if (ours.tiff && theirs.tiff) {
        const image::Decoded a = image::decode_preview(ours);
        const image::Decoded b = image::decode_preview(theirs);
        CHECK(a.error == b.error);
        CHECK(a.image.width == b.image.width);
        CHECK(a.image.height == b.image.height);
        CHECK(a.image.rgba == b.image.rgba);
    } else {
        CHECK(ours.data == theirs.data);
    }
    CHECK(ours.tiff == theirs.tiff);
    CHECK(ours.width == theirs.width);
    CHECK(ours.height == theirs.height);
    CHECK(ours.crop.has_value() == theirs.crop.has_value());
    if (ours.crop && theirs.crop) {
        CHECK(ours.crop->x == theirs.crop->x);
        CHECK(ours.crop->y == theirs.crop->y);
        CHECK(ours.crop->width == theirs.crop->width);
        CHECK(ours.crop->height == theirs.crop->height);
    }
    CHECK(ours.orientation == theirs.orientation);
    CHECK(ours.full_width == theirs.full_width);
    CHECK(ours.full_height == theirs.full_height);
}

void same_info(const image::PhotoInfo& ours, const image::PhotoInfo& theirs) {
    CHECK(ours.width == theirs.width);
    CHECK(ours.height == theirs.height);
    CHECK(ours.camera == theirs.camera);
    CHECK(ours.lens == theirs.lens);
    CHECK(ours.exposure_time == theirs.exposure_time);
    CHECK(ours.aperture == theirs.aperture);
    CHECK(ours.iso == theirs.iso);
    CHECK(ours.focal_length == theirs.focal_length);
    CHECK(ours.taken == theirs.taken);
    CHECK(ours.program == theirs.program);
    CHECK(ours.exposure_bias == theirs.exposure_bias);
    CHECK(ours.metering == theirs.metering);
    CHECK(ours.flash == theirs.flash);
    CHECK(ours.white_balance == theirs.white_balance);
    CHECK(ours.software == theirs.software);
}

}  // namespace

TEST_CASE("exiv2 reference: embedded previews and photo information as exiv2 gave them") {
    const std::vector<fs::path> all = files();
    REQUIRE_FALSE(all.empty());
    for (const fs::path& path : all) {
        INFO(path.string());
        const Fd file{path};
        REQUIRE(file.fd >= 0);
        // A thumbnail, Quick Look on a small and on a large screen.
        for (const std::uint32_t side : {160U, 1600U, 4000U}) {
            INFO("long side ", side);
            same_preview(image::embedded_preview(file.fd, side),
                         image::reference::embedded_preview(file.fd, side));
        }
        // The sidecar's rating is no longer read (2e-4): exiv2 is asked without it.
        same_info(image::read_photo_info(file.fd), image::reference::read_photo_info(file.fd, -1));
    }
}

namespace {

// A little-endian TIFF of a 1x1 grey image whose IFD0 has Make, Model, Software and Orientation
// and whose EXIF IFD has the fields given, each (tag, type, count, value bytes).
struct Field {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    std::vector<std::uint8_t> bytes;
};

Field ascii(std::uint16_t tag, std::string_view text) {
    Field f{.tag = tag, .type = 2, .count = 0, .bytes = {text.begin(), text.end()}};
    f.bytes.push_back(0);
    f.count = static_cast<std::uint32_t>(f.bytes.size());
    return f;
}

Field rational(std::uint16_t tag, std::int64_t n, std::int64_t d, bool is_signed = false) {
    Field f{.tag = tag,
            .type = static_cast<std::uint16_t>(is_signed ? 10 : 5),
            .count = 1,
            .bytes = {}};
    for (const std::int64_t v : {n, d}) {
        const auto u = static_cast<std::uint32_t>(v);
        for (int shift = 0; shift < 32; shift += 8) {
            f.bytes.push_back(static_cast<std::uint8_t>(u >> shift));
        }
    }
    return f;
}

Field short_value(std::uint16_t tag, std::uint16_t v) {
    return {.tag = tag,
            .type = 3,
            .count = 1,
            .bytes = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)}};
}

std::vector<std::uint8_t> tiff_with(std::vector<Field> image, std::vector<Field> exif) {
    std::vector<std::uint8_t> out{'I', 'I', 42, 0, 0, 0, 0, 0};
    const auto put = [&](std::size_t at, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.at(at + static_cast<std::size_t>(i)) = static_cast<std::uint8_t>(v >> (8 * i));
        }
    };
    // An IFD at the end of out; values that do not fit after it. Returns its offset.
    const auto ifd = [&](std::vector<Field> fields) {
        std::ranges::sort(fields, {}, &Field::tag);
        const std::size_t at = out.size();
        out.resize(at + 2 + (fields.size() * 12) + 4);
        put(at, static_cast<std::uint32_t>(fields.size()), 2);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const std::size_t entry = at + 2 + (i * 12);
            put(entry, fields[i].tag, 2);
            put(entry + 2, fields[i].type, 2);
            put(entry + 4, fields[i].count, 4);
            if (fields[i].bytes.size() <= 4) {
                std::ranges::copy(fields[i].bytes,
                                  out.begin() + static_cast<std::ptrdiff_t>(entry + 8));
            } else {
                put(entry + 8, static_cast<std::uint32_t>(out.size()), 4);
                out.insert(out.end(), fields[i].bytes.begin(), fields[i].bytes.end());
                if (out.size() % 2 != 0) {
                    out.push_back(0);
                }
            }
        }
        return static_cast<std::uint32_t>(at);
    };
    const std::uint32_t exif_at = ifd(std::move(exif));
    const auto pixel = static_cast<std::uint32_t>(out.size());
    out.push_back(128);
    out.push_back(0);
    image.push_back({.tag = 256, .type = 3, .count = 1, .bytes = {1, 0}});
    image.push_back({.tag = 257, .type = 3, .count = 1, .bytes = {1, 0}});
    image.push_back(short_value(258, 8));
    image.push_back(short_value(259, 1));
    image.push_back(short_value(262, 1));
    image.push_back(
        {.tag = 273,
         .type = 4,
         .count = 1,
         .bytes = {static_cast<std::uint8_t>(pixel), static_cast<std::uint8_t>(pixel >> 8), 0, 0}});
    image.push_back(short_value(277, 1));
    image.push_back({.tag = 279, .type = 4, .count = 1, .bytes = {1, 0, 0, 0}});
    image.push_back({.tag = 0x8769,
                     .type = 4,
                     .count = 1,
                     .bytes = {static_cast<std::uint8_t>(exif_at),
                               static_cast<std::uint8_t>(exif_at >> 8), 0, 0}});
    put(4, ifd(std::move(image)), 4);
    return out;
}

}  // namespace

TEST_CASE("exiv2 reference: every way a value is written, on TIFFs made here") {
    const rummage::test::TempDir dir;

    struct Case {
        const char* name;
        std::vector<Field> image;
        std::vector<Field> exif;
    };

    const std::vector<Case> cases{
        {"plain",
         {ascii(0x10F, "OLYMPUS IMAGING CORP.  "), ascii(0x110, "E-M1MarkII"),
          ascii(0x131, "Version 1.0  "), short_value(274, 6)},
         {rational(0x829A, 1, 250), rational(0x829D, 28, 10), short_value(0x8827, 400),
          rational(0x920A, 120, 10), ascii(0x9003, "2024:05:01 07:08:09"), short_value(0x8822, 3),
          rational(0x9204, -2, 6, true), short_value(0x9207, 5), short_value(0x9209, 0x10),
          short_value(0xA403, 1), ascii(0xA434, "M.12-40mm F2.8  ")}},
        {"long exposure",
         {ascii(0x10F, "NIKON"), ascii(0x110, "D800")},
         {rational(0x829A, 25, 10), rational(0x829D, 16, 1), rational(0x920A, 0, 1),
          rational(0x9204, 133, 100, true), short_value(0x8822, 99), short_value(0x9207, 255),
          short_value(0x9209, 0x5f), short_value(0xA403, 7)}},
        {"odd fractions",
         {ascii(0x110, "Model only"), short_value(274, 8)},
         {rational(0x829A, 10, 3), rational(0x829D, 0, 0), rational(0x920A, 7, 0),
          rational(0x9204, 0, 1, true), short_value(0x9209, 0x99)}},
        {"edges",
         {ascii(0x10F, "Canon"), ascii(0x110, "Canon EOS R5")},
         {rational(0x829A, 1, 1), rational(0x829A + 0, 0, 1), rational(0x829D, 14, 10),
          rational(0x9204, 3, -2, true), rational(0x920A, 2400, 10)}},
        {"seconds", {}, {rational(0x829A, 30, 1), rational(0x9204, 1, 3, true)}},
        {"signed time", {}, {rational(0x829A, 1, 60, true)}},
        {"empty", {}, {}},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        const fs::path path = dir.path / (std::string{c.name} + ".tif");
        {
            const std::vector<std::uint8_t> bytes = tiff_with(c.image, c.exif);
            std::ofstream out{path, std::ios::binary};
            out.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT: bytes to chars
                      static_cast<std::streamsize>(bytes.size()));
        }
        const Fd file{path};
        REQUIRE(file.fd >= 0);
        same_info(image::read_photo_info(file.fd), image::reference::read_photo_info(file.fd, -1));
    }
}
