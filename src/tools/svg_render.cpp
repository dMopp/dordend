#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "rummage/picture/png.hpp"
#include "rummage/picture/wuffs.hpp"
#include "rummage/svg/svg.hpp"
#include "svg_compare.hpp"

namespace {
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 8U * 1024U * 1024U) {
        throw std::runtime_error("input too large");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input{path, std::ios::binary};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("cannot read input");
    }
    return bytes;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) {
            std::cerr << "Usage: rummage-svg-render INPUT.svg PIXELS OUTPUT.png [REFERENCE.png]\n";
            return 2;
        }
        int pixels{};
        const std::string_view value{argv[2]};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), pixels);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return 2;
        }
        const auto data = read(argv[1]);
        const auto start = std::chrono::steady_clock::now();
        bool unsupported_filter = false;
        const auto image = rummage::svg::render(
            data, pixels,
            {[](void* context, std::string_view) { *static_cast<bool*>(context) = true; },
             &unsupported_filter});
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (unsupported_filter)
            std::cerr << "unsupported filter primitive: rendered unfiltered\n";
        if (image.empty())
            std::cerr << "SVG parse or render limit failure\n";
        rummage::test::SvgDifference difference;
        if (!image.empty()) {
            const auto png = rummage::picture::encode_png(image);
            std::ofstream output{argv[3], std::ios::binary};
            if (png.empty() || !output.write(reinterpret_cast<const char*>(png.data()),
                                             static_cast<std::streamsize>(png.size()))) {
                throw std::runtime_error("cannot write output");
            }
            if (argc == 5) {
                const auto reference = rummage::picture::decode_wuffs(read(argv[4]), 1024U * 1024U);
                difference = rummage::test::svg_difference(image, reference.image);
            }
        }
        std::cout << std::setprecision(12) << seconds << '\t' << difference.maximum << '\t'
                  << difference.fraction << '\t' << difference.passes() << '\n';
        return image.empty() ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
