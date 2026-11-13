#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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
#include "rummage/views/info_pane.hpp"
#include "rummage/views/list_size.hpp"

namespace rummage::views {

// A folder tree as a table: expandable rows with name, size, date and type under a header.
// Only the rows inside the viewport are drawn. A click on a column's title sorts by it, a second
// click reverses the order; dragging the line on a column's left edge makes it wider or narrower
// (the name takes what is left). The order is the view's: every folder it shows is sorted that
// way, and keeps it in the other views.
//
// Keys: Up/Down move the cursor, Page Up/Down by a screenful, Home/End to the ends; Shift, Ctrl,
// Ctrl+Space, Ctrl+A and Escape select within the active parent folder. Right expands a folder
// or moves to its first child; Left collapses it or selects its parent. Enter, Alt+Down and
// double click open a folder; Backspace and Alt+Up open the root folder's parent.
//
// Loop thread only. Must not outlive the loop, pool or watcher.
class DetailsView {
public:
    using Clock = ui::Scroller::Clock;

    enum class Field : std::uint8_t { name, size, modified, type };

    static constexpr float kRowHeight = kListSizes[kUsualListSize].row;  // at the usual size
    static constexpr float kHeaderHeight = 30.0F;

    DetailsView(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                std::function<void(ui::CursorShape)> set_cursor, InfoPane* info_pane = nullptr);

    // Shows folder as it is (cursor and selection), sorted as this view sorts, the cursor in
    // sight.
    void show(std::shared_ptr<Folder> folder);

    // Shows directory, no cursor.
    void open(const std::filesystem::path& directory);

    // The active row's parent folder, used for file operations; nullptr before show() or open().
    [[nodiscard]] const std::shared_ptr<Folder>& folder() const noexcept { return folder_; }

    [[nodiscard]] const std::shared_ptr<Folder>& root_folder() const noexcept { return root_; }

    void refresh_entry(const std::filesystem::path& path);

    // Dotfiles listed or left out (hidden at first), in the folder shown and the ones it opens.
    void set_show_hidden(bool show);
    void set_entry_filter(fs::Listing::Predicate filter);

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

    // Sorts by field: ascending, or reversed when it is the field already sorted by; tells the
    // sort handler (the browser, which sorts the other views the same way).
    void sort_by(Field field);

    void set_sort_handler(std::function<void(const fs::SortOrder&)> handler) {
        sorted_ = std::move(handler);
    }

    [[nodiscard]] const fs::SortOrder& order() const noexcept { return order_; }

    // The order as remembered (views::State): by key, reversed or not.
    void set_sort(fs::SortKey key, bool descending);

    // The rows' size (ListSize, kListSizes[index]), as the window sets it: no zoom handler call.
    void set_list_size(std::size_t index);

    [[nodiscard]] std::size_t list_size() const noexcept { return list_size_; }

    // Ctrl+plus/minus/0 and Ctrl+wheel changed the size: the window makes it every list's.
    void set_zoom_handler(std::function<void(std::size_t)> zoomed) { zoomed_ = std::move(zoomed); }

    // The widths of size, modified and type, as dragged; set_widths keeps each column's minimum.
    [[nodiscard]] std::array<float, 3> widths() const noexcept {
        return {widths_[1], widths_[2], widths_[3]};
    }

    void set_widths(const std::array<float, 3>& widths);

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

    [[nodiscard]] ui::Scroller& scroller() noexcept { return scroller_; }

    // A column's width in logical pixels (the name's as last laid out).
    [[nodiscard]] float width(Field field) const;

    // Shows or hides the info pane (Ctrl+I in the browser).
    void set_info_visible(bool visible);

    [[nodiscard]] bool info_visible() const noexcept { return info_visible_; }

private:
    void zoom(std::size_t index);
    // The fixed columns grow with the text: widths_ are at the usual size, shown times this.
    [[nodiscard]] float width_scale() const;
    [[nodiscard]] float shown_width(std::size_t column) const;
    void right_click(float x, float y);

    // Where each column starts, from the last layout; a narrow table leaves out the type, then
    // the date, then the size, so that the name keeps its minimum width.
    struct Layout {
        std::array<float, 4> left{};
        std::array<float, 4> width{};
        std::array<bool, 4> shown{true, true, true, true};
    };

    struct TreeRow {
        std::shared_ptr<Folder> folder;
        std::size_t entry;
        std::size_t depth;
    };

    void rebuild_rows();
    void append_rows(const std::shared_ptr<Folder>& folder, std::size_t depth);
    void focus_row(std::size_t row, SelectMode mode, bool press = false);
    void toggle_row(std::size_t row);
    void move_row(std::ptrdiff_t delta, SelectMode mode);
    [[nodiscard]] std::optional<std::size_t> cursor_row() const;
    [[nodiscard]] bool disclosure_at(std::size_t row, float x) const;
    std::shared_ptr<Folder> root_;
    std::map<std::filesystem::path, std::shared_ptr<Folder>> expanded_;
    std::vector<TreeRow> tree_rows_;

    void enter();
    void go_up();
    void apply_order();
    void lay_out();
    void reveal_cursor(Folder::Reveal reveal, Clock::time_point now);
    [[nodiscard]] std::optional<std::size_t> row_at(float x, float y) const;
    // The column whose left edge is under x in the header (not the name: it has none).
    [[nodiscard]] std::optional<Field> divider_at(float x, float y) const;
    [[nodiscard]] std::optional<Field> title_at(float x, float y) const;
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
    std::function<void(ui::CursorShape)> set_cursor_;
    bool info_visible_{true};
    InfoPane* info_pane_;

    // Dragging the info pane's edge: where the pointer and the width started.
    struct PaneDrag {
        float start_x;
        float start_width;
    };

    std::optional<PaneDrag> pane_drag_;
    float pane_edge_{-1.0F};  // x of the info pane's edge as last drawn; < 0 without a pane

    std::shared_ptr<Folder> folder_;
    fs::Listing::Predicate entry_filter_;
    bool show_hidden_{false};
    const DropMark* drop_{nullptr};
    fs::SortOrder order_;
    std::function<void(const fs::SortOrder&)> sorted_;
    std::size_t list_size_{kUsualListSize};
    float zoom_wheel_{0.0F};  // Ctrl+wheel notches not yet turned into a step
    std::function<void(std::size_t)> zoomed_;
    ListSize size_{kListSizes[kUsualListSize]};
    ui::Scroller scroller_;
    // Widths of size, modified and type; the name takes the rest.
    std::array<float, 4> widths_{0.0F, 96.0F, 150.0F, 220.0F};
    Layout layout_;

    struct Drag {
        Field field;
        float start_x;
        float start_width;
    };

    std::optional<Drag> drag_;
    bool over_divider_{false};

    ui::Rect header_{};  // where the header and the rows were last drawn
    ui::Rect rows_{};
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
    std::optional<std::size_t> hovered_;
    // Where the icon and name of each drawn row end (window x), from item_first_ on: a press
    // before that drags the entry, after it starts a band.
    std::vector<float> item_right_;
    std::size_t item_first_{0};
};

}  // namespace rummage::views
