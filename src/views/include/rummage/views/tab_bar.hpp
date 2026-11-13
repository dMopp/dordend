#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// The tabs above the path, shown while there are two or more (ROADMAP 9.1): one per pane, named
// after its folder, the one shown standing out. A click on a tab shows it; its × or a middle click
// closes it; the + at the end opens a new tab. The tabs share the width, each between kMinWidth
// and kMaxWidth.
//
// Loop thread only.
class TabBar {
public:
    static constexpr float kHeight = 36.0F;
    static constexpr float kMinWidth = 90.0F;
    static constexpr float kMaxWidth = 240.0F;

    struct Handlers {
        std::function<void(std::size_t)> select;
        std::function<void(std::size_t)> close;
        std::function<void()> add;
    };

    TabBar(std::function<void()> redraw, Handlers handlers);

    // titles: one per tab, in order; active: the one shown.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const std::vector<std::string>& titles, std::size_t active);

    // Not drawn this frame (one tab only): claims nothing.
    void not_drawn();

    [[nodiscard]] bool claims(float x, float y) const;
    // A tab, its × or the + is there.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event);

    // The tabs' rectangles as last drawn (tests).
    [[nodiscard]] const std::vector<ui::Rect>& tab_rects() const noexcept { return tabs_; }

    [[nodiscard]] const std::vector<ui::Rect>& close_rects() const noexcept { return closes_; }

    [[nodiscard]] const ui::Rect& add_rect() const noexcept { return add_; }

private:
    enum class Part : std::uint8_t { tab, close, add };

    struct Hit {
        Part part{Part::tab};
        std::size_t index{0};

        bool operator==(const Hit&) const = default;
    };

    [[nodiscard]] std::optional<Hit> hit(float x, float y) const;

    std::function<void()> redraw_;
    Handlers handlers_;
    ui::Rect bounds_{};
    std::vector<ui::Rect> tabs_;    // as last drawn
    std::vector<ui::Rect> closes_;  // each tab's ×
    ui::Rect add_{};
    std::optional<Hit> hovered_;
};

}  // namespace rummage::views
