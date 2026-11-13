#include "rummage/views/title_bar.hpp"

#include <optional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

using rummage::ui::CursorShape;
using rummage::ui::Edge;
using rummage::ui::PointerButton;
using rummage::ui::PointerEvent;
using rummage::views::TitleBar;
using Part = TitleBar::Part;
using Kind = PointerEvent::Kind;

namespace {

constexpr rummage::ui::Rect kWindow{0.0F, 0.0F, 800.0F, 600.0F};

// A title bar on an 800x600 window whose actions are written down.
struct Fixture {
    std::vector<std::string> actions;
    std::optional<Edge> resized;
    std::vector<CursorShape> cursors;
    int redraws{0};
    TitleBar bar{{.move = [this] { actions.emplace_back("move"); },
                  .resize =
                      [this](Edge edge) {
                          actions.emplace_back("resize");
                          resized = edge;
                      },
                  .minimize = [this] { actions.emplace_back("minimize"); },
                  .toggle_maximized = [this] { actions.emplace_back("maximize"); },
                  .window_menu = [this](float /*x*/, float /*y*/) { actions.emplace_back("menu"); },
                  .close = [this] { actions.emplace_back("close"); }},
                 [this] { ++redraws; },
                 [this](CursorShape shape) { cursors.push_back(shape); }};

    Fixture() { bar.layout(kWindow); }

    bool send(Kind kind, float x, float y, PointerButton button = PointerButton::left,
              int clicks = 1) {
        return bar.pointer({.kind = kind, .x = x, .y = y, .button = button, .click_count = clicks});
    }

    bool click(float x, float y) {
        const bool taken = send(Kind::press, x, y);
        send(Kind::release, x, y);
        return taken;
    }
};

// The middle of a button: close is the rightmost, then maximize, then minimize.
constexpr float kButtonY = 18.0F;
constexpr float kClose = 800.0F - 8.0F - 12.0F;
constexpr float kMaximize = kClose - 30.0F;
constexpr float kMinimize = kMaximize - 30.0F;

}  // namespace

TEST_CASE("title bar: what is where") {
    Fixture f;
    CHECK(f.bar.hit(400.0F, 18.0F).part == Part::title);
    CHECK(f.bar.hit(kClose, kButtonY).part == Part::close);
    CHECK(f.bar.hit(kMaximize, kButtonY).part == Part::maximize);
    CHECK(f.bar.hit(kMinimize, kButtonY).part == Part::minimize);
    CHECK(f.bar.hit(400.0F, 300.0F).part == Part::none);  // the content
    CHECK(f.bar.hit(900.0F, 18.0F).part == Part::none);   // outside

    const auto edge = [&](float x, float y) {
        const TitleBar::Hit hit = f.bar.hit(x, y);
        REQUIRE(hit.part == Part::edge);
        return hit.edge;
    };
    CHECK(edge(400.0F, 2.0F) == Edge::top);
    CHECK(edge(400.0F, 598.0F) == Edge::bottom);
    CHECK(edge(1.0F, 300.0F) == Edge::left);
    CHECK(edge(797.0F, 300.0F) == Edge::right);
    CHECK(edge(1.0F, 1.0F) == Edge::top_left);
    CHECK(edge(10.0F, 2.0F) == Edge::top_left);  // a corner reaches along its edges
    CHECK(edge(2.0F, 10.0F) == Edge::top_left);
    CHECK(edge(797.0F, 2.0F) == Edge::top_right);
    CHECK(edge(2.0F, 597.0F) == Edge::bottom_left);
    CHECK(edge(790.0F, 598.0F) == Edge::bottom_right);

    CHECK(f.bar.content(kWindow).y == TitleBar::kHeight);
    CHECK(f.bar.content(kWindow).height == 600.0F - TitleBar::kHeight);
}

TEST_CASE("title bar: maximized, tiled, fullscreen, without minimize") {
    Fixture f;
    f.bar.set_state({.maximized = true});
    CHECK(f.bar.hit(400.0F, 2.0F).part == Part::title);  // no edges
    CHECK(f.bar.hit(1.0F, 300.0F).part == Part::none);

    f.bar.set_state({.tiled_left = true, .tiled_top = true});
    CHECK(f.bar.hit(1.0F, 300.0F).part == Part::none);
    CHECK(f.bar.hit(400.0F, 2.0F).part == Part::title);
    CHECK(f.bar.hit(1.0F, 598.0F).edge == Edge::bottom);  // the corner's left side is tiled
    CHECK(f.bar.hit(797.0F, 300.0F).edge == Edge::right);

    f.bar.set_state({.fullscreen = true});
    CHECK(f.bar.hit(400.0F, 18.0F).part == Part::none);
    CHECK(f.bar.content(kWindow).y == 0.0F);

    f.bar.set_state({.can_minimize = false});
    CHECK(f.bar.hit(kMaximize, kButtonY).part == Part::maximize);
    CHECK(f.bar.hit(kMinimize, kButtonY).part == Part::title);
}

TEST_CASE("title bar: the buttons act on release over them") {
    Fixture f;
    CHECK(f.click(kClose, kButtonY));
    CHECK(f.click(kMaximize, kButtonY));
    CHECK(f.click(kMinimize, kButtonY));
    CHECK(f.actions == std::vector<std::string>{"close", "maximize", "minimize"});

    // Pressed, then released elsewhere: nothing, and the bar keeps the pointer until then.
    f.actions.clear();
    CHECK(f.send(Kind::press, kClose, kButtonY));
    CHECK(f.send(Kind::motion, 400.0F, 300.0F));
    CHECK(f.send(Kind::release, 400.0F, 300.0F));
    CHECK(f.actions.empty());
    CHECK_FALSE(f.send(Kind::motion, 400.0F, 300.0F));  // the content again
}

TEST_CASE("title bar: drag, double click, right click, edges") {
    Fixture f;
    CHECK(f.send(Kind::press, 300.0F, 18.0F));
    CHECK(f.send(Kind::press, 300.0F, 18.0F, PointerButton::left, 2));
    CHECK(f.send(Kind::press, 300.0F, 18.0F, PointerButton::right));
    CHECK(f.send(Kind::press, 797.0F, 598.0F));
    CHECK(f.actions == std::vector<std::string>{"move", "maximize", "menu", "resize"});
    CHECK(f.resized == Edge::bottom_right);

    // The content's presses are not the bar's.
    f.actions.clear();
    CHECK_FALSE(f.send(Kind::press, 400.0F, 300.0F));
    CHECK(f.actions.empty());

    // No maximize, no double click; no window menu, no right click.
    f.bar.set_state({.can_maximize = false, .has_window_menu = false});
    f.send(Kind::press, 300.0F, 18.0F, PointerButton::left, 2);
    f.send(Kind::press, 300.0F, 18.0F, PointerButton::right);
    CHECK(f.actions == std::vector<std::string>{"move"});
}

TEST_CASE("title bar: the edges' cursors") {
    Fixture f;
    f.send(Kind::motion, 400.0F, 2.0F);
    f.send(Kind::motion, 1.0F, 1.0F);
    f.send(Kind::motion, 797.0F, 300.0F);
    f.send(Kind::motion, 400.0F, 18.0F);   // the title: back to the arrow
    f.send(Kind::motion, 400.0F, 300.0F);  // the content: the bar leaves the cursor alone
    CHECK(f.cursors == std::vector<CursorShape>{CursorShape::n_resize, CursorShape::nw_resize,
                                                CursorShape::e_resize, CursorShape::standard});

    // Hovering a button redraws; leaving the window forgets it.
    const int redraws = f.redraws;
    f.send(Kind::motion, kClose, kButtonY);
    CHECK(f.redraws == redraws + 1);
    CHECK_FALSE(f.bar.pointer({.kind = Kind::leave}));
    CHECK(f.redraws == redraws + 2);
}
