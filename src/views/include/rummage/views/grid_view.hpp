#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/cut_marks.hpp"
#include "rummage/views/drop_mark.hpp"
#include "rummage/views/edit_slot.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/grid_size.hpp"
#include "rummage/views/info_pane.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// One folder as a grid of cells (a large icon, the name under it), filled row by row and scrolled
// vertically. Only the rows inside the viewport are drawn. The cells stretch to fill the width;
// their size follows the icon size, which Ctrl+plus/minus (or Ctrl+wheel) change and Ctrl+0
// resets. Images and raw files show their thumbnail in the icon's place once it is ready.
//
// Keys: arrows move the cursor (Up/Down by a row), Page Up/Down by a screenful, Home/End to the
// ends; Shift, Ctrl, Ctrl+Space, Ctrl+A and Escape select as in the column view. Enter (or
// Alt+Down, or a double click) opens the folder under the cursor; Backspace and Alt+Up open the
// parent, with the cursor on where we were.
//
// Loop thread only. Must not outlive the loop, pool or watcher.
class GridView {
public:
    using Clock = ui::Scroller::Clock;

    // thumbnails may be nullptr: icons only.
    GridView(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
             std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
             std::function<void(ui::CursorShape)> set_cursor, Thumbnails* thumbnails = nullptr,
             InfoPane* info_pane = nullptr);

    // Shows folder as it is (cursor and selection), the cursor in sight.
    void show(std::shared_ptr<Folder> folder);

    // Shows directory, no cursor.
    void open(const std::filesystem::path& directory);

    // The folder shown; nullptr before show() or open().
    [[nodiscard]] const std::shared_ptr<Folder>& folder() const noexcept { return folder_; }

    // Dotfiles listed or left out (hidden at first), in the folder shown and the ones it opens.
    void set_show_hidden(bool show);
    void set_entry_filter(fs::Listing::Predicate filter);

    // The cells' size (kGridIconSizes[index]), as the window sets it: no zoom handler call.
    void set_size_index(std::size_t index);

    [[nodiscard]] std::size_t size_index() const noexcept { return size_; }

    // Ctrl+plus/minus/0 and Ctrl+wheel changed the size: the window makes it every grid's.
    void set_zoom_handler(std::function<void(std::size_t)> zoomed) { zoomed_ = std::move(zoomed); }

    // The order of every folder the grid shows, now and later (the window's: Browser::set_sort).
    void set_order(const fs::SortOrder& order);

    // Where a drag over the window would drop (the browser's).
    void set_drop_mark(const DropMark* drop) { drop_ = drop; }

    // The folder and the entry at a window point, as last drawn; nullopt off the entries' area.
    [[nodiscard]] std::optional<ViewHit> hit(float x, float y) const;

    // A rubber band over folder's entries: where it runs (nullopt when folder is not shown),
    // the entries a band in content points touches, and scrolling under it by pixels.
    [[nodiscard]] std::optional<BandLane> band_lane(const Folder& folder) const;
    [[nodiscard]] std::vector<std::size_t> band_entries(const Folder& folder,
                                                        const ui::Rect& content) const;
    void band_scroll(const Folder& folder, float pixels);

    // Lays out and draws into bounds at time now (advancing the scroll animation).
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, Clock::time_point now);

    // A scroll animation is running: the window should keep drawing frames.
    [[nodiscard]] bool animating() const noexcept { return scroller_.animating(); }

    void pointer(const ui::PointerEvent& event, Clock::time_point now);
    // false when event is not one of its keys.
    bool key(const ui::KeyEvent& event, Clock::time_point now);

    // Called on a double click on a file (not a folder): the browser opens the selection.
    void set_open_handler(std::function<void()> open) { open_ = std::move(open); }

    // A right click on an entry (selected first, unless it was), on empty space (the selection
    // cleared) or on the info pane: menu(x, y) opens the context menu there.
    void set_menu_handler(std::function<void(float x, float y)> menu) { menu_ = std::move(menu); }

    // Where a context menu opened from the keyboard goes: by the cursor's entry, when it is shown.
    [[nodiscard]] std::optional<std::pair<float, float>> cursor_anchor() const;

    // The name being edited, which the view leaves for the editor to draw (the browser's).
    void set_edit_slot(EditSlot* edit) { edit_ = edit; }

    // The entries cut and not pasted yet, drawn faded (the browser's).
    void set_cut_marks(const CutMarks* cut) { cut_ = cut; }

    // The folder has loaded and its cursor landed (snapshots, tests).
    [[nodiscard]] bool loading() const;

    [[nodiscard]] float icon_size() const { return static_cast<float>(kGridIconSizes.at(size_)); }

    // Cells per row in the last layout (1 before the first draw).
    [[nodiscard]] std::size_t columns() const noexcept { return columns_; }

    [[nodiscard]] ui::Scroller& scroller() noexcept { return scroller_; }

    // Height of a cell at the current icon size.
    [[nodiscard]] float cell_height() const;

    // Shows or hides the info pane (Ctrl+I in the browser).
    void set_info_visible(bool visible);

    [[nodiscard]] bool info_visible() const noexcept { return info_visible_; }

private:
    void right_click(float x, float y);
    void enter();
    void go_up();
    void zoom(std::size_t size);
    void keep_place();
    void layout(float width);
    [[nodiscard]] float drawn_icon_size() const;
    void reveal_cursor(Folder::Reveal reveal, Clock::time_point now);
    [[nodiscard]] std::size_t rows() const;
    [[nodiscard]] float row_top(std::size_t row) const;  // content coordinates
    [[nodiscard]] std::optional<std::size_t> cell_at(float x, float y) const;
    // The point is on cell index's icon or name, as last drawn.
    [[nodiscard]] bool on_item(std::size_t index, float x, float y) const;
    void update_hover();
    [[nodiscard]] bool on_pane_edge(float x, float y) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_;
    std::function<void()> open_;
    std::function<void(float, float)> menu_;
    EditSlot* edit_{nullptr};
    const CutMarks* cut_{nullptr};
    Thumbnails* thumbnails_;
    bool info_visible_{true};
    InfoPane* info_pane_;
    std::function<void(ui::CursorShape)> set_cursor_;

    // Dragging the info pane's edge: where the pointer and the width started.
    struct PaneDrag {
        float start_x;
        float start_width;
    };

    std::optional<PaneDrag> pane_drag_;
    bool over_pane_edge_{false};
    float pane_edge_{-1.0F};  // x of the info pane's edge as last drawn; < 0 without a pane

    std::shared_ptr<Folder> folder_;
    fs::Listing::Predicate entry_filter_;
    bool show_hidden_{false};
    fs::SortOrder order_;
    const DropMark* drop_{nullptr};
    ui::Scroller scroller_;
    std::size_t size_{kUsualGridSize};
    std::function<void(std::size_t)> zoomed_;
    std::size_t columns_{1};
    float cell_width_{0.0F};

    // A zoom keeps this entry where it was on screen (its row's top, viewport coordinates).
    struct Anchor {
        std::size_t index;
        float screen_y;
    };

    std::optional<Anchor> anchor_;
    float zoom_wheel_{0.0F};  // Ctrl+wheel notches not yet turned into a step

    ui::Rect area_{};  // where the cells were last drawn
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
    std::optional<std::size_t> hovered_;
    // The width of each drawn cell's name, from item_first_ on: a press on the icon or the name
    // drags the entry, elsewhere in the cell it starts a band.
    std::vector<float> name_width_;
    std::size_t item_first_{0};
};

}  // namespace rummage::views
