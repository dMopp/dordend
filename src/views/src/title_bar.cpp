#include "rummage/views/title_bar.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "draw_helpers.hpp"

namespace rummage::views {
namespace {

constexpr float kButtonSize = 24.0F;
constexpr float kButtonGap = 6.0F;
constexpr float kRightPadding = 8.0F;
constexpr float kIconSize = 14.0F;
constexpr float kTextSize = 13.0F;

bool contains(const ui::Rect& rect, float x, float y) {
    return rect.width > 0.0F && x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

std::string_view icon_of(TitleBar::Part part, bool maximized) {
    switch (part) {
        case TitleBar::Part::minimize:
            return "minus";
        case TitleBar::Part::maximize:
            return maximized ? "squares" : "square";  // restore, maximize
        default:
            return "x";
    }
}

ui::CursorShape cursor_of(ui::Edge edge) {
    switch (edge) {
        case ui::Edge::top:
            return ui::CursorShape::n_resize;
        case ui::Edge::bottom:
            return ui::CursorShape::s_resize;
        case ui::Edge::left:
            return ui::CursorShape::w_resize;
        case ui::Edge::right:
            return ui::CursorShape::e_resize;
        case ui::Edge::top_left:
            return ui::CursorShape::nw_resize;
        case ui::Edge::top_right:
            return ui::CursorShape::ne_resize;
        case ui::Edge::bottom_left:
            return ui::CursorShape::sw_resize;
        case ui::Edge::bottom_right:
            return ui::CursorShape::se_resize;
    }
    return ui::CursorShape::standard;
}

}  // namespace

TitleBar::TitleBar(WindowControls controls, std::function<void()> redraw,
                   std::function<void(ui::CursorShape)> set_cursor)
    : controls_{std::move(controls)},
      redraw_{std::move(redraw)},
      set_cursor_{std::move(set_cursor)} {}

void TitleBar::set_state(const ui::WindowState& state) {
    if (state != state_) {
        state_ = state;
        layout(bounds_);
    }
}

void TitleBar::layout(const ui::Rect& bounds) {
    bounds_ = bounds;
    bar_ = state_.fullscreen ? ui::Rect{bounds.x, bounds.y, bounds.width, 0.0F}
                             : ui::Rect{bounds.x, bounds.y, bounds.width, kHeight};
    minimize_ = maximize_ = close_ = {};
    if (bar_.height <= 0.0F) {
        return;
    }
    // From the right: close, maximize, minimize.
    const float top = bar_.y + ((kHeight - kButtonSize) / 2.0F);
    float x = bar_.right() - kRightPadding - kButtonSize;
    close_ = {x, top, kButtonSize, kButtonSize};
    if (state_.can_maximize) {
        x -= kButtonSize + kButtonGap;
        maximize_ = {x, top, kButtonSize, kButtonSize};
    }
    if (state_.can_minimize) {
        x -= kButtonSize + kButtonGap;
        minimize_ = {x, top, kButtonSize, kButtonSize};
    }
}

ui::Rect TitleBar::content(const ui::Rect& bounds) const {
    const float bar = state_.fullscreen ? 0.0F : kHeight;
    return {bounds.x, bounds.y + bar, bounds.width, std::max(0.0F, bounds.height - bar)};
}

TitleBar::Hit TitleBar::hit(float x, float y) const {
    if (!contains(bounds_, x, y)) {
        return {};
    }
    if (!state_.maximized && !state_.fullscreen) {
        const ui::Rect& b = bounds_;
        const bool left = !state_.tiled_left && x < b.x + kEdge;
        const bool right = !state_.tiled_right && x >= b.right() - kEdge;
        const bool top = !state_.tiled_top && y < b.y + kEdge;
        const bool bottom = !state_.tiled_bottom && y >= b.bottom() - kEdge;
        const bool near_left = !state_.tiled_left && x < b.x + kCorner;
        const bool near_right = !state_.tiled_right && x >= b.right() - kCorner;
        const bool near_top = !state_.tiled_top && y < b.y + kCorner;
        const bool near_bottom = !state_.tiled_bottom && y >= b.bottom() - kCorner;
        const auto edge = [](ui::Edge e) { return Hit{.part = Part::edge, .edge = e}; };
        if ((top && near_left) || (left && near_top)) {
            return edge(ui::Edge::top_left);
        }
        if ((top && near_right) || (right && near_top)) {
            return edge(ui::Edge::top_right);
        }
        if ((bottom && near_left) || (left && near_bottom)) {
            return edge(ui::Edge::bottom_left);
        }
        if ((bottom && near_right) || (right && near_bottom)) {
            return edge(ui::Edge::bottom_right);
        }
        if (top) {
            return edge(ui::Edge::top);
        }
        if (bottom) {
            return edge(ui::Edge::bottom);
        }
        if (left) {
            return edge(ui::Edge::left);
        }
        if (right) {
            return edge(ui::Edge::right);
        }
    }
    if (contains(close_, x, y)) {
        return {.part = Part::close};
    }
    if (contains(maximize_, x, y)) {
        return {.part = Part::maximize};
    }
    if (contains(minimize_, x, y)) {
        return {.part = Part::minimize};
    }
    if (contains(bar_, x, y)) {
        return {.part = Part::title};
    }
    return {};
}

ui::Rect TitleBar::button(Part part) const {
    switch (part) {
        case Part::minimize:
            return minimize_;
        case Part::maximize:
            return maximize_;
        case Part::close:
            return close_;
        default:
            return {};
    }
}

void TitleBar::hover(const Hit& hit) {
    if (hit == hovered_) {
        return;
    }
    const bool buttons_changed = hit.part != hovered_.part;
    hovered_ = hit;
    if (hit.part == Part::edge) {
        set_cursor_(cursor_of(hit.edge));
        edge_cursor_ = true;
    } else if (edge_cursor_) {
        set_cursor_(ui::CursorShape::standard);
        edge_cursor_ = false;
    }
    if (buttons_changed) {
        redraw_();
    }
}

bool TitleBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    if (event.kind == Kind::leave) {
        hover({});
        pressed_ = Part::none;
        return false;  // the content hears it too
    }
    const Hit at = hit(event.x, event.y);
    if (pressed_ != Part::none) {  // a button held: the bar keeps the pointer until release
        hover(at);
        if (event.kind == Kind::release && event.button == ui::PointerButton::left) {
            const Part part = std::exchange(pressed_, Part::none);
            redraw_();
            if (at.part == part) {
                switch (part) {
                    case Part::minimize:
                        controls_.minimize();
                        break;
                    case Part::maximize:
                        controls_.toggle_maximized();
                        break;
                    case Part::close:
                        controls_.close();
                        break;
                    default:
                        break;
                }
            }
        }
        return true;
    }
    hover(at);
    if (at.part == Part::none) {
        return false;
    }
    if (event.kind != Kind::press) {
        return true;
    }
    if (event.button == ui::PointerButton::left) {
        switch (at.part) {
            case Part::edge:
                controls_.resize(at.edge);
                break;
            case Part::title:
                if (event.click_count == 2 && state_.can_maximize) {
                    controls_.toggle_maximized();
                } else {
                    controls_.move();
                }
                break;
            case Part::minimize:
            case Part::maximize:
            case Part::close:
                pressed_ = at.part;
                redraw_();
                break;
            case Part::none:
                break;
        }
    } else if (event.button == ui::PointerButton::right && at.part == Part::title &&
               state_.has_window_menu) {
        controls_.window_menu(event.x, event.y);
    }
    return true;
}

void TitleBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                    ui::IconSet& icons, std::string_view title) {
    layout(bounds);
    if (bar_.height <= 0.0F) {
        return;
    }
    renderer.fill_background(bar_, palette.surface_container_low);
    renderer.fill_rect(
        {bar_.x, bar_.bottom() - (1.0F / renderer.scale()), bar_.width, 1.0F / renderer.scale()},
        palette.outline_variant);

    const ui::Color text = state_.activated ? palette.on_surface : palette.on_surface_variant;
    // Centered on the window, but never under the buttons.
    float buttons_left = close_.x;
    for (const ui::Rect& rect : {maximize_, minimize_}) {
        if (rect.width > 0.0F) {
            buttons_left = std::min(buttons_left, rect.x);
        }
    }
    const float middle = bar_.x + (bar_.width / 2.0F);
    const float room = 2.0F * std::max(0.0F, buttons_left - kButtonGap - middle);
    if (room > kTextSize) {
        draw_centered(renderer, title, {middle - (room / 2.0F), bar_.y, room, kHeight},
                      bar_.y + ((kHeight - kTextSize) / 2.0F) - 2.0F, kTextSize, text);
    }

    for (const Part part : {Part::minimize, Part::maximize, Part::close}) {
        const ui::Rect rect = button(part);
        if (rect.width <= 0.0F) {
            continue;
        }
        const bool hovered = hovered_.part == part;
        const bool pressed = pressed_ == part && hovered;
        ui::Color icon = text;
        if (part == Part::close && hovered) {
            renderer.draw_box(
                rect, {.fill = pressed ? palette.error_container : palette.error, .radius = {}});
            icon = pressed ? palette.on_error_container : palette.on_error;
        } else if (hovered) {
            renderer.draw_box(
                rect, {.fill = pressed ? palette.surface_bright : palette.surface_container_highest,
                       .radius = {}});
        }
        icons.draw(renderer, icon_of(part, state_.maximized),
                   rect.x + ((kButtonSize - kIconSize) / 2.0F),
                   rect.y + ((kButtonSize - kIconSize) / 2.0F), kIconSize, icon);
    }
}

}  // namespace rummage::views
