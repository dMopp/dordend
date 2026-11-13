#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/menu_layout.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// One line of a context menu.
struct MenuItem {       // NOLINT(misc-no-recursion): a submenu's items are MenuItems too
    std::string id;     // what tests and set_submenu() find it by ("open_with")
    std::string label;  // "Open With"
    std::string keys;   // the shortcut shown at the right ("Ctrl+C"); may be empty
    bool enabled{true};
    bool separator{false};  // a line above it
    bool submenu{false};    // opens children instead of running action
    bool radio{false};      // one of a choice ("Sort By": Name, Size…): a dot when checked
    bool checked{false};
    std::string empty_text;  // a submenu without children shows it, dimmed ("Loading…")
    std::vector<MenuItem> children;
    std::function<void()> action;  // runs once the menu has closed
};

// Arrange the available items without changing their applicability or callbacks.
[[nodiscard]] std::vector<MenuItem> arrange_menu(const std::vector<MenuItem>& available,
                                                 const std::vector<core::MenuNode>& layout);

// A menu over the window at a point (a right click, the Menu key), drawn by rummage with the
// theme's colors: items, separators, the shortcut of each, nested submenus. Moved to stay
// whole near the window's edges.
//
// While open it takes every key and every click: Up and Down move (over what cannot be chosen),
// Home and End, Enter runs an item or opens its submenu, Right opens a submenu, Left and Escape
// go back out of it, Escape closes. The pointer highlights what it is over (a submenu opens
// when hovered), a click runs an item, a click outside closes the menu (and does nothing else).
//
// Loop thread only.
class ContextMenu {
public:
    explicit ContextMenu(std::function<void()> redraw);

    // At (x, y), top-left; from the keyboard, the first item is highlighted at once.
    void open(float x, float y, std::vector<MenuItem> items, bool keyboard);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return !items_.empty(); }

    [[nodiscard]] const std::vector<MenuItem>& items() const noexcept { return items_; }

    // Fills the submenu of the item called id (applications, templates found on a worker).
    void set_submenu(std::string_view id, std::vector<MenuItem> children);

    // The highlighted item of the menu and of the open submenu (tests).
    [[nodiscard]] std::optional<std::size_t> highlighted(std::size_t level = 0) const noexcept {
        return level < levels_.size() ? levels_[level].highlighted : std::nullopt;
    }

    [[nodiscard]] std::optional<std::size_t> sub_highlighted() const noexcept {
        return highlighted(1);
    }

    [[nodiscard]] bool submenu_open() const noexcept { return levels_.size() > 1; }

    [[nodiscard]] std::size_t open_levels() const noexcept { return levels_.size(); }

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    // Every event while open: true.
    bool pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

private:
    // The next choosable item from `from` in direction step (+1, -1), wrapping.
    [[nodiscard]] static std::optional<std::size_t> step(const std::vector<MenuItem>& items,
                                                         std::optional<std::size_t> from,
                                                         int direction);
    [[nodiscard]] const std::vector<MenuItem>& level_items(std::size_t depth) const;
    void open_submenu(std::size_t depth, std::size_t index, bool keyboard);
    void choose(const MenuItem& item);
    // The panels' sizes for items, in logical pixels.
    [[nodiscard]] static ui::Rect measure(ui::Renderer& renderer,
                                          const std::vector<MenuItem>& items);
    static void draw_panel(ui::Renderer& renderer, const ui::Rect& panel,
                           const std::vector<MenuItem>& items,
                           std::optional<std::size_t> highlighted, std::vector<ui::Rect>& rects,
                           const theme::Palette& palette);

    std::function<void()> redraw_;
    std::vector<MenuItem> items_;
    float x_{0.0F};
    float y_{0.0F};

    struct Level {
        std::size_t parent{0};  // item in the preceding level that opened this one
        std::optional<std::size_t> highlighted;
        ui::Rect panel{};
        std::vector<ui::Rect> rects;
    };

    std::vector<Level> levels_;
    std::size_t active_{0};
};

}  // namespace rummage::views
