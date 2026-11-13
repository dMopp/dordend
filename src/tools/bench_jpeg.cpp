// JPEG decoding comparison for the thumbnails (ROADMAP 5.2). Not installed.
//
//   rummage-bench-jpeg [--runs N] FILE...
//
// Decodes each JPEG from memory with wuffs (full size, as rummage::image does) and with
// TurboJPEG at scales 1, 1/2, 1/4, 1/8 (DCT scaling: the smaller sizes are decoded, not
// resized), to RGBA, without the EXIF turn. Prints the median over N runs (default 5, the first
// not counted) per file, then the median over all files.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string_view>
#include <turbojpeg.h>
#include <vector>

#include "rummage/formats/image/decode.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<int, 4> kDenominators{1, 2, 4, 8};

double median(std::vector<double> values) {
    std::ranges::sort(values);
    return values[values.size() / 2];
}

// The median time of run() over runs runs, the first not counted.
double time_runs(int runs, const std::function<bool()>& run) {
    std::vector<double> times;
    for (int i = 0; i < runs; ++i) {
        const auto start = Clock::now();
        if (!run()) {
            return -1.0;
        }
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    if (times.size() > 1) {
        times.erase(times.begin());
    }
    return median(times);
}

}  // namespace

int main(int argc, char** argv) {
    std::span args{argv, static_cast<std::size_t>(argc)};
    args = args.subspan(1);
    int runs = 5;
    if (args.size() >= 2 && std::string_view{args[0]} == "--runs") {
        runs = std::max(1, std::atoi(args[1]));
        args = args.subspan(2);
    }
    if (args.empty()) {
        std::fprintf(stderr, "usage: rummage-bench-jpeg [--runs N] FILE...\n");
        return 2;
    }
    tjhandle turbo = tj3Init(TJINIT_DECOMPRESS);
    if (turbo == nullptr) {
        std::fprintf(stderr, "cannot start TurboJPEG\n");
        return 1;
    }
    std::vector<double> wuffs_all;
    std::array<std::vector<double>, kDenominators.size()> turbo_all;
    std::vector<std::uint8_t> pixels;
    for (const char* arg : args) {
        const std::filesystem::path path{arg};
        std::vector<std::uint8_t> data(std::filesystem::file_size(path));
        std::ifstream file{path, std::ios::binary};
        file.read(reinterpret_cast<char*>(data.data()),  // NOLINT: bytes to char for read
                  static_cast<std::streamsize>(data.size()));

        const double wuffs = time_runs(runs, [&] {
            return rummage::formats::image::decode(data, {.apply_orientation = false})
                .error.empty();
        });
        std::printf("%s\n  wuffs 1/1: %.1f ms\n", arg, wuffs);
        wuffs_all.push_back(wuffs);

        if (tj3DecompressHeader(turbo, data.data(), data.size()) != 0) {
            std::printf("  TurboJPEG: %s\n", tj3GetErrorStr(turbo));
            continue;
        }
        const int width = tj3Get(turbo, TJPARAM_JPEGWIDTH);
        const int height = tj3Get(turbo, TJPARAM_JPEGHEIGHT);
        for (std::size_t i = 0; i < kDenominators.size(); ++i) {
            const tjscalingfactor factor{.num = 1, .denom = kDenominators.at(i)};
            tj3SetScalingFactor(turbo, factor);
            const int scaled_width = TJSCALED(width, factor);
            const int scaled_height = TJSCALED(height, factor);
            pixels.resize(static_cast<std::size_t>(scaled_width) *
                          static_cast<std::size_t>(scaled_height) * 4);
            const double time = time_runs(runs, [&] {
                return tj3Decompress8(turbo, data.data(), data.size(), pixels.data(), 0,
                                      TJPF_RGBA) == 0;
            });
            std::printf("  TurboJPEG 1/%d (%dx%d): %.1f ms\n", factor.denom, scaled_width,
                        scaled_height, time);
            turbo_all.at(i).push_back(time);
        }
    }
    tj3Destroy(turbo);
    std::printf("median of %zu files: wuffs 1/1 %.1f ms", wuffs_all.size(), median(wuffs_all));
    for (std::size_t i = 0; i < kDenominators.size(); ++i) {
        if (!turbo_all.at(i).empty()) {
            std::printf(", TurboJPEG 1/%d %.1f ms", kDenominators.at(i), median(turbo_all.at(i)));
        }
    }
    std::printf("\n");
    return 0;
}
