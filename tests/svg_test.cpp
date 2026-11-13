#include "rummage/svg/svg.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "rummage/picture/wuffs.hpp"
#include "svg_compare.hpp"

namespace {
rummage::picture::Image render(std::string_view source, int pixels = 64) {
    return rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, pixels);
}
}  // namespace

TEST_CASE("svg: invalid input and dimensions fail cleanly") {
    CHECK(render("").empty());
    CHECK(render("not SVG").empty());
    CHECK(render("<svg/>").empty());
    CHECK(render("<svg width='nan' height='1'/>").empty());
    CHECK(render("<svg width='1e999' height='1'/>").empty());
    CHECK(render("<svg width='64' height='64'/>", 0).empty());
    CHECK(render("<svg width='64' height='64'/>", 1000000).empty());
    CHECK(render(std::string(8U * 1024U * 1024U + 1U, ' ')).empty());
    CHECK(render("<svg width='64' height='64'><rect></svg>").empty());
    CHECK(render("<svg width='64' height='64'/><svg/>").empty());
}

TEST_CASE("svg: rectangle retains aspect ratio and straight alpha") {
    const auto image = render(
        "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10'>"
        "<rect width='20' height='10' fill='#ff0000' opacity='.5'/></svg>",
        80);
    REQUIRE_FALSE(image.empty());
    CHECK(image.width == 80);
    CHECK(image.height == 40);
    REQUIRE(image.rgba.size() == 80 * 40 * 4);
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        CHECK(image.rgba[i] == 255);
        CHECK(image.rgba[i + 1] == 0);
        CHECK(image.rgba[i + 2] == 0);
        CHECK(image.rgba[i + 3] == 128);
    }
}

TEST_CASE("svg: XML nesting and element limits reject before rendering") {
    std::string source = "<svg width='1' height='1'>";
    for (int i = 0; i < 257; ++i) {
        source += "<g>";
    }
    for (int i = 0; i < 257; ++i) {
        source += "</g>";
    }
    source += "</svg>";
    CHECK(render(source).empty());
    source = "<svg width='1' height='1'>";
    for (int i = 0; i < 100001; ++i) {
        source += "<g/>";
    }
    CHECK(render(source + "</svg>").empty());
}

TEST_CASE("svg: XML declarations, comments, namespaces and entities") {
    const auto image = render(
        "<?xml version='1.0'?><!-- icon -->"
        "<!DOCTYPE svg [<!ENTITY ignored 'red'>]>"
        "<s:svg xmlns:s='http://www.w3.org/2000/svg' width='1' height='1'>"
        "<s:rect width='1' height='1' fill='&#35;ff0000'/></s:svg>",
        1);
    REQUIRE_FALSE(image.empty());
    CHECK(image.rgba == std::vector<std::uint8_t>{255, 0, 0, 255});
    CHECK(render("<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>"
                 "<svg width='1' height='1'>&x;</svg>")
              .empty());
}

TEST_CASE("svg: large coordinates and raster work fail cleanly") {
    CHECK(render("<svg width='1' height='1'><rect width='1e300' height='1'/></svg>").empty());
    std::string source = "<svg width='64' height='64'>";
    for (int i = 0; i < 65; ++i) {
        source += "<rect width='64' height='64'/>";
    }
    CHECK(render(source + "</svg>", 1024).empty());
}

TEST_CASE("svg: solid rectangle matches its captured reference") {
    const auto read = [](std::string_view suffix) {
        std::ifstream input{std::string{RUMMAGE_TEST_DATA} + "/svg/solid." + std::string{suffix},
                            std::ios::binary};
        REQUIRE(input.good());
        return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input},
                                         std::istreambuf_iterator<char>{}};
    };
    const auto image = rummage::svg::render(read("svg"), 64);
    const auto reference = rummage::picture::decode_wuffs(read("png"), 4096);
    REQUIRE_FALSE(reference.image.empty());
    CHECK(rummage::test::svg_difference(image, reference.image).passes());
}

TEST_CASE("svg: reference comparison tolerates transparent edges but catches opaque errors") {
    const rummage::picture::Image transparent{.width = 1, .height = 1, .rgba = {0, 0, 0, 0}};
    const rummage::picture::Image edge{.width = 1, .height = 1, .rgba = {255, 255, 0, 1}};
    const rummage::picture::Image opaque{.width = 1, .height = 1, .rgba = {255, 255, 0, 255}};
    CHECK(rummage::test::svg_difference(edge, transparent).passes());
    CHECK_FALSE(rummage::test::svg_difference(opaque, transparent).passes());
}

TEST_CASE("svg hardening: the input cap includes otherwise ignored content") {
    const std::string prefix = "<svg width='1' height='1'><!--";
    const std::string suffix = "--><rect width='1' height='1' fill='red'/></svg>";
    const auto source =
        prefix + std::string(8U * 1024U * 1024U - prefix.size() - suffix.size(), 'x') + suffix;
    CHECK_FALSE(render(source, 1).empty());
    CHECK(render(source + " ", 1).empty());
}

TEST_CASE("svg hardening: exceptions from warning callbacks cannot escape render") {
    const std::string source =
        "<svg width='1' height='1'><filter id='f'><feOffset/></filter>"
        "<rect width='1' height='1' filter='url(#f)'/></svg>";
    CHECK(
        rummage::svg::render({reinterpret_cast<const std::uint8_t*>(source.data()), source.size()},
                             1, {[](void*, std::string_view) { throw 1; }, nullptr})
            .empty());
    CHECK_FALSE(render("<svg width='1' height='1'/>", 1).empty());
}
