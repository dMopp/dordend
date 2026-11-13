#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// One step of a path in the breadcrumb: the home or the root as an icon, then a folder's name.
struct Crumb {
    std::filesystem::path path;
    std::string label;      // empty for the first crumb, which is an icon
    std::string_view icon;  // set for the first crumb only

    bool operator==(const Crumb&) const = default;
};

// folder's steps: from the home (an icon) when folder is in it, else from the root.
[[nodiscard]] std::vector<Crumb> crumbs_of(const std::filesystem::path& folder,
                                           const std::filesystem::path& home);

// The bar above the view: the folder's path as clickable steps, cut from the left with "…" when
// it does not fit (the first step and the last ones stay). A click on a step asks to go there,
// with the cursor on the step after it, the folder we came from.
//
// Loop thread only.
class Breadcrumb {
public:
    // go(path, child): the step at path was clicked; child is the name of the step after it
    // (empty for the last).
    using Go = std::function<void(const std::filesystem::path& path, const std::string& child)>;

    Breadcrumb(std::function<void()> redraw, Go go, std::filesystem::path home);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder);

    // The pointer at (x, y) is over the bar as last drawn.
    [[nodiscard]] bool claims(float x, float y) const;

    // A crumb that opens its folder is there (not the last: the folder shown).
    [[nodiscard]] bool clickable_at(float x, float y) const;

    void pointer(const ui::PointerEvent& event);

    // The folder of the step at a window point, as last drawn (nullopt: none, or the "…").
    [[nodiscard]] std::optional<std::filesystem::path> path_at(float x, float y) const;

    // A drag over the window would drop into the step at this path (nullopt: none), drawn
    // marked.
    void set_drop_target(std::optional<std::filesystem::path> path);

    // The steps shown in the last draw: the first one's icon name, then the folders' names, "…"
    // where steps were left out (tests).
    [[nodiscard]] std::vector<std::string> shown() const;

private:
    struct Shown {
        std::optional<std::size_t> crumb;  // nullopt: the "…"
        ui::Rect rect{};
    };

    [[nodiscard]] std::optional<std::size_t> crumb_at(float x, float y) const;

    std::function<void()> redraw_;
    Go go_;
    std::filesystem::path home_;
    std::vector<Crumb> crumbs_;  // as last drawn
    std::vector<Shown> shown_;
    ui::Rect bounds_{};
    std::optional<std::size_t> hovered_;  // a crumb
    std::optional<std::filesystem::path> drop_target_;
};

}  // namespace rummage::views
