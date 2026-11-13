#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "rummage/fs/places.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/list_size.hpp"

namespace rummage::views {

// The place that holds path: the one with the longest path that path is in (Pictures before
// Home, Home before the file system); nullopt when none does.
[[nodiscard]] std::optional<std::size_t> place_holding(std::span<const fs::Place> places,
                                                       const std::filesystem::path& path);

// The places at the left of the window, in sections (places, bookmarks, devices), the one that
// holds the current folder marked. A click opens a place; so does Enter when the keyboard is in
// the sidebar (the browser moves it there and back), where Up/Down, Home/End move a cursor and
// Escape leaves; a right click asks for a place's menu. Its right edge drags to resize it; a double
// click there restores the width.
//
// Loop thread only.
class Sidebar {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kWidth = 200.0F;  // at start, and after a double click on its edge
    static constexpr float kMinWidth = 140.0F;
    static constexpr float kMaxWidth = 400.0F;
    // The view at its right keeps at least this: the sidebar narrows, then is left out.
    static constexpr float kMinContent = 480.0F;
    static constexpr float kEdgeGrab = 4.0F;  // either side of the edge, where the pointer grabs
    static constexpr float kRowHeight = 30.0F;

    // open: a place was chosen (click or Enter); a disk not mounted has no path yet.
    Sidebar(std::function<void()> redraw, std::function<void(ui::CursorShape)> set_cursor,
            std::function<void(const fs::Place&)> open);

    // The eject icon of an external disk was clicked.
    void set_eject_handler(std::function<void(const fs::Place&)> eject) {
        eject_ = std::move(eject);
    }

    // Where the eject icon of the place at a window point is, as last drawn; nullopt when it
    // has none (tests).
    [[nodiscard]] std::optional<ui::Rect> eject_icon(const std::filesystem::path& path) const;

    void set_places(std::vector<fs::Place> places);

    // A right click on a place: menu(place, x, y).
    using MenuHandler = std::function<void(const fs::Place& place, float x, float y)>;

    void set_menu_handler(MenuHandler menu) { menu_ = std::move(menu); }

    [[nodiscard]] const std::vector<fs::Place>& places() const noexcept { return places_; }

    // current: the folder shown, whose place is marked.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const std::filesystem::path& current, Clock::time_point now);

    // Left out of this frame (no room, or hidden): it takes no pointer events.
    void not_drawn() noexcept { bounds_ = {}; }

    // Its width in a window this wide: narrowed to leave the view kMinContent, 0 when even
    // kMinWidth does not fit.
    [[nodiscard]] float width_in(float window_width) const;

    [[nodiscard]] float width() const noexcept { return width_; }

    // Clamped to [kMinWidth, kMaxWidth].
    void set_width(float width);

    // Independent of the main view; scales rows, labels and icons together.
    void set_list_size(std::size_t index);

    [[nodiscard]] std::size_t list_size() const noexcept { return list_size_; }

    // The pointer at (x, y) is the sidebar's: over it or its edge, or dragging the edge.
    [[nodiscard]] bool claims(float x, float y) const;

    // An eject icon is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event, Clock::time_point now);

    // A key while the keyboard is in the sidebar: false when it is not one of its keys. Enter
    // and Escape hand the keyboard back (focused() turns false).
    bool key(const ui::KeyEvent& event);

    // The keyboard is in the sidebar: its cursor shows, starting on the current place.
    void set_focused(bool focused);

    [[nodiscard]] bool focused() const noexcept { return focused_; }

    [[nodiscard]] std::optional<std::size_t> cursor() const noexcept { return cursor_; }

    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    // The place at a window point, as last drawn.
    [[nodiscard]] const fs::Place* place_under(float x, float y) const;

    // A drag over the window would drop into the place at this path (nullopt: none), drawn
    // marked.
    void set_drop_target(std::optional<std::filesystem::path> path);

    // Where a drop adds bookmarks: the "BOOKMARKS" title, or the free space below the places.
    [[nodiscard]] bool in_bookmark_zone(float x, float y) const;

    // A drag over that zone: a line shows where the bookmarks would go (after the last one).
    void set_bookmark_drop(bool shown);

private:
    struct Row {
        float top{0.0F};                   // content coordinates
        std::optional<std::size_t> place;  // nullopt: the title of section
        fs::Place::Section section{fs::Place::Section::places};
    };

    [[nodiscard]] float scale() const noexcept {
        return kListSizes[list_size_].row / kListSizes[kUsualListSize].row;
    }

    void layout();
    [[nodiscard]] std::optional<std::size_t> place_at(float x, float y) const;
    // The place whose eject icon is at a point (a little wider than the icon).
    [[nodiscard]] std::optional<std::size_t> eject_at(float x, float y) const;
    [[nodiscard]] bool on_edge(float x, float y) const;
    void update_hover();
    void choose(std::size_t place);

    std::function<void()> redraw_;
    std::function<void(ui::CursorShape)> set_cursor_;
    std::function<void(const fs::Place&)> open_;
    std::function<void(const fs::Place&)> eject_;
    // A place's eject icon, by place, as last drawn (empty for the others).
    std::vector<ui::Rect> eject_rects_;
    std::optional<std::size_t> hovered_eject_;
    MenuHandler menu_;
    std::vector<fs::Place> places_;
    std::vector<Row> rows_;
    float content_height_{0.0F};
    std::optional<std::size_t> current_;  // the place holding the folder shown, as last drawn
    std::optional<std::size_t> cursor_;   // while focused
    std::optional<std::size_t> hovered_;
    std::optional<std::filesystem::path> drop_target_;
    bool bookmark_drop_{false};
    bool focused_{false};
    float width_{kWidth};
    std::size_t list_size_{kUsualListSize};
    float zoom_wheel_{0.0F};
    ui::Rect bounds_{};  // as last drawn; empty when not drawn
    ui::Scroller scroller_;

    struct Drag {
        float start_x;
        float start_width;
    };

    std::optional<Drag> drag_;
    bool over_edge_{false};
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
};

}  // namespace rummage::views
