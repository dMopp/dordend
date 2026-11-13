#pragma once

#include <array>
#include <functional>
#include <string_view>
#include <vector>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// The groups shown in the overlay, in reading order.
[[nodiscard]] std::vector<std::string_view> overlay_groups();

// Every keyboard shortcut (shortcuts()), by group, on a card over the window. F1, "?", the
// status bar's "?" open it; those, Escape, the close button or a click outside close it.
//
// Loop thread only.
class ShortcutsOverlay {
public:
    explicit ShortcutsOverlay(std::function<void()> redraw);

    void open();
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }

    void layout(const ui::Rect& bounds);
    void pointer(const ui::PointerEvent& event);
    void key(const ui::KeyEvent& event);

    [[nodiscard]] float scroll_offset() const noexcept { return offset_; }

    [[nodiscard]] float max_scroll() const noexcept { return max_scroll_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

private:
    std::function<void()> redraw_;
    void scroll_to(float offset);
    [[nodiscard]] ui::Rect thumb() const;
    bool open_{false};
    bool dragging_{false};
    float drag_y_{0.0F};
    float offset_{0.0F};
    float max_scroll_{0.0F};
    float content_{0.0F};
    ui::Rect card_;
    ui::Rect viewport_;
    ui::Rect close_button_;
    ui::Rect track_;

    struct Group {
        std::string_view name;
        bool expanded{false};
        ui::Rect header{};
    };

    std::array<Group, 8> groups_{{{"General"},
                                  {"Moving"},
                                  {"Selecting"},
                                  {"Files"},
                                  {"Tabs"},
                                  {"Sidebar"},
                                  {"Zoom"},
                                  {"Quick Look"}}};
    ui::Rect bounds_;
    std::size_t focused_group_{0};
    bool keyboard_focus_{false};
    void set_expanded(bool expanded);
    void reveal_group();
};

}  // namespace rummage::views
