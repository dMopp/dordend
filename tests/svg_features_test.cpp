// Stage 1 specifications, prepared before implementation and integrated after the stage 0 gate.
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
auto svg(std::string_view body, std::string_view attrs = "width='64' height='64'", int size = 64) {
    const std::string source = "<svg xmlns='http://www.w3.org/2000/svg' " + std::string(attrs) +
                               ">" + std::string(body) + "</svg>";
    return rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, size);
}

std::array<unsigned, 4> pixel(const rummage::picture::Image& image, unsigned x, unsigned y) {
    REQUIRE(x < image.width);
    REQUIRE(y < image.height);
    const auto at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return {image.rgba[at], image.rgba[at + 1], image.rgba[at + 2], image.rgba[at + 3]};
}

std::vector<std::uint8_t> bytes(const std::string& name) {
    std::ifstream f{std::string(RUMMAGE_TEST_DATA) + "/svg/" + name, std::ios::binary};
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}
}  // namespace

TEST_CASE("svg stage1: committed shape and paint references") {
    for (const std::string name :
         {"solid", "shapes", "paths", "compact-path", "strokes", "css", "colours",
          "linear-gradient", "gradient-inheritance", "radial-gradient", "gradient-repeat", "uses",
          "transforms", "visibility", "aspect-ratio", "units"}) {
        CAPTURE(name);
        const auto image = rummage::svg::render(bytes(name + ".svg"), 64);
        const auto reference = rummage::picture::decode_wuffs(bytes(name + ".png"), 4096);
        const auto difference = rummage::test::svg_difference(image, reference.image);
        CAPTURE(difference.maximum);
        CAPTURE(difference.fraction);
        CHECK(difference.passes());
    }
}

TEST_CASE("svg stage1: stylesheet specificity, source order and inheritance") {
    const auto image =
        svg("<style>rect{fill:red}.a{fill:blue}#b{fill:lime}.a{fill:yellow}</style>"
            "<g color='purple' fill='currentColor'><rect width='16' height='64' class='a'/>"
            "<rect x='16' width='16' height='64' class='a' id='b'/>"
            "<rect x='32' width='16' height='64' class='a' style='fill:inherit'/>"
            "<rect x='48' width='16' height='64' fill='black' style='fill:orange'/></g>");
    CHECK(pixel(image, 8, 8) == std::array<unsigned, 4>{255, 255, 0, 255});
    CHECK(pixel(image, 24, 8) == std::array<unsigned, 4>{0, 255, 0, 255});
    CHECK(pixel(image, 40, 8) == std::array<unsigned, 4>{128, 0, 128, 255});
    CHECK(pixel(image, 56, 8) == std::array<unsigned, 4>{255, 165, 0, 255});
}

TEST_CASE("svg stage1: named colours and rgba percentages") {
    CHECK(pixel(svg("<rect width='64' height='64' fill='lightgoldenrodyellow'/>"), 0, 0) ==
          std::array<unsigned, 4>{250, 250, 210, 255});
    CHECK(pixel(svg("<rect width='64' height='64' fill='rgba(100%,0%,0%,50%)'/>"), 0, 0) ==
          std::array<unsigned, 4>{255, 0, 0, 128});
    CHECK(pixel(svg("<rect width='64' height='64' fill='currentColor'/>"), 0, 0) ==
          std::array<unsigned, 4>{0, 0, 0, 255});
    CHECK(pixel(svg("<rect width='64' height='64' fill='url(#missing) red'/>"), 0, 0) ==
          std::array<unsigned, 4>{255, 0, 0, 255});
}

TEST_CASE("svg stage1: viewBox sizing, units and preserveAspectRatio") {
    for (std::string unit :
         {"64px", "48pt", "4pc", "16.933333333mm", "1.6933333333cm", ".6666666667in"}) {
        CAPTURE(unit);
        const auto image = svg("<rect width='100%' height='100%' fill='red'/>",
                               "width='" + unit + "' height='32px'");
        CHECK(image.width == 64);
        CHECK(image.height == 32);
        CHECK(pixel(image, 63, 31) == std::array<unsigned, 4>{255, 0, 0, 255});
    }
    CHECK(svg("<rect width='64' height='32'/>", "viewBox='0 0 64 32'").height == 32);
    CHECK(svg("<rect width='64' height='32'/>", "width='100%' height='100%' viewBox='0 0 64 32'")
              .height == 32);
    for (std::string x : {"xMin", "xMid", "xMax"}) {
        for (std::string y : {"YMin", "YMid", "YMax"}) {
            for (std::string mode : {"meet", "slice"}) {
                CAPTURE(x);
                CAPTURE(y);
                CAPTURE(mode);
                const auto image =
                    svg("<rect width='16' height='16' fill='red'/>",
                        "width='64' height='32' viewBox='0 0 16 16' preserveAspectRatio='" + x + y +
                            " " + mode + "'");
                REQUIRE_FALSE(image.empty());
                const unsigned offset = x == "xMin" ? 0 : x == "xMid" ? 16 : 32;
                CHECK(pixel(image, mode == "slice" ? 0 : offset, 0) ==
                      std::array<unsigned, 4>{255, 0, 0, 255});
            }
        }
    }
    const auto stretch =
        svg("<rect width='16' height='16' fill='red'/>",
            "width='64' height='32' viewBox='0 0 16 16' preserveAspectRatio='none'");
    CHECK(pixel(stretch, 63, 31) == std::array<unsigned, 4>{255, 0, 0, 255});
}

TEST_CASE("svg stage1: references are bounded and never fetch resources") {
    CHECK(svg("<defs><g id='a'><use href='#a'/></g></defs><use href='#a'/>").empty());
    CHECK(svg("<defs><linearGradient id='a' href='#b'/><linearGradient id='b' "
              "href='#a'/></defs><rect width='64' height='64' fill='url(#a)'/>")
              .empty());
    const auto image =
        svg("<use href='file:///etc/passwd'/><image "
            "href='https://example.com/a.png'/><text>ignored</text><rect width='64' height='64' "
            "fill='red'/>");
    CHECK(pixel(image, 0, 0) == std::array<unsigned, 4>{255, 0, 0, 255});
    std::string defs = "<defs><g id='a0'><rect width='1' height='1'/></g>";
    for (int i = 1; i < 16; ++i) {
        defs += "<g id='a" + std::to_string(i) + "'><use href='#a" + std::to_string(i - 1) +
                "'/><use href='#a" + std::to_string(i - 1) + "'/></g>";
    }
    CHECK(svg(defs + "</defs><use href='#a15'/>").empty());
}

TEST_CASE("svg stage1: linear gradient pixel centres and stop alpha") {
    const auto image = svg(
        "<defs><linearGradient id='g'><stop stop-color='red'/><stop offset='1' "
        "stop-color='blue'/></linearGradient></defs><rect width='64' height='64' fill='url(#g)'/>");
    CHECK(pixel(image, 0, 0) == std::array<unsigned, 4>{253, 0, 2, 255});
    CHECK(pixel(image, 31, 0) == std::array<unsigned, 4>{129, 0, 126, 255});
    CHECK(pixel(image, 63, 0) == std::array<unsigned, 4>{2, 0, 253, 255});
}

TEST_CASE("svg stage1: path grammar and segment cap") {
    CHECK(svg("<path d='M0 0Lnan 1'/>").empty());
    CHECK(svg("<path d='M0 0A1 1 0 2 0 1 1'/>").empty());
    std::string path = "<path d='M0 0";
    for (int i = 0; i < 1000001; ++i) {
        path += "l0 0";
    }
    CHECK(svg(path + "'/>").empty());
}

TEST_CASE("svg stage1: empty stroked subpaths and drawing after close are safe") {
    CHECK_FALSE(svg("<path d='M1 1 M2 2 L10 2' fill='none' stroke='red'/>").empty());
    CHECK_FALSE(svg("<path d='M1 1 L1 1 M2 2 Z M3 3 L3 3' fill='none' stroke='red' "
                    "stroke-linecap='round'/>")
                    .empty());
    CHECK_FALSE(svg("<path d='M1 1L10 1L10 10ZL12 12' fill='none' stroke='red'/>").empty());
}

TEST_CASE("svg stage1: symbol and nested SVG viewports clip overflowing content") {
    const auto image =
        svg("<defs><symbol id='s' viewBox='0 0 10 10'><rect x='-10' y='-10' width='30' height='30' "
            "fill='red'/></symbol></defs><use href='#s' x='16' y='16' width='16' height='16'/>");
    CHECK(pixel(image, 15, 16)[3] == 0);
    CHECK(pixel(image, 16, 16) == std::array<unsigned, 4>{255, 0, 0, 255});
    CHECK(pixel(image, 31, 31) == std::array<unsigned, 4>{255, 0, 0, 255});
    CHECK(pixel(image, 32, 31)[3] == 0);
    const auto nested =
        svg("<svg x='16' y='16' width='16' height='16' viewBox='0 0 10 10'><rect x='-10' y='-10' "
            "width='30' height='30'/></svg>");
    CHECK(pixel(nested, 15, 16)[3] == 0);
    CHECK(pixel(nested, 16, 16)[3] == 255);
    CHECK(pixel(nested, 32, 16)[3] == 0);
}

TEST_CASE("svg stage1: stylesheet expansion and live viewport layers are bounded") {
    std::string css = "<style>";
    for (int i = 0; i < 101; ++i) {
        css += (i ? ",.x" : ".x") + std::to_string(i);
    }
    css += "{";
    for (int i = 0; i < 1000; ++i) {
        css += "property" + std::to_string(i) + ":value;";
    }
    CHECK(svg(css + "}</style>").empty());
    std::string nested;
    for (int i = 0; i < 17; ++i) {
        nested += "<svg width='64' height='64'>";
    }
    nested += "<rect width='64' height='64'/>";
    for (int i = 0; i < 17; ++i) {
        nested += "</svg>";
    }
    CHECK(svg(nested).empty());
}

TEST_CASE("svg stage1: a closed dash without gaps retains the closing join") {
    const std::string path =
        "<path d='M16 16H48V48H16Z' fill='none' stroke='red' stroke-width='5' "
        "stroke-linecap='square' ";
    const auto solid = svg(path + "/>");
    const auto dashed = svg(path + "stroke-dasharray='1000 1'/>");
    REQUIRE_FALSE(solid.empty());
    CHECK(solid.rgba == dashed.rgba);
}

TEST_CASE("svg stage1: duplicate XML attributes and non-finite transforms are rejected") {
    CHECK(svg("<rect width='1' width='2' height='1'/>").empty());
    CHECK(svg("<rect width='64' height='64' transform='matrix(1 0 0 1 nan 0)'/>").empty());
    CHECK(svg("<rect width='64' height='64' transform='scale(1e300)'/>").empty());
}

TEST_CASE("svg stage1: dash fragments that collapse to one fixed point are safe") {
    CHECK_FALSE(svg("<path d='M7 14420 L34 14' fill='none' stroke='red' stroke-width='5' "
                    "stroke-dasharray='1 3' stroke-dashoffset='2'/>",
                    "width='64' height='64'", 16)
                    .empty());
    CHECK_FALSE(svg("<path d='M1 1L1.00000001 1' fill='none' stroke='red' stroke-width='.000001' "
                    "stroke-linecap='round'/>")
                    .empty());
}

TEST_CASE("svg stage1: the stroker tolerates curves below its internal precision") {
    constexpr std::string_view source =
        "<svg viewBox='0 0 64 64'><circle cx='44' cy='12' r='.1' stroke='purple'/></svg>";
    const auto image = rummage::svg::render(
        {reinterpret_cast<const std::uint8_t*>(source.data()), source.size()}, 16);
    REQUIRE(image.width == 16);
    REQUIRE(image.height == 16);
}

TEST_CASE("svg: subpixel horizontal strokes preserve their coverage") {
    const auto image = svg("<path d='M0 .5H64' fill='none' stroke='red' stroke-width='.5'/>");
    CHECK(pixel(image, 32, 0) == std::array<unsigned, 4>{255, 0, 0, 128});
    CHECK(pixel(image, 32, 1)[3] == 0);
}

TEST_CASE("svg: coincident cubic controls retain the endpoint tangent") {
    const auto image =
        svg("<path d='M16 16C16 16 16 48 48 48' fill='none' "
            "stroke='red' stroke-width='8'/>");
    // The first nonzero derivative points down. The butt cap is horizontal,
    // with no paint above y=16, and the left side starts at x=12.
    CHECK(pixel(image, 13, 16)[3] == 255);
    CHECK(pixel(image, 18, 15)[3] == 0);
}

TEST_CASE("svg: zero length segments do not remove thin stroke caps") {
    for (const std::string cap : {"round", "square"}) {
        const auto attrs =
            "' fill='none' stroke='red' stroke-width='.5' stroke-linecap='" + cap + "'/>";
        const auto simple = svg("<path d='M10.25 10.5H20.75" + attrs);
        const auto repeated = svg("<path d='M10.25 10.5h0H20.75h0" + attrs);
        CHECK(simple.rgba == repeated.rgba);
        const auto mixed = svg("<path d='M10.5 10.5h0 M30 30H40" + attrs);
        CHECK(pixel(mixed, 10, 10)[3] > 0);
        const auto move = svg("<path d='M10.5 10.5 M30 30H40" + attrs);
        CHECK(pixel(move, 10, 10)[3] == 0);
    }
}

TEST_CASE("svg: wide strokes cover the inside of tight curves") {
    const auto image =
        svg("<circle cx='32' cy='32' r='2' fill='none' "
            "stroke='red' stroke-width='12'/>");
    CHECK(pixel(image, 32, 32)[3] == 255);
    CHECK(pixel(image, 31, 31)[3] == 255);
    CHECK(pixel(image, 40, 32)[3] == 0);
}

TEST_CASE("svg: thin strokes paint their coverage union once") {
    const auto attrs = "' fill='none' stroke='red' stroke-opacity='.5'/>";
    const auto line = svg("<path d='M2 8.5H14" + std::string(attrs));
    for (const std::string path :
         {"M2 8.5H8.5H14", "M2 8.5H14M2 8.5H14", "M2 8.5H14H2", "M2 8.5H10M6 8.5H14"}) {
        CAPTURE(path);
        CHECK(svg("<path d='" + path + attrs).rgba == line.rgba);
    }
    CHECK(pixel(line, 8, 8)[3] == 128);
    const auto opaque = svg("<path d='M2 8.5H8.5H14' fill='none' stroke='red'/>");
    CHECK(pixel(opaque, 8, 8)[3] == 255);
    const auto crossing =
        svg("<path d='M2 8.5H14M8.5 2V14' fill='none' stroke='red' "
            "stroke-opacity='.5'/>");
    CHECK(pixel(crossing, 8, 8)[3] == 128);
    const auto vertical = svg("<path d='M8.5 2V14" + std::string(attrs));
    CHECK(svg("<path d='M8.5 2V8.5V14M8.5 14V2" + std::string(attrs)).rgba == vertical.rgba);
    const std::string gradient =
        "<defs><linearGradient id='g' gradientUnits='userSpaceOnUse' x2='64'>"
        "<stop stop-color='red'/><stop offset='1' stop-color='blue'/></linearGradient></defs>";
    const auto gradient_style = "' fill='none' stroke='url(#g)' stroke-opacity='.5'/>";
    CHECK(svg(gradient + "<path d='M2 8.5H14" + gradient_style).rgba ==
          svg(gradient + "<path d='M2 8.5H8.5H14M2 8.5H14" + gradient_style).rgba);
}

TEST_CASE("svg: dense overlapping strokes have bounded expansion") {
    std::string path;
    for (int i = 0; i < 8192; ++i)
        path += "M" + std::to_string(10.0 + i / 16384.0) + " 10.5H11";
    CHECK(svg("<path d='" + path + "' fill='none' stroke='red'/>").empty());
}

TEST_CASE("svg: a thin diagonal stroke covers its normal-width rectangle") {
    const auto stroke = svg("<path d='M2.5 2.5L13.5 13.5' fill='none' stroke='red'/>");
    // Width 1 gives normal offsets (+/-1/sqrt(8), -/+1/sqrt(8)).
    const auto rectangle =
        svg("<path d='M2.146446609 2.853553391L2.853553391 2.146446609"
            "L13.853553391 13.146446609L13.146446609 13.853553391Z' "
            "fill='red'/>");
    // Fixed-point cap coordinates can shift samples between the two end
    // pixels. The area and the interior normal-width footprint must agree.
    auto area = [](const auto& image) {
        unsigned total = 0;
        for (std::size_t i = 3; i < image.rgba.size(); i += 4)
            total += image.rgba[i];
        return total;
    };
    CHECK(area(stroke) == area(rectangle));
    CHECK(pixel(stroke, 8, 9) == pixel(rectangle, 8, 9));
}

TEST_CASE("svg: an unused move does not collapse a cubic stroke endpoint") {
    const std::string curve = "M20.908 48.421C31.005 27.126 27.797 15.946 27.797 15.946";
    const std::string style = "' fill='none' stroke='red' stroke-width='3'/>";
    const auto alone = svg("<path d='" + curve + style);
    const auto with_move = svg("<path d='M1 1 " + curve + style);
    CHECK(pixel(alone, 28, 16)[3] == 255);
    CHECK(pixel(with_move, 28, 16)[3] == 255);
    CHECK(alone.rgba == with_move.rgba);
}

TEST_CASE("svg hardening: CSS matching charges class scans and declaration expansion") {
    std::string rules = "<style>";
    for (int i = 0; i < 128; ++i)
        rules += ".absent{fill:red;}";
    rules += "</style>";
    std::string classes;
    for (int i = 0; i < 512; ++i)
        classes += "x ";
    std::string body = rules;
    for (int i = 0; i < 128; ++i)
        body += "<g class='" + classes + "'/>";
    CHECK(svg(body).empty());
    CHECK_FALSE(svg(rules + "<rect class='absent' width='1' height='1'/>").empty());

    rules = "<style>";
    for (int i = 0; i < 128; ++i) {
        rules += "g{";
        for (int j = 0; j < 128; ++j)
            rules += "property" + std::to_string(j) + ":value;";
        rules += "}";
    }
    body = rules + "</style>";
    for (int i = 0; i < 640; ++i)
        body += "<g/>";
    CHECK(svg(body).empty());
}

TEST_CASE("svg hardening: zero dash scans share the expansion budget") {
    std::string dash;
    for (int i = 0; i < 8192; ++i)
        dash += "0 ";
    dash += "1 1";
    const auto attrs = "' fill='none' stroke='red' stroke-dasharray='" + dash + "'/>";
    CHECK_FALSE(svg("<path d='M0 0H1" + attrs).empty());
    std::string path;
    for (int i = 0; i < 128; ++i)
        path += "M0 0H1";
    CHECK(svg("<path d='" + path + attrs).empty());
}

TEST_CASE("svg hardening: repeating textures keep extreme translations in range") {
    for (const std::string offset : {"-1e34", "1e34", "-1e36", "1e36"}) {
        const auto image =
            svg("<defs><pattern id='p' patternUnits='userSpaceOnUse' width='3' height='1' "
                "patternTransform='translate(" +
                offset +
                ")'><rect width='3' height='1' "
                "fill='red'/></pattern></defs><rect width='64' height='64' fill='url(#p)'/>");
        REQUIRE_FALSE(image.empty());
        CHECK(pixel(image, 32, 32) == std::array<unsigned, 4>{255, 0, 0, 255});
    }
}

TEST_CASE("svg hardening: extreme arcs reject before converting non-finite angles") {
    CHECK(svg("<path d='M0 0A1e12 1e12 0 0 0 1e-143 0'/>").empty());
    CHECK(svg("<path d='M0 0A1e12 1e12 0 0 1 0 1e-143'/>").empty());
    CHECK(svg("<path d='M0 0A1e-300 1e12 0 0 0 2 0'/>").empty());
}

TEST_CASE("svg hardening: inherited paint and dash text cannot bypass work limits") {
    const std::string large(1024U * 1024U, 'x');
    std::string rectangles;
    for (int i = 0; i < 65; ++i)
        rectangles += "<rect width='1' height='1'/>";
    CHECK(svg("<g fill='" + large + "'>" + rectangles + "</g>").empty());
    CHECK_FALSE(svg("<g fill='" + large + "'><rect width='1' height='1'/></g>").empty());

    std::string paths;
    for (int i = 0; i < 65; ++i)
        paths += "<path d='M0 0H1'/>";
    const std::string whitespace = "0" + std::string(1024U * 1024U, ' ') + "0";
    CHECK(svg("<g fill='none' stroke='red' stroke-dasharray='" + whitespace + "'>" + paths + "</g>")
              .empty());
}
