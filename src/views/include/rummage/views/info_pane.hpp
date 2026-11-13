#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "rummage/core/worker_pool.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// The info pane: a preview of what is selected in a folder and what it is. The same in every view
// (the columns draw it after their last column, the grid and the details at their right), so one
// object serves them all and keeps what it read.
//
// From top to bottom, centered: the preview (a photo's thumbnail, a text file's first lines, else
// the icon), the name and type, then sections: information (size, pixels, dates), the shot (camera,
// lens, exposure, the darktable rating) for photos, and on request ("More details") permissions,
// owner, location and the rest of the exposure. Several entries selected: how many and how big;
// nothing selected: the folder itself. What the directory entry says shows at once, the rest when
// read on a worker.
//
// Loop thread only. Draw it while drawing (thumbnail textures are made then).
class InfoPane {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kWidth = 300.0F;  // at start, and after a double click on its edge
    static constexpr float kMinWidth = 240.0F;
    static constexpr float kMaxWidth = 720.0F;
    // A view keeps at least this for its content: the pane narrows, then is left out.
    static constexpr float kMinContent = 320.0F;
    // Either side of the pane's edge, where the pointer grabs it (grid and details).
    static constexpr float kEdgeGrab = 4.0F;

    // thumbnails may be nullptr: icons only.
    InfoPane(core::WorkerPool& pool, Thumbnails* thumbnails, std::function<void()> redraw);
    ~InfoPane();

    InfoPane(const InfoPane&) = delete;
    InfoPane& operator=(const InfoPane&) = delete;
    InfoPane(InfoPane&&) = delete;
    InfoPane& operator=(InfoPane&&) = delete;

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder, Clock::time_point now);

    // A click at (x, y): true when the pane took it ("More details").
    bool click(float x, float y);

    // A wheel or touchpad scroll: true when it was over the pane, which scrolls when its content
    // is taller than it.
    bool scroll(const ui::PointerEvent& event, Clock::time_point now);

    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    [[nodiscard]] bool expanded() const noexcept { return expanded_; }

    // "More details" is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const { return toggle_.contains(x, y); }

    // Before a frame: what it drew is gone unless it is drawn again (a view without the pane).
    void forget_layout() noexcept { toggle_ = {}; }

    // The width the user gave it (dragging its edge in the grid or the details).
    [[nodiscard]] float width() const noexcept { return width_; }

    // Clamped to [kMinWidth, kMaxWidth].
    void set_width(float width);

    // Its width in a view this wide: narrowed to leave the view kMinContent, 0 when even
    // kMinWidth does not fit.
    [[nodiscard]] float width_in(float view_width) const;

    void set_expanded(bool expanded);

    // What it shows is still being read (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept { return job_.has_value(); }

    // What was read about the entry shown last; nullptr before it arrives (tests).
    [[nodiscard]] const preview::Summary* summary() const noexcept { return summary_.get(); }

private:
    // Reads path's summary unless it is the one shown or on its way.
    void request(const std::filesystem::path& path, std::int64_t modified_ns, std::string mime,
                 preview::Kind kind);

    core::WorkerPool& pool_;
    Thumbnails* thumbnails_;
    std::function<void()> redraw_;

    std::string key_;  // what summary_ is (or will be) about
    std::optional<core::CancelToken> job_;
    std::shared_ptr<const preview::Summary> summary_;
    bool expanded_{false};
    float width_{kWidth};
    ui::Rect bounds_{};  // as last drawn
    ui::Rect toggle_{};  // "More details", as last drawn; empty when not shown
    ui::Scroller scroller_;
    float content_height_{0.0F};  // of the last draw
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
