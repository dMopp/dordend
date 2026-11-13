// Thumbnail check (ROADMAP 5.2). Not installed.
//
//   rummage-thumb [--size normal|large|x-large|xx-large] [--cache DIR] [--make] [--ppm OUT]
//                 FILE...
//
// Gets the thumbnail of each file through the freedesktop cache (default: ~/.cache/thumbnails,
// as rummage does): prints where it came from (cache, preview, decoded, failed), its size, the
// time and the thumbnail's path. --make ignores what is cached. --ppm writes the last one, over
// a mid gray, to look at it.
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/formats/image/load.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/thumbs/cache.hpp"

namespace {

namespace thumbs = rummage::thumbs;
using Clock = std::chrono::steady_clock;

bool write_ppm(const std::filesystem::path& path, const rummage::picture::Image& picture) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << picture.width << ' ' << picture.height << "\n255\n";
    for (std::size_t i = 0; i < picture.rgba.size(); i += 4) {
        const unsigned alpha = picture.rgba[i + 3];
        for (std::size_t c = 0; c < 3; ++c) {
            out.put(static_cast<char>(((picture.rgba[i + c] * alpha) + (128U * (255U - alpha))) /
                                      255U));
        }
    }
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    std::span args{argv, static_cast<std::size_t>(argc)};
    args = args.subspan(1);
    thumbs::Size size = thumbs::Size::large;
    std::filesystem::path root = thumbs::default_root();
    std::filesystem::path ppm;
    bool make = false;
    while (!args.empty()) {
        const std::string_view option{args[0]};
        if (option == "--make") {
            make = true;
            args = args.subspan(1);
            continue;
        }
        if (args.size() < 2) {
            break;
        }
        const std::string_view value{args[1]};
        if (option == "--size") {
            bool known = false;
            for (const thumbs::Size candidate : {thumbs::Size::normal, thumbs::Size::large,
                                                 thumbs::Size::x_large, thumbs::Size::xx_large}) {
                if (value == thumbs::directory_name(candidate)) {
                    size = candidate;
                    known = true;
                }
            }
            if (!known) {
                std::fprintf(stderr, "unknown size: %s\n", args[1]);
                return 2;
            }
        } else if (option == "--cache") {
            root = value;
        } else if (option == "--ppm") {
            ppm = value;
        } else {
            break;
        }
        args = args.subspan(2);
    }
    if (args.empty()) {
        std::fprintf(stderr,
                     "usage: rummage-thumb [--size normal|large|x-large|xx-large] [--cache DIR] "
                     "[--make] [--ppm OUT] FILE...\n");
        return 2;
    }
    // A development tool: pictures made in this process (rummage uses its isolated helpers).
    rummage::picture::set_loader([](const rummage::picture::LoadRequest& request) {
        return rummage::formats::image::load(request);
    });
    const thumbs::Cache cache{root, "rummage-dev"};
    int failures = 0;
    rummage::picture::Image last;
    for (const char* arg : args) {
        const std::filesystem::path file = std::filesystem::absolute(arg).lexically_normal();
        const auto start = Clock::now();
        thumbs::Thumbnail thumbnail = make ? cache.make(file, size) : cache.get(file, size);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (thumbnail.source == thumbs::Source::failed) {
            std::printf("%s: failed (%s) in %.1f ms\n", arg, thumbnail.error.c_str(), ms);
            ++failures;
            continue;
        }
        std::printf("%s: %s %ux%u in %.1f ms -> %s\n", arg,
                    std::string{thumbs::source_name(thumbnail.source)}.c_str(),
                    thumbnail.image.width, thumbnail.image.height, ms,
                    cache.path_for(file, size).c_str());
        last = std::move(thumbnail.image);
    }
    if (!ppm.empty() && !last.empty() && !write_ppm(ppm, last)) {
        std::fprintf(stderr, "cannot write %s\n", ppm.c_str());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
