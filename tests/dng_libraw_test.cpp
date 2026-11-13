// Every DNG in the RUMMAGE_RAW_SAMPLES folder (not in the repository) read by rummage's own
// reader (ROADMAP 2d-1) and by LibRaw: the same values, linearized, at every site. LibRaw is the
// reference only: it is linked into the tests when found, never into rummage or its helper.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <libraw/libraw.h>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "dng/develop.hpp"
#include "dng/raw.hpp"
#include "fd_file.hpp"
#include "tiff/container.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace dng = rummage::formats::image::dng;
namespace tiff = rummage::formats::image::tiff;

namespace {

const fs::path kSamples = [] {
    const char* folder = std::getenv("RUMMAGE_RAW_SAMPLES");
    return fs::path{folder != nullptr ? folder : ""};
}();

std::vector<fs::path> dngs() {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator{kSamples}) {
        std::string extension = entry.path().extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (extension == ".dng") {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

// The value of an optional that must be there (the test stops here when it is not).
template <typename T>
const T& got(const std::optional<T>& value) {
    REQUIRE(value.has_value());
    return *value;  // NOLINT(bugprone-unchecked-optional-access): REQUIRE above
}

// The mean colour of each cell of a 16 x 12 grid over a picture of width x height (channels a
// pixel): a comparison that small differences of crop do not upset.
std::vector<double> grid(const std::uint8_t* pixels, std::size_t width, std::size_t height,
                         std::size_t channels) {
    std::vector<double> sums(std::size_t{16} * 12 * 3);
    std::vector<std::size_t> counts(std::size_t{16} * 12);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t cell = ((y * 12 / height) * 16) + (x * 16 / width);
            ++counts[cell];
            for (std::size_t c = 0; c < 3; ++c) {
                sums[(cell * 3) + c] += pixels[(((y * width) + x) * channels) + c];  // NOLINT
            }
        }
    }
    for (std::size_t i = 0; i < sums.size(); ++i) {
        sums[i] /= static_cast<double>(std::max<std::size_t>(counts[i / 3], 1));
    }
    return sums;
}

}  // namespace

TEST_CASE("dng: sensor data of real DNGs as LibRaw reads it" *
          doctest::skip(kSamples.empty() || !fs::is_directory(kSamples))) {
    const std::vector<fs::path> files = dngs();
    REQUIRE_FALSE(files.empty());
    for (const fs::path& path : files) {
        INFO(path.filename().string());
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        REQUIRE(fd >= 0);
        const std::optional<image::detail::FdFile> file = image::detail::FdFile::of(fd);
        REQUIRE(file.has_value());
        const tiff::Source source{*file};
        const std::optional<tiff::Container> container = tiff::Container::open(source);
        REQUIRE(container.has_value());
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Raw> raw = dng::find_raw(*container, error, problem);
        REQUIRE_MESSAGE(raw.has_value(), error);
        const std::optional<std::vector<std::uint16_t>> ours = dng::read_all(*raw, error);
        ::close(fd);
        REQUIRE_MESSAGE(ours.has_value(), error);

        LibRaw reference;
        REQUIRE(reference.open_file(path.c_str()) == LIBRAW_SUCCESS);
        REQUIRE(reference.unpack() == LIBRAW_SUCCESS);
        const auto& sizes = reference.imgdata.sizes;
        REQUIRE(std::uint32_t{sizes.raw_width} == raw->width);
        REQUIRE(std::uint32_t{sizes.raw_height} == raw->height);
        const auto& data = reference.imgdata.rawdata;
        std::size_t differing = 0;
        const std::size_t pixels = std::size_t{raw->width} * raw->height;
        if (raw->cfa) {
            REQUIRE(data.raw_image != nullptr);
            for (std::size_t i = 0; i < pixels; ++i) {
                differing += (*ours)[i] != data.raw_image[i] ? 1U : 0U;
            }
        } else {
            REQUIRE(data.color4_image != nullptr);
            for (std::size_t i = 0; i < pixels; ++i) {
                for (std::size_t c = 0; c < raw->samples; ++c) {
                    differing +=
                        (*ours)[(i * raw->samples) + c] != data.color4_image[i][c] ? 1U : 0U;
                }
            }
        }
        CHECK(differing == 0);
    }
}

// Developed plainly (no look) and set beside LibRaw's development of the same file: half size,
// the camera's white balance, the file's matrix, sRGB, no automatic brightness, highlights
// clipped, no opcodes (LibRaw applies none of list 3, the DJI's gain map and warp). Measured
// 2026-11-07 on the six samples: mean difference 0.1 to 1.7 levels of 255, at worst 14 in one
// cell (crops differ by a few pixels).
TEST_CASE("dng: developed as LibRaw develops it" *
          doctest::skip(kSamples.empty() || !fs::is_directory(kSamples))) {
    const std::vector<fs::path> files = dngs();
    REQUIRE_FALSE(files.empty());
    for (const fs::path& path : files) {
        INFO(path.filename().string());
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        REQUIRE(fd >= 0);
        const std::optional<image::detail::FdFile> file = image::detail::FdFile::of(fd);
        REQUIRE(file.has_value());
        const tiff::Source source{*file};
        const std::optional<tiff::Container> container = tiff::Container::open(source);
        REQUIRE(container.has_value());
        std::string error;
        image::Problem problem{};
        const std::optional<dng::Developed> ours =
            dng::develop(got(container), {.look = false, .opcodes = false}, error, problem);
        ::close(fd);
        REQUIRE_MESSAGE(ours.has_value(), error);

        LibRaw reference;
        auto& params = reference.imgdata.params;
        params.half_size = 1;
        params.use_camera_wb = 1;
        params.output_color = 1;  // sRGB
        params.gamm[0] = 1.0 / 2.4;
        params.gamm[1] = 12.92;
        params.no_auto_bright = 1;
        params.highlight = 0;  // clip
        params.user_flip = 0;  // as stored
        params.output_bps = 8;
        REQUIRE(reference.open_file(path.c_str()) == LIBRAW_SUCCESS);
        REQUIRE(reference.unpack() == LIBRAW_SUCCESS);
        REQUIRE(reference.dcraw_process() == LIBRAW_SUCCESS);
        int status = 0;
        libraw_processed_image_t* theirs = reference.dcraw_make_mem_image(&status);
        REQUIRE(theirs != nullptr);
        const std::vector<double> a =
            grid(ours->image.rgba.data(), ours->image.width, ours->image.height, 4);
        const std::vector<double> b = grid(theirs->data, theirs->width, theirs->height, 3);
        LibRaw::dcraw_clear_mem(theirs);
        double total = 0.0;
        double worst = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            total += std::abs(a[i] - b[i]);
            worst = std::max(worst, std::abs(a[i] - b[i]));
        }
        CHECK(total / static_cast<double>(a.size()) < 3.0);
        CHECK(worst < 20.0);
    }
}
