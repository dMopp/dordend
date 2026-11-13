// Image decoding check and benchmark (ROADMAP 5.1). Not installed.
//
//   rummage-decode [--runs N] [--stored] [--ppm OUT.ppm] [--raw OUT.pnm] FILE...
//
// For each file prints the format, the size (after the EXIF orientation), the orientation, the
// time to read the file and the median time to decode it over N runs (default 5; the first run
// is not counted when N > 1). --stored leaves the pixels as stored (no EXIF turn). --ppm writes the
// last image decoded, over a mid gray where it is transparent, to look at it (magick OUT.ppm
// OUT.png). A camera raw shows what rummage shows of it (image::load, unscaled: its largest
// preview), not the first image of its TIFF structure. --raw also reads a DNG's sensor data
// (ROADMAP 2d), prints its time and the development's (half size and in a 2560x1440 box, with
// and without the file's opcodes), and writes the last one read as a 16-bit
// PGM (CFA) or PPM (linear raw) of the stored values.
#include "rummage/formats/image/decode.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "dng/develop.hpp"
#include "dng/raw.hpp"
#include "fd_file.hpp"
#include "rummage/formats/image/load.hpp"
#include "tiff/container.hpp"
#include "tiff/decode.hpp"

namespace {

namespace image = rummage::formats::image;
namespace picture = rummage::picture;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

bool write_ppm(const std::filesystem::path& path, const picture::Image& picture) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << picture.width << ' ' << picture.height << "\n255\n";
    std::vector<char> row(std::size_t{picture.width} * 3);
    for (std::size_t y = 0; y < picture.height; ++y) {
        for (std::size_t x = 0; x < picture.width; ++x) {
            const std::size_t at = ((y * picture.width) + x) * 4;
            const unsigned alpha = picture.rgba[at + 3];
            for (std::size_t c = 0; c < 3; ++c) {
                const unsigned value =
                    ((picture.rgba[at + c] * alpha) + (128U * (255U - alpha))) / 255U;
                row[(x * 3) + c] = static_cast<char>(value);
            }
        }
        out.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(out);
}

bool write_raw(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::uint32_t samples, const std::vector<std::uint16_t>& values) {
    std::ofstream out{path, std::ios::binary};
    out << (samples == 1 ? "P5\n" : "P6\n") << width << ' ' << height << "\n65535\n";
    std::vector<char> bytes;
    bytes.reserve(values.size() * 2);
    const std::uint32_t channels = samples == 1 ? 1 : 3;
    for (std::size_t i = 0; i < std::size_t{width} * height; ++i) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const std::uint16_t v = values[(i * samples) + std::min(c, samples - 1)];
            bytes.push_back(static_cast<char>(v >> 8));
            bytes.push_back(static_cast<char>(v & 0xFFU));
        }
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// A DNG's sensor data: read runs times, the median printed; the values of the last read kept in
// values. False when it is not a DNG this reader reads (the reason printed).
bool read_sensor(const char* arg, int fd, int runs, std::vector<std::uint16_t>& values,
                 std::uint32_t& width, std::uint32_t& height, std::uint32_t& samples) {
    const std::optional<image::detail::FdFile> file = image::detail::FdFile::of(fd);
    if (!file) {
        return false;
    }
    const image::tiff::Source source{*file};
    const std::optional<image::tiff::Container> container = image::tiff::Container::open(source);
    std::string error = "not a TIFF";
    image::Problem problem{};
    const std::optional<image::dng::Raw> raw =
        container ? image::dng::find_raw(*container, error, problem) : std::nullopt;
    if (!raw) {
        std::printf("%s: sensor data: %s\n", arg, error.c_str());
        return false;
    }
    std::vector<double> times;
    for (int run = 0; run < runs; ++run) {
        const auto start = Clock::now();
        std::optional<std::vector<std::uint16_t>> all = image::dng::read_all(*raw, error);
        times.push_back(ms_since(start));
        if (!all) {
            std::printf("%s: sensor data: %s\n", arg, error.c_str());
            return false;
        }
        values = std::move(*all);
    }
    if (times.size() > 1) {
        times.erase(times.begin());
    }
    std::ranges::sort(times);
    const double median = times[times.size() / 2];
    const double megapixels = static_cast<double>(raw->width) * raw->height / 1e6;
    std::printf(
        "%s: sensor %ux%u (%.1f MP), %s, %u samples of %u bits, compression %llu, read "
        "in %.1f ms (%.0f MP/s)\n",
        arg, raw->width, raw->height, megapixels, raw->cfa ? "CFA" : "linear raw", raw->samples,
        raw->bits, static_cast<unsigned long long>(raw->compression), median,
        megapixels / (median / 1000.0));

    // The development alone (sensor read included): at half size and into a 2560x1440 box (Quick
    // Look on a 1440p screen), with the file's opcodes and without them.
    struct Run {
        const char* what{nullptr};
        image::dng::DevelopOptions options;
    };

    const std::array<Run, 4> developments{{
        {"half size", {}},
        {"half size, no opcodes", {.opcodes = false}},
        {"2560x1440 box", {.max_width = 2560, .max_height = 1440}},
        {"2560x1440 box, no opcodes", {.opcodes = false, .max_width = 2560, .max_height = 1440}},
    }};
    for (const Run& development : developments) {
        times.clear();
        std::optional<image::dng::Developed> developed;
        for (int run = 0; run < runs; ++run) {
            const auto start = Clock::now();
            developed = image::dng::develop(*container, development.options, error, problem);
            times.push_back(ms_since(start));
        }
        if (times.size() > 1) {
            times.erase(times.begin());
        }
        std::ranges::sort(times);
        if (developed) {
            std::printf("%s: developed (%s) %ux%u in %.1f ms (sensor read included)\n", arg,
                        development.what, developed->image.width, developed->image.height,
                        times[times.size() / 2]);
        } else {
            std::printf("%s: not developed: %s\n", arg, error.c_str());
            break;
        }
    }
    width = raw->width;
    height = raw->height;
    samples = raw->samples;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::span args{argv, static_cast<std::size_t>(argc)};
    args = args.subspan(1);
    int runs = 5;
    std::filesystem::path ppm;
    std::filesystem::path raw_out;
    bool sensor = false;
    image::DecodeOptions options;
    while (!args.empty()) {
        const std::string_view option{args[0]};
        if (option == "--stored") {
            options.apply_orientation = false;
            args = args.subspan(1);
            continue;
        }
        if (args.size() < 2) {
            break;
        }
        if (option == "--runs") {
            runs = std::max(1, std::atoi(args[1]));
        } else if (option == "--ppm") {
            ppm = args[1];
        } else if (option == "--raw") {
            raw_out = args[1];
            sensor = true;
        } else {
            break;
        }
        args = args.subspan(2);
    }
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: rummage-decode [--runs N] [--stored] [--ppm OUT.ppm] [--raw OUT.pnm] "
                     "FILE...\n");
        return 2;
    }
    int failures = 0;
    picture::Image last;
    std::vector<std::uint16_t> last_raw;
    std::uint32_t raw_width = 0;
    std::uint32_t raw_height = 0;
    std::uint32_t raw_samples = 1;
    for (const char* arg : args) {
        const std::filesystem::path path{arg};
        const auto read_start = Clock::now();
        std::vector<std::uint8_t> data(std::filesystem::file_size(path));
        std::ifstream file{path, std::ios::binary};
        file.read(reinterpret_cast<char*>(data.data()),  // NOLINT: bytes to char for read
                  static_cast<std::streamsize>(data.size()));
        const double read_ms = ms_since(read_start);

        // A camera raw: what load() shows of it, its largest preview.
        bool camera_raw = false;
        const int fd = ::open(arg, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (const auto opened = image::detail::FdFile::of(fd)) {
                camera_raw = image::sniff(data) == image::Format::tiff &&
                             image::tiff::is_camera_raw(image::tiff::Source{*opened});
            }
            if (sensor) {
                (void)read_sensor(arg, fd, runs, last_raw, raw_width, raw_height, raw_samples);
            }
        }
        std::vector<double> times;
        image::Decoded decoded;
        for (int run = 0; run < runs; ++run) {
            const auto start = Clock::now();
            if (camera_raw) {
                image::Loaded loaded = image::load(fd, 65535, 65535);
                decoded = {};
                decoded.format = image::Format::tiff;
                decoded.image = std::move(loaded.image);
                decoded.error = loaded.failure == image::Failure::none ? "" : loaded.detail;
                if (loaded.failure != image::Failure::none && decoded.error.empty()) {
                    decoded.error = "no picture";
                }
            } else {
                decoded = image::decode(data, options);
            }
            times.push_back(ms_since(start));
        }
        if (fd >= 0) {
            ::close(fd);
        }
        if (!decoded.error.empty()) {
            std::printf("%s: %s (%s)\n", arg, decoded.error.c_str(),
                        std::string{image::format_name(decoded.format)}.c_str());
            ++failures;
            continue;
        }
        if (times.size() > 1) {
            times.erase(times.begin());  // the first run warms the caches up
        }
        std::ranges::sort(times);
        const double median = times[times.size() / 2];
        const double megapixels =
            static_cast<double>(decoded.image.width) * decoded.image.height / 1e6;
        std::printf(
            "%s: %s %ux%u (%.1f MP), orientation %u, %.1f MB read in %.1f ms, decoded in "
            "%.1f ms (%.0f MP/s)\n",
            arg, std::string{image::format_name(decoded.format)}.c_str(), decoded.image.width,
            decoded.image.height, megapixels, unsigned{decoded.orientation},
            static_cast<double>(data.size()) / 1e6, read_ms, median,
            megapixels / (median / 1000.0));
        last = std::move(decoded.image);
    }
    if (!raw_out.empty() && !last_raw.empty() &&
        !write_raw(raw_out, raw_width, raw_height, raw_samples, last_raw)) {
        std::fprintf(stderr, "cannot write %s\n", raw_out.c_str());
        return 1;
    }
    if (!ppm.empty() && !last.empty() && !write_ppm(ppm, last)) {
        std::fprintf(stderr, "cannot write %s\n", ppm.c_str());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
