#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/picture/png.hpp"
#include "rummage/picture/wuffs.hpp"
#include "rummage/svg/svg.hpp"
#include "svg_compare.hpp"

namespace {
auto svg(std::string_view body) {
    const auto source = "<svg width='64' height='64'>" + std::string(body) + "</svg>";
    return rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 64);
}

std::array<unsigned, 4> pixel(const rummage::picture::Image& im, unsigned x, unsigned y) {
    REQUIRE(x < im.width);
    REQUIRE(y < im.height);
    const auto at = (static_cast<std::size_t>(y) * im.width + x) * 4;
    return {im.rgba[at], im.rgba[at + 1], im.rgba[at + 2], im.rgba[at + 3]};
}

std::vector<std::uint8_t> bytes(std::string_view name) {
    std::ifstream f{std::string(RUMMAGE_TEST_DATA) + "/svg/" + std::string(name), std::ios::binary};
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

std::string base64(std::span<const std::uint8_t> b) {
    constexpr auto alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        unsigned n = static_cast<unsigned>(b[i]) << 16;
        if (i + 1 < b.size())
            n |= static_cast<unsigned>(b[i + 1]) << 8;
        if (i + 2 < b.size())
            n |= b[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < b.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < b.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
}  // namespace

TEST_CASE("svg stage3: committed effect references") {
    for (const std::string name : {"adwaita-mask", "blur", "filter-chain", "pattern"}) {
        CAPTURE(name);
        const auto actual = rummage::svg::render(bytes(name + ".svg"), 64);
        const auto expected = rummage::picture::decode_wuffs(bytes(name + ".png"), 4096);
        const auto diff = rummage::test::svg_difference(actual, expected.image);
        CAPTURE(diff.maximum);
        CAPTURE(diff.fraction);
        CHECK(diff.passes());
    }
}

TEST_CASE("svg stage3: colour matrices use straight colour and preserve alpha") {
    auto im =
        svg("<defs><filter id='f' color-interpolation-filters='sRGB'><feColorMatrix "
            "values='1 0 0 0 0 0 1 0 0 0 0 0 1 0 0 0 0 0 1 0'/></filter></defs><rect "
            "width='64' height='64' fill='red' fill-opacity='.5' filter='url(#f)'/>");
    CHECK(pixel(im, 32, 32) == std::array<unsigned, 4>{255, 0, 0, 128});
    im =
        svg("<defs><filter id='f' color-interpolation-filters='sRGB'><feColorMatrix "
            "type='luminanceToAlpha'/></filter></defs><rect width='64' height='64' "
            "fill='blue' filter='url(#f)'/>");
    CHECK(pixel(im, 32, 32) == std::array<unsigned, 4>{0, 0, 0, 18});
    im =
        svg("<defs><filter id='f'><feColorMatrix in='SourceAlpha' values='0 0 0 "
            "0 1 0 0 0 0 1 0 0 0 0 1 0 0 0 1 0' result='white'/><feGaussianBlur "
            "in='white' stdDeviation='0'/></filter></defs><rect width='64' "
            "height='64' fill='blue' fill-opacity='.5' filter='url(#f)'/>");
    CHECK(pixel(im, 32, 32) == std::array<unsigned, 4>{255, 255, 255, 128});
}

TEST_CASE("svg stage3: filter and primitive regions clip output") {
    const auto im =
        svg("<defs><filter id='f' filterUnits='userSpaceOnUse' x='16' y='16' "
            "width='32' height='32'><feColorMatrix x='24' y='24' width='16' "
            "height='16' values='0 0 0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0 1 "
            "0'/></filter></defs><rect width='64' height='64' filter='url(#f)'/>");
    CHECK(pixel(im, 23, 24)[3] == 0);
    CHECK(pixel(im, 24, 24)[3] == 255);
    CHECK(pixel(im, 40, 24)[3] == 0);
}

TEST_CASE("svg stage3: blur includes source outside the viewport") {
    const auto im =
        svg("<defs><filter id='f' x='-100%' y='-100%' width='300%' "
            "height='300%'><feGaussianBlur stdDeviation='3'/></filter></defs><rect "
            "x='-8' y='16' width='8' height='32' fill='blue' filter='url(#f)'/>");
    CHECK(pixel(im, 0, 32)[3] > 70);
    CHECK(pixel(im, 10, 32)[3] == 0);
}

TEST_CASE("svg stage3: unsupported filters keep the unfiltered element") {
    const auto im =
        svg("<defs><filter id='f'><feColorMatrix "
            "type='luminanceToAlpha'/><feOffset dx='20'/></filter></defs><rect "
            "width='64' height='64' fill='red' filter='url(#f)'/>");
    CHECK(pixel(im, 0, 0) == std::array<unsigned, 4>{255, 0, 0, 255});
}

TEST_CASE("svg stage3: patterns inherit tiles and map object bounding boxes") {
    const auto im =
        svg("<defs><pattern id='a' width='.5' height='.5' "
            "patternContentUnits='objectBoundingBox'><rect width='.25' height='.5' "
            "fill='red'/></pattern><pattern id='b' href='#a'/></defs><rect x='16' "
            "y='16' width='32' height='32' fill='url(#b)'/>");
    CHECK(pixel(im, 16, 16) == std::array<unsigned, 4>{255, 0, 0, 255});
    CHECK(pixel(im, 24, 16)[3] == 0);
    CHECK(pixel(im, 32, 16)[3] == 255);
    const auto vb =
        svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='16' "
            "height='16' viewBox='0 0 2 2'><rect width='1' height='2' "
            "fill='blue'/></pattern></defs><rect width='64' height='64' "
            "fill='url(#p)'/>");
    CHECK(pixel(vb, 7, 0) == std::array<unsigned, 4>{0, 0, 255, 255});
    CHECK(pixel(vb, 8, 0)[3] == 0);
}

TEST_CASE(
    "svg stage3: embedded PNGs preserve straight alpha and ignore "
    "external images") {
    rummage::picture::Image source;
    source.width = source.height = 1;
    source.rgba = {255, 0, 0, 128};
    const auto data = base64(rummage::picture::encode_png(source));
    const auto im =
        svg("<image width='64' height='64' href='data:image/png;base64," + data + "'/>");
    CHECK(pixel(im, 32, 32) == std::array<unsigned, 4>{255, 0, 0, 128});
    CHECK(pixel(svg("<image width='64' height='64' href='file:///etc/passwd'/><rect "
                    "width='1' height='1' fill='blue'/>"),
                0, 0) == std::array<unsigned, 4>{0, 0, 255, 255});
}

TEST_CASE("svg stage3: pattern and filter cycles and budgets fail cleanly") {
    CHECK(svg("<defs><pattern id='a' href='#b'/><pattern id='b' "
              "href='#a'/></defs><rect width='64' height='64' fill='url(#a)'/>")
              .empty());
    CHECK(svg("<defs><pattern id='p' width='1' height='1'><rect width='64' "
              "height='64' fill='url(#p)'/></pattern></defs><rect width='64' "
              "height='64' fill='url(#p)'/>")
              .empty());
    CHECK(svg("<defs><filter id='a' href='#b'/><filter id='b' "
              "href='#a'/></defs><rect width='64' height='64' filter='url(#a)'/>")
              .empty());
}

TEST_CASE("svg stage3: unsupported filter warnings occur once per document") {
    const std::string source =
        "<svg width='8' height='8'><defs><filter id='f'><feOffset/></filter><filter "
        "id='g'><feBlend/></filter></defs><rect width='8' height='8' filter='url(#f)'/><rect "
        "width='8' height='8' filter='url(#g)'/></svg>";
    unsigned warnings = 0;
    const auto image = rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 8,
        {[](void* context, std::string_view message) {
             CHECK_FALSE(message.empty());
             ++*static_cast<unsigned*>(context);
         },
         &warnings});
    CHECK_FALSE(image.empty());
    CHECK(warnings == 1);
}

TEST_CASE("svg stage3: embedded JPEG uses the existing decoder") {
    std::ifstream file{std::string(RUMMAGE_TEST_DATA) + "/image/blocks.jpg", std::ios::binary};
    REQUIRE(file.good());
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>{file},
                                         std::istreambuf_iterator<char>{}};
    const auto decoded = rummage::picture::decode_wuffs(data, 1024U * 1024U);
    REQUIRE_FALSE(decoded.image.empty());
    const auto& expected = decoded.image;
    const auto source = "<svg width='" + std::to_string(expected.width) + "' height='" +
                        std::to_string(expected.height) +
                        "'><image width='100%' height='100%' href='data:image/jpeg;base64," +
                        base64(data) + "'/></svg>";
    const auto image =
        rummage::svg::render({reinterpret_cast<const std::uint8_t*>(source.data()), source.size()},
                             static_cast<int>(std::max(expected.width, expected.height)));
    CHECK(image.rgba == expected.rgba);
}

TEST_CASE("svg stage3: decoded image pixels and pattern tiles are bounded") {
    rummage::picture::Image source;
    source.width = source.height = 1025;
    source.rgba.resize(1025U * 1025U * 4, 255);
    const auto data = base64(rummage::picture::encode_png(source));
    CHECK(svg("<image width='64' height='64' href='data:image/png;base64," + data + "'/>").empty());
    CHECK(svg("<image width='64' height='64' href='data:image/png;base64,!!!!'/>").empty());
    CHECK(
        svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='100000' height='1'><rect "
            "width='1' height='1'/></pattern></defs><rect width='64' height='64' fill='url(#p)'/>")
            .empty());
}

TEST_CASE("svg stage3: excessive blur sigma is clamped") {
    const std::string start =
        "<defs><filter id='f' x='-100%' y='-100%' width='300%' height='300%'><feGaussianBlur "
        "stdDeviation='";
    const std::string end =
        "'/></filter></defs><rect x='16' y='16' width='32' height='32' filter='url(#f)'/>";
    const auto capped = svg(start + "32" + end), large = svg(start + "1e300" + end);
    REQUIRE_FALSE(large.empty());
    CHECK(large.rgba == capped.rgba);
}

TEST_CASE("svg stage3: empty contours still consume the raster work budget") {
    std::string source = "<svg width='1024' height='1024'>";
    for (int i = 0; i < 65; ++i)
        source += "<path d='M0 0L1024 1024'/>";
    source += "</svg>";
    CHECK(rummage::svg::render(
              {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 1024)
              .empty());
}

TEST_CASE("svg: offscreen curve expansion is bounded before allocating scanline crossings") {
    std::string path = "<path d='";
    for (int i = 0; i < 4096; ++i)
        path += "M0 0C32760 32760 -32760 32760 0 0";
    path += "'/>";
    CHECK(svg(path).empty());
}

TEST_CASE("svg hardening: padded filter layers obey the aggregate memory cap") {
    const std::string source =
        "<svg width='1024' height='1024'><defs><filter id='f'>"
        "<feColorMatrix/><feColorMatrix/><feColorMatrix/><feColorMatrix/>"
        "<feGaussianBlur stdDeviation='32'/><feGaussianBlur stdDeviation='32'/>"
        "<feGaussianBlur stdDeviation='32'/><feGaussianBlur stdDeviation='32'/>"
        "</filter></defs><rect width='1' height='1' filter='url(#f)'/></svg>";
    // Four later blurs reserve 384 px of padding. Each layer is 12.25 MiB;
    // the fourth matrix's clip layer exceeds 64 MiB with only six live
    // layers, before blur work can exhaust the separate canvas work limit.
    CHECK(rummage::svg::render(
              {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 1024)
              .empty());
}
