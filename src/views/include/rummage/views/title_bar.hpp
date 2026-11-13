#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/window.hpp"

namespace rummage::views {

// What the title bar asks of the window; wired by main to ui::Window, faked in tests.
struct WindowControls {
    std::function<void()> move;
    std::function<void(ui::Edge edge)> resize;
    std::function<void()> minimize;
    std::function<void()> toggle_maximized;
    std::function<void(float x, float y)> window_menu;
    std::function<void()> close;
};

// The title bar and the resize edges rummage draws itself when the compositor draws none
// (client-side decorations, ROADMAP 7.4): the title in the middle, minimize, maximize and close
// at the right. Dragging the bar moves the window, a double click maximizes it, a right click
// opens the compositor's window menu, and a band along the window's edges resizes it, with the
// matching cursor. Maximized or fullscreen the edges do not resize; fullscreen there is no bar.
//
// Hit tests use the last layout() (draw() lays out). Loop thread only.
class TitleBar {
public:
    static constexpr float kHeight = 36.0F;
    static constexpr float kEdge = 5.0F;     // the resize band, inside the window
    static constexpr float kCorner = 16.0F;  // how far a corner reaches along its edges

    enum class Part : std::uint8_t { none, title, minimize, maximize, close, edge };

    struct Hit {
        Part part{Part::none};
        ui::Edge edge{ui::Edge::top};  // when part is edge

        bool operator==(const Hit&) const = default;
    };

    TitleBar(WindowControls controls, std::function<void()> redraw,
             std::function<void(ui::CursorShape)> set_cursor);

    void set_state(const ui::WindowState& state);

    // Where the bar and the buttons go in bounds (the whole window).
    void layout(const ui::Rect& bounds);

    // What is left for the content under the bar.
    [[nodiscard]] ui::Rect content(const ui::Rect& bounds) const;

    [[nodiscard]] Hit hit(float x, float y) const;

    // A pointer event over the window. True when the bar or an edge took it: the content does
    // not get it then.
    bool pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, std::string_view title);

private:
    [[nodiscard]] ui::Rect button(Part part) const;
    void hover(const Hit& hit);

    WindowControls controls_;
    std::function<void()> redraw_;
    std::function<void(ui::CursorShape)> set_cursor_;
    ui::WindowState state_;
    ui::Rect bounds_{};
    ui::Rect bar_{};
    ui::Rect minimize_{};  // empty when the compositor cannot minimize
    ui::Rect maximize_{};
    ui::Rect close_{};
    Hit hovered_;
    Part pressed_{Part::none};  // a button held down
    bool edge_cursor_{false};   // the cursor shows an edge's arrows
};

}  // namespace rummage::views
