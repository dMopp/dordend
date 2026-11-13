#include "rummage/ui/xcursor.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

using rummage::ui::CursorShape;
using rummage::ui::xcursor_names;
using rummage::ui::xcursor_size;

namespace {

constexpr std::array kShapes{
    CursorShape::standard,  CursorShape::pointer,     CursorShape::text,
    CursorShape::wait,      CursorShape::progress,    CursorShape::grab,
    CursorShape::grabbing,  CursorShape::not_allowed, CursorShape::ew_resize,
    CursorShape::ns_resize, CursorShape::n_resize,    CursorShape::s_resize,
    CursorShape::e_resize,  CursorShape::w_resize,    CursorShape::ne_resize,
    CursorShape::nw_resize, CursorShape::se_resize,   CursorShape::sw_resize};

}  // namespace

TEST_CASE("xcursor: every shape has the cursor spec's name, then an X11 one") {
    for (const CursorShape shape : kShapes) {
        const auto names = xcursor_names(shape);
        REQUIRE(names.size() >= 2);
        CHECK_MESSAGE(names.front().find('_') == std::string_view::npos, names.front());
    }
    CHECK(xcursor_names(CursorShape::standard).front() == "default");
    CHECK(xcursor_names(CursorShape::text).back() == "xterm");
    CHECK(xcursor_names(CursorShape::ew_resize).front() == "ew-resize");
}

TEST_CASE("xcursor: XCURSOR_SIZE") {
    CHECK(xcursor_size(nullptr) == 24);
    CHECK(xcursor_size("32") == 32);
    CHECK(xcursor_size("") == 24);
    CHECK(xcursor_size("0") == 24);
    CHECK(xcursor_size("-5") == 24);
    CHECK(xcursor_size("big") == 24);
    CHECK(xcursor_size("48px") == 24);
    CHECK(xcursor_size("1000") == 24);
}

TEST_CASE("xcursor: every shape is in the Adwaita theme") {
    const std::filesystem::path dir{"/usr/share/icons/Adwaita/cursors"};
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        MESSAGE("no Adwaita cursor theme on this system: skipped");
        return;
    }
    for (const CursorShape shape : kShapes) {
        const std::string first{xcursor_names(shape).front()};
        CHECK_MESSAGE(std::filesystem::exists(dir / first, error), first);
    }
}
