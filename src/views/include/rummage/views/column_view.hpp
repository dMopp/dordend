#pragma once

#include <cstddef>
#include <cstdint>
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
#include "rummage/views/column.hpp"
#include "rummage/views/cut_marks.hpp"
#include "rummage/views/drop_mark.hpp"
#include "rummage/views/edit_slot.hpp"
#include "rummage/views/info_pane.hpp"
#include "rummage/views/list_size.hpp"

namespace rummage::views {

// Miller columns, as in Finder: each column is a folder, and the entry under its cursor is shown
// in the column to its right (a folder) or in an info pane (a file). One column has the keyboard
// focus.
//
// Keys: Up/Down, Page Up/Down, Home/End move the cursor in the focused column; Right or Enter
// enter the folder under the cursor; Left goes back one column, and from the first column opens
// the parent folder as a new first column (Backspace and Alt+Up do the same). A click puts the
// cursor on a row. Columns are resized by dragging their right edge.
//
// Selection (in the focused column): a click or an arrow selects one entry; Shift+click and
// Shift+arrows/Page/Home/End select a range from the anchor; Ctrl+click adds or removes one
// entry; Ctrl+arrows move the cursor alone and Ctrl+Space adds or removes the entry under it;
// Ctrl+A selects everything, Escape goes back to the cursor's entry. With more than one entry
// selected no column opens after it: the info pane sums the selection up.
//
// Scrolling: each column scrolls vertically (wheel glide, touchpad with inertia); the row of
// columns scrolls horizontally (touchpad, tilt wheel, Shift+wheel) and glides by itself to keep
// the focused column and the one it opens in view. A touchpad gesture is locked to the axis it
// starts on.
//
// The grid can take over the focused column's folder (focused_folder(), then suspend()) and hand
// a folder back (show()), so the cursor and the selection survive the switch.
//
// Loop thread only. Must not outlive the loop, pool or watcher.
class ColumnView {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kDefaultColumnWidth = 260.0F;
    static constexpr float kMinColumnWidth = 140.0F;
    static constexpr float kMaxColumnWidth = 800.0F;
    static constexpr float kInfoWidth = InfoPane::kWidth;

    ColumnView(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
               std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
               std::function<void(ui::CursorShape)> set_cursor, InfoPane* info_pane = nullptr);

    ~ColumnView();

    ColumnView(const ColumnView&) = delete;
    ColumnView& operator=(const ColumnView&) = delete;
    ColumnView(ColumnView&&) = delete;
    ColumnView& operator=(ColumnView&&) = delete;

    void set_info_visible(bool visible) { info_visible_ = visible; }

    // One column showing directory, focused, no cursor.
    void open(const std::filesystem::path& directory);

    // Columns from root down to target, with the cursor on each step of the way (they unfold
    // as each folder loads). target must be inside root.
    void reveal(const std::filesystem::path& root, const std::filesystem::path& target);

    // Focuses the column showing directory, if one does (true then), and goes back there: the
    // folder under its cursor shows again without a cursor, the columns after it close.
    bool focus_folder(const std::filesystem::path& directory);

    // Dotfiles listed in every column, or left out (hidden at first). Left out, the columns
    // before the focused one keep the hidden entry that leads on, the others move their cursor.
    void set_show_hidden(bool show);
    void set_entry_filter(fs::Listing::Predicate filter);

    // The order of every folder the columns show, now and later (the window's: Browser::set_sort).
    void set_order(const fs::SortOrder& order);

    // The rows' size (ListSize, kListSizes[index]), as the window sets it: no zoom handler call.
    void set_list_size(std::size_t index);

    [[nodiscard]] std::size_t list_size() const noexcept { return list_size_; }

    // Ctrl+plus/minus/0 and Ctrl+wheel changed the size: the window makes it every list's.
    void set_zoom_handler(std::function<void(std::size_t)> zoomed) { zoomed_ = std::move(zoomed); }

    // The folder of the focused column; nullptr before open().
    [[nodiscard]] std::shared_ptr<Folder> focused_folder() const;

    // Another view shows the folders for now: changes to their selections wait for show().
    void suspend() noexcept { suspended_ = true; }

    // Back on screen, with folder in the focused column: the column that holds it, or columns
    // from the deepest one that leads to it (its parent's, when none does), down to folder
    // itself, with its cursor and selection as they are.
    void show(std::shared_ptr<Folder> folder);

    // Lays out and draws into bounds at time now (advancing every animation).
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, Clock::time_point now);

    // An animation is running: the window should keep drawing frames.
    [[nodiscard]] bool animating() const noexcept;

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

    // Where a drag over the window would drop (the browser's).
    void set_drop_mark(const DropMark* drop) { drop_ = drop; }

    // The column and row at a window point, as last drawn; nullopt off the columns (a divider,
    // the info pane, outside).
    [[nodiscard]] std::optional<ViewHit> hit(float x, float y) const;

    // A rubber band over folder's entries: where it runs (nullopt when folder is not shown),
    // the entries a band in content points touches, and scrolling under it by pixels.
    [[nodiscard]] std::optional<BandLane> band_lane(const Folder& folder) const;
    [[nodiscard]] std::vector<std::size_t> band_entries(const Folder& folder,
                                                        const ui::Rect& content) const;
    void band_scroll(const Folder& folder, float pixels);

    [[nodiscard]] std::size_t column_count() const noexcept { return columns_.size(); }

    [[nodiscard]] Column& column(std::size_t index) { return *columns_.at(index).column; }

    [[nodiscard]] const Column& column(std::size_t index) const {
        return *columns_.at(index).column;
    }

    [[nodiscard]] std::size_t focus() const noexcept { return focus_; }

    // reveal() or show() is still unfolding the columns: the focused one is not the last word.
    [[nodiscard]] bool revealing() const noexcept { return !reveal_.empty(); }

    // Every column has loaded (snapshots, tests).
    [[nodiscard]] bool loading() const;

    [[nodiscard]] float scroll_offset() const noexcept { return scroller_.offset(); }

private:
    void zoom(std::size_t index);
    void right_click(float x, float y);

    struct Slot {
        std::unique_ptr<Column> column;
        float width{kDefaultColumnWidth};
        float x{0.0F};  // left edge in content coordinates, set by layout()
    };

    enum class Axis : std::uint8_t { none, vertical, horizontal };

    std::unique_ptr<Column> make_column(const std::filesystem::path& directory);
    std::unique_ptr<Column> make_column(std::shared_ptr<Folder> folder);
    void on_cursor_changed(const Folder* folder);
    void sync_after(std::size_t index);
    void truncate(std::size_t count);
    void go_left();
    void go_right();
    void focus_on(std::size_t index);
    void layout();
    [[nodiscard]] bool shows_info() const;
    [[nodiscard]] float content_width() const;
    [[nodiscard]] std::optional<std::size_t> column_at(float x) const;
    [[nodiscard]] std::optional<std::size_t> divider_at(float x, float y) const;
    void scroll(const ui::PointerEvent& event, Clock::time_point now);
    void update_hover();

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    fs::DirectoryWatcher& watcher_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_;
    std::function<void()> open_;
    std::function<void(float, float)> menu_;
    EditSlot* edit_{nullptr};
    const CutMarks* cut_{nullptr};
    const DropMark* drop_{nullptr};
    std::function<void(ui::CursorShape)> set_cursor_;
    InfoPane* info_pane_;
    bool info_visible_{true};
    ui::Rect info_area_{};  // where the info pane was last drawn; empty when not shown

    std::vector<Slot> columns_;
    std::size_t focus_{0};
    std::filesystem::path reveal_;   // where reveal() is heading; empty when done
    std::shared_ptr<Folder> adopt_;  // the folder show() unfolds to, used as its column
    bool suspended_{false};
    fs::Listing::Predicate entry_filter_;
    bool show_hidden_{false};
    fs::SortOrder order_;
    std::size_t list_size_{kUsualListSize};
    float zoom_wheel_{0.0F};  // Ctrl+wheel notches not yet turned into a step
    std::function<void(std::size_t)> zoomed_;
    ui::Scroller scroller_;     // horizontal
    bool reveal_focus_{false};  // glide to show the focused column at the next draw
    float shift_{0.0F};         // added to the offset at the next draw (a column prepended)

    ui::Rect area_{};  // where the columns were last drawn
    float pointer_x_{0.0F};
    float pointer_y_{0.0F};
    bool pointer_inside_{false};
    std::optional<std::size_t> hovered_column_;
    std::optional<std::size_t> resizing_;  // column whose right edge is being dragged
    float resize_grab_{0.0F};              // pointer x minus the edge when the drag began
    bool over_divider_{false};
    Axis gesture_axis_{Axis::none};  // touchpad gesture lock
    std::optional<std::size_t> gesture_column_;
};

}  // namespace rummage::views
