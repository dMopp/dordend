#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/picture/wuffs.hpp"
#include "rummage/svg/svg.hpp"
#include "svg_compare.hpp"

namespace {
auto svg(std::string_view body) {
    const std::string source = "<svg xmlns='http://www.w3.org/2000/svg' width='64' height='64'>" +
                               std::string(body) + "</svg>";
    return rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 64);
}

std::array<unsigned, 4> pixel(const rummage::picture::Image& im, unsigned x, unsigned y) {
    REQUIRE(x < im.width);
    REQUIRE(y < im.height);
    auto at = (static_cast<std::size_t>(y) * im.width + x) * 4;
    return {im.rgba[at], im.rgba[at + 1], im.rgba[at + 2], im.rgba[at + 3]};
}

std::vector<std::uint8_t> bytes(std::string_view name) {
    std::ifstream f{std::string(RUMMAGE_TEST_DATA) + "/svg/" + std::string(name), std::ios::binary};
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}
}  // namespace

TEST_CASE("svg stage2: committed layer references") {
    for (const std::string name : {"group-opacity", "clips", "clip-bbox", "mask"}) {
        CAPTURE(name);
        const auto actual = rummage::svg::render(bytes(name + ".svg"), 64);
        const auto expected = rummage::picture::decode_wuffs(bytes(name + ".png"), 4096);
        const auto diff = rummage::test::svg_difference(actual, expected.image);
        CAPTURE(diff.maximum);
        CAPTURE(diff.fraction);
        CHECK(diff.passes());
    }
}

TEST_CASE("svg stage2: opacity composites a group and use once") {
    const std::string content =
        "<rect width='48' height='48' fill='red'/><rect x='16' y='16' width='48' height='48' "
        "fill='blue'/>";
    const auto group = svg("<g opacity='.5'>" + content + "</g>");
    const auto use = svg("<defs><g id='a'>" + content + "</g></defs><use href='#a' opacity='.5'/>");
    CHECK(pixel(group, 24, 24) == std::array<unsigned, 4>{0, 0, 255, 128});
    CHECK(group.rgba == use.rgba);
    CHECK(pixel(svg("<g opacity='.5'><g opacity='.5'><rect width='64' height='64' "
                    "fill='red'/></g></g>"),
                0, 0) == std::array<unsigned, 4>{255, 0, 0, 64});
}

TEST_CASE("svg stage2: clipping units, transforms, use and clip on clip") {
    auto image =
        svg("<defs><rect id='r' width='32' height='64'/><clipPath id='a'><use href='#r' "
            "transform='translate(16)'/></clipPath><clipPath id='b' clip-path='url(#a)'><rect "
            "width='64' height='32'/></clipPath></defs><rect width='64' height='64' fill='red' "
            "clip-path='url(#b)'/>");
    CHECK(pixel(image, 15, 0)[3] == 0);
    CHECK(pixel(image, 16, 0)[3] == 255);
    CHECK(pixel(image, 47, 31)[3] == 255);
    CHECK(pixel(image, 48, 0)[3] == 0);
    CHECK(pixel(image, 16, 32)[3] == 0);
    image =
        svg("<defs><clipPath id='a' clipPathUnits='objectBoundingBox'><rect width='.5' "
            "height='1'/></clipPath></defs><g clip-path='url(#a)'><rect x='16' y='16' width='32' "
            "height='32' fill='blue'/></g>");
    CHECK(pixel(image, 31, 16)[3] == 255);
    CHECK(pixel(image, 32, 16)[3] == 0);
}

TEST_CASE("svg stage2: mask luminance uses straight colour and alpha once") {
    const auto luminance =
        svg("<defs><mask id='m'><rect width='64' height='64' fill='blue' "
            "opacity='.5'/></mask></defs><rect width='64' height='64' fill='red' mask='url(#m)'/>");
    CHECK(pixel(luminance, 10, 10) == std::array<unsigned, 4>{255, 0, 0, 9});
    const auto alpha =
        svg("<defs><mask id='m' mask-type='alpha'><rect width='64' height='64' fill='black' "
            "opacity='.5'/></mask></defs><rect width='64' height='64' fill='red' mask='url(#m)'/>");
    CHECK(pixel(alpha, 10, 10) == std::array<unsigned, 4>{255, 0, 0, 128});
}

TEST_CASE("svg stage2: mask region, content units and nested masks") {
    const auto image =
        svg("<defs><mask id='a' maskContentUnits='objectBoundingBox' x='0' y='0' width='.5' "
            "height='1'><rect width='1' height='1' fill='white'/></mask></defs><rect x='16' y='16' "
            "width='32' height='32' fill='red' mask='url(#a)'/>");
    CHECK(pixel(image, 31, 16)[3] == 255);
    CHECK(pixel(image, 32, 16)[3] == 0);
    const auto nested = svg(
        "<defs><mask id='a'><rect width='64' height='64' fill='white' opacity='.5'/></mask><mask "
        "id='b'><rect width='64' height='64' fill='white' opacity='.5' "
        "mask='url(#a)'/></mask></defs><rect width='64' height='64' fill='red' mask='url(#b)'/>");
    CHECK(pixel(nested, 10, 10) == std::array<unsigned, 4>{255, 0, 0, 64});
}

TEST_CASE("svg stage2: reference cycles and offscreen layer limits fail cleanly") {
    CHECK(svg("<defs><clipPath id='a' clip-path='url(#a)'><rect width='64' "
              "height='64'/></clipPath></defs><rect width='64' height='64' clip-path='url(#a)'/>")
              .empty());
    CHECK(svg("<defs><mask id='a'><rect width='64' height='64' mask='url(#a)'/></mask></defs><rect "
              "width='64' height='64' mask='url(#a)'/>")
              .empty());
    std::string deep;
    for (int i = 0; i < 17; ++i)
        deep += "<g opacity='.5'>";
    deep += "<rect width='64' height='64'/>";
    for (int i = 0; i < 17; ++i)
        deep += "</g>";
    CHECK(svg(deep).empty());
}

TEST_CASE("svg stage2: bounds of a referenced SVG apply its viewport once") {
    const auto image = svg(
        "<defs><svg id='v' viewBox='0 0 16 16' width='16' height='16'><rect width='16' "
        "height='16'/></svg><clipPath id='c' clipPathUnits='objectBoundingBox'><rect width='.5' "
        "height='1'/></clipPath></defs><use href='#v' x='16' y='16' width='32' height='32' "
        "clip-path='url(#c)'/>");
    CHECK(pixel(image, 15, 16)[3] == 0);
    CHECK(pixel(image, 16, 16)[3] == 255);
    CHECK(pixel(image, 31, 47)[3] == 255);
    CHECK(pixel(image, 32, 16)[3] == 0);
}
