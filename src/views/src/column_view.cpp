#include "rummage/views/column_view.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "item_style.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

constexpr float kDividerGrab = 4.0F;  // either side of a column edge
constexpr float kScrollbarHeight = 4.0F;

}  // namespace

ColumnView::ColumnView(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                       std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                       std::function<void(ui::CursorShape)> set_cursor, InfoPane* info_pane)
    : loop_{loop},
      pool_{pool},
      watcher_{watcher},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      set_cursor_{std::move(set_cursor)},
      info_pane_{info_pane} {
    order_.mime = mime_.get();
}

ColumnView::~ColumnView() {
    // A folder may outlive its columns (held by a rename on its way, when a tab closes): it must
    // not report to them any more.
    for (const Slot& slot : columns_) {
        slot.column->folder().set_changed_handler(nullptr);
    }
}

std::unique_ptr<Column> ColumnView::make_column(const std::filesystem::path& directory) {
    return make_column(
        std::make_shared<Folder>(loop_, pool_, watcher_, mime_, directory, redraw_, show_hidden_));
}

void ColumnView::set_entry_filter(fs::Listing::Predicate filter) {
    entry_filter_ = std::move(filter);
    // Filtering can remove the selection and synchronously truncate later columns through
    // on_cursor_changed(). Recheck the size instead of retaining invalidated iterators.
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        columns_[i].column->folder().set_entry_filter(entry_filter_);
    }
    redraw_();
}

void ColumnView::set_show_hidden(bool show) {
    show_hidden_ = show;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        columns_[i].column->folder().set_show_hidden(show, i < focus_);
    }
}

void ColumnView::set_order(const fs::SortOrder& order) {
    order_ = order;
    order_.mime = mime_.get();
    for (const Slot& slot : columns_) {
        slot.column->folder().set_order(order_);
    }
}

void ColumnView::set_list_size(std::size_t index) {
    list_size_ = std::min(index, kListSizes.size() - 1);
    for (const Slot& slot : columns_) {
        slot.column->set_size(kListSizes.at(list_size_));
    }
    redraw_();
}

void ColumnView::zoom(std::size_t index) {
    index = std::min(index, kListSizes.size() - 1);
    if (index == list_size_) {
        return;
    }
    set_list_size(index);
    if (zoomed_) {
        zoomed_(index);
    }
}

std::unique_ptr<Column> ColumnView::make_column(std::shared_ptr<Folder> folder) {
    folder->set_entry_filter(entry_filter_);
    folder->set_order(order_);
    // The folder reports its cursor changes with its own address.
    folder->set_changed_handler([this, self = folder.get()] { on_cursor_changed(self); });
    auto column = std::make_unique<Column>(std::move(folder));
    column->set_size(kListSizes.at(list_size_));
    return column;
}

void ColumnView::open(const std::filesystem::path& directory) {
    reveal_.clear();
    adopt_.reset();
    suspended_ = false;
    columns_.clear();
    focus_ = 0;
    resizing_.reset();
    hovered_column_.reset();
    gesture_axis_ = Axis::none;
    scroller_.jump_to(0.0F);
    columns_.push_back({.column = make_column(directory)});
    redraw_();
}

void ColumnView::reveal(const std::filesystem::path& root, const std::filesystem::path& target) {
    open(root);
    const std::filesystem::path relative = target.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") {
        return;  // not inside root
    }
    reveal_ = target;
    columns_.front().column->folder().select_name(relative.begin()->string());
}

bool ColumnView::focus_folder(const std::filesystem::path& directory) {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (columns_[i].column->folder().path() == directory) {
            // The folder its cursor is on opens again from the top, and nothing after it.
            truncate(i + 1);
            sync_after(i);
            focus_on(i);
            return true;
        }
    }
    return false;
}

std::shared_ptr<Folder> ColumnView::focused_folder() const {
    return columns_.empty() ? nullptr : columns_[focus_].column->shared_folder();
}

void ColumnView::show(std::shared_ptr<Folder> folder) {
    suspended_ = false;
    reveal_.clear();
    adopt_.reset();
    // Selections may have changed while suspended: bring the columns up to date first.
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        sync_after(i);
    }
    const auto held = std::ranges::find_if(
        columns_, [&folder](const Slot& slot) { return slot.column->shared_folder() == folder; });
    if (held != columns_.end()) {
        focus_on(static_cast<std::size_t>(held - columns_.begin()));
        redraw_();
        return;
    }
    const std::filesystem::path target = folder->path();
    const auto inside = [&target](const std::filesystem::path& root) {
        const std::filesystem::path relative = target.lexically_relative(root);
        return !relative.empty() && relative != "." && *relative.begin() != "..";
    };
    // The deepest column that leads to target; else target's parent as the only column.
    std::optional<std::size_t> base;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (inside(columns_[i].column->path())) {
            base = i;
        }
    }
    if (!base) {
        const std::filesystem::path parent = target.parent_path();
        if (parent == target || target.filename().empty()) {
            open(target);  // the root: nothing leads to it
            columns_.front() = Slot{.column = make_column(std::move(folder))};
            return;
        }
        open(parent);
        base = 0;
    }
    truncate(*base + 1);
    focus_on(*base);
    reveal_ = target;
    adopt_ = std::move(folder);
    columns_[*base].column->folder().select_name(
        target.lexically_relative(columns_[*base].column->path()).begin()->string());
}

bool ColumnView::loading() const {
    return std::ranges::any_of(columns_, [](const Slot& slot) {
        const Folder& folder = slot.column->folder();
        return folder.model().loading() || folder.cursor_pending();
    });
}

bool ColumnView::animating() const noexcept {
    return scroller_.animating() ||
           std::ranges::any_of(columns_, [](const Slot& slot) { return slot.column->animating(); });
}

void ColumnView::on_cursor_changed(const Folder* folder) {
    if (suspended_) {
        return;  // show() catches up
    }
    const auto it = std::ranges::find_if(
        columns_, [folder](const Slot& slot) { return &slot.column->folder() == folder; });
    if (it == columns_.end()) {
        return;  // still being built, or no longer a column
    }
    sync_after(static_cast<std::size_t>(it - columns_.begin()));
}

// Keeps column index + 1 showing what column index has selected, when that is one entry: the
// same folder (kept as it is), another folder (a new column, and nothing after it), or a file or
// nothing (no column after it: the info pane shows the file, or sums up a larger selection).
// NOLINTNEXTLINE(misc-no-recursion): one level, for the column show() hands over
void ColumnView::sync_after(std::size_t index) {
    const Folder& folder = columns_.at(index).column->folder();
    if (folder.cursor_pending()) {
        return;  // keep what is there until the cursor lands
    }
    const fs::Entry* const entry = folder.single_selection();
    if (!reveal_.empty() && entry != nullptr && folder.path_of(*entry) == reveal_) {
        reveal_.clear();  // arrived
        focus_ = index;
    }
    if (entry == nullptr || !entry->is_directory()) {
        reveal_.clear();  // arrived above, or the way is lost
        truncate(index + 1);
        reveal_focus_ = true;
        redraw_();
        return;
    }
    const std::filesystem::path child = folder.path_of(*entry);
    if (index + 1 < columns_.size() && columns_[index + 1].column->path() == child) {
        return;
    }
    const float width =
        index + 1 < columns_.size() ? columns_[index + 1].width : kDefaultColumnWidth;
    truncate(index + 1);
    if (adopt_ && adopt_->path() == child) {
        // show() arrived: the folder handed over becomes the column, as it is.
        columns_.push_back({.column = make_column(std::move(adopt_)), .width = width});
        adopt_.reset();
        focus_ = index + 1;
        reveal_focus_ = true;
        sync_after(index + 1);
        redraw_();
        return;
    }
    columns_.push_back({.column = make_column(child), .width = width});
    reveal_focus_ = true;
    redraw_();
    if (!reveal_.empty()) {
        const std::filesystem::path rest = reveal_.lexically_relative(child);
        if (!rest.empty() && rest != "." && *rest.begin() != "..") {
            columns_.back().column->folder().select_name(rest.begin()->string());  // may sync
        } else {
            reveal_.clear();
        }
    }
}

void ColumnView::truncate(std::size_t count) {
    if (count >= columns_.size()) {
        return;
    }
    columns_.erase(columns_.begin() + static_cast<std::ptrdiff_t>(count), columns_.end());
    focus_ = std::min(focus_, count - 1);
    if (resizing_ && *resizing_ >= count) {
        resizing_.reset();
    }
    if (hovered_column_ && *hovered_column_ >= count) {
        hovered_column_.reset();
    }
    if (gesture_column_ && *gesture_column_ >= count) {
        gesture_column_.reset();
    }
}

void ColumnView::focus_on(std::size_t index) {
    if (index != focus_) {
        focus_ = index;
        redraw_();
    }
    reveal_focus_ = true;
}

void ColumnView::go_right() {
    const fs::Entry* const entry = columns_[focus_].column->folder().single_selection();
    if (entry == nullptr || !entry->is_directory() || focus_ + 1 >= columns_.size()) {
        return;
    }
    focus_on(focus_ + 1);
    Folder& next = columns_[focus_].column->folder();
    if (!next.cursor() && !next.cursor_pending()) {
        next.set_cursor(0);
    }
}

void ColumnView::go_left() {
    if (focus_ > 0) {
        columns_[focus_].column->folder().set_cursor(std::nullopt);  // closes what it opened
        focus_on(focus_ - 1);
        return;
    }
    // From the first column: its parent becomes the new first column.
    const std::filesystem::path current = columns_.front().column->path();
    const std::filesystem::path parent = current.parent_path();
    if (parent == current || current.filename().empty()) {
        return;  // at the root
    }
    columns_.front().column->folder().set_cursor(std::nullopt);
    columns_.insert(columns_.begin(), Slot{.column = make_column(parent)});
    if (resizing_) {
        ++*resizing_;
    }
    hovered_column_.reset();
    gesture_column_.reset();
    focus_ = 0;
    reveal_focus_ = true;
    // The horizontal offset now counts from the new column: keep the old ones where they were,
    // then glide the new one in.
    shift_ += columns_.front().width;
    columns_.front().column->folder().select_name(current.filename().string());
    redraw_();
}

void ColumnView::layout() {
    float x = 0.0F;
    for (Slot& slot : columns_) {
        slot.x = x;
        x += slot.width;
    }
}

bool ColumnView::shows_info() const {
    if (!info_visible_ || columns_.empty()) {
        return false;
    }
    const Folder& last = columns_.back().column->folder();
    const fs::Entry* const entry = last.single_selection();
    return entry != nullptr ? !entry->is_directory() : last.selected_count() > 1;
}

float ColumnView::content_width() const {
    float width = columns_.empty() ? 0.0F : columns_.back().x + columns_.back().width;
    if (shows_info()) {
        width += kInfoWidth;
    }
    return width;
}

std::optional<std::size_t> ColumnView::column_at(float x) const {
    const float content_x = x - area_.x + scroller_.offset();
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (content_x >= columns_[i].x && content_x < columns_[i].x + columns_[i].width) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<ViewHit> ColumnView::hit(float x, float y) const {
    if (y < area_.y || y >= area_.bottom() || x < area_.x || x >= area_.right() ||
        divider_at(x, y) || (info_area_.width > 0.0F && x >= info_area_.x)) {
        return std::nullopt;
    }
    const std::optional<std::size_t> index = column_at(x);
    if (!index) {
        return std::nullopt;
    }
    const Column& column = *columns_[*index].column;
    const std::optional<std::size_t> row = column.row_at(x, y);
    return ViewHit{
        .folder = column.shared_folder(), .entry = row, .on_item = row && column.on_item(*row, x)};
}

std::optional<BandLane> ColumnView::band_lane(const Folder& folder) const {
    for (const Slot& slot : columns_) {
        if (&slot.column->folder() == &folder) {
            return BandLane{.area = slot.column->rows_area(),
                            .offset = slot.column->scroller().offset()};
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> ColumnView::band_entries(const Folder& folder,
                                                  const ui::Rect& content) const {
    for (const Slot& slot : columns_) {
        if (&slot.column->folder() == &folder) {
            return slot.column->rows_in(content);
        }
    }
    return {};
}

void ColumnView::band_scroll(const Folder& folder, float pixels) {
    for (Slot& slot : columns_) {
        if (&slot.column->folder() == &folder) {
            ui::Scroller& scroller = slot.column->scroller();
            scroller.jump_to(scroller.offset() + pixels);
            redraw_();
            return;
        }
    }
}

std::optional<std::size_t> ColumnView::divider_at(float x, float y) const {
    if (y < area_.y || y >= area_.bottom()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const float edge = area_.x + columns_[i].x + columns_[i].width - scroller_.offset();
        if (std::abs(x - edge) <= kDividerGrab) {
            return i;
        }
    }
    return std::nullopt;
}

void ColumnView::update_hover() {
    std::optional<std::size_t> column;
    std::optional<std::size_t> row;
    if (pointer_inside_ && !resizing_ && pointer_y_ >= area_.y && pointer_y_ < area_.bottom() &&
        !divider_at(pointer_x_, pointer_y_)) {
        column = column_at(pointer_x_);
        if (column) {
            row = columns_[*column].column->row_at(pointer_x_, pointer_y_);
        }
    }
    bool changed = false;
    if (hovered_column_ && hovered_column_ != column && *hovered_column_ < columns_.size()) {
        changed = columns_[*hovered_column_].column->set_hovered(std::nullopt);
    }
    hovered_column_ = column;
    if (column) {
        changed = columns_[*column].column->set_hovered(row) || changed;
    }
    if (changed) {
        redraw_();
    }
}

void ColumnView::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                      ui::IconSet& icons, Clock::time_point now) {
    area_ = bounds;

    layout();
    scroller_.set_extent(content_width(), area_.width);
    if (shift_ != 0.0F) {
        scroller_.jump_to(scroller_.offset() + shift_);
        shift_ = 0.0F;
    }
    if (reveal_focus_ && !columns_.empty()) {
        // Show the focused column and what it opens (the next column, or the info pane).
        const float left = columns_[focus_].x;
        const float right = focus_ + 1 < columns_.size()
                                ? columns_[focus_ + 1].x + columns_[focus_ + 1].width
                                : content_width();
        const float from = scroller_.destination();
        float to = from;
        if (right > to + area_.width) {
            to = right - area_.width;
        }
        to = std::min(to, left);
        if (to != from) {
            scroller_.glide_to(to, now);
        }
        reveal_focus_ = false;
    }
    scroller_.advance(now);
    const float offset = scroller_.offset();

    renderer.push_clip(area_);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const Slot& slot = columns_[i];
        const float left = area_.x + slot.x - offset;
        if (left + slot.width <= area_.x || left >= area_.right()) {
            slot.column->scroller().advance(now);  // off screen: animations still end
            continue;
        }
        slot.column->draw(renderer, {left, area_.y, slot.width, area_.height}, palette, icons,
                          i == focus_, now, edit_, cut_, drop_);
        const bool grabbed =
            resizing_ == i || (over_divider_ && divider_at(pointer_x_, pointer_y_) == i);
        const float line = grabbed ? 2.0F : 1.0F / renderer.scale();
        renderer.fill_rect({left + slot.width - line, area_.y, line, area_.height},
                           grabbed ? palette.primary : palette.outline_variant);
    }
    info_area_ = {};
    if (shows_info() && info_pane_ != nullptr) {
        const float left = area_.x + content_width() - kInfoWidth - offset;
        info_area_ = {left, area_.y, std::max(kInfoWidth, area_.right() - left), area_.height};
        info_pane_->draw(renderer, info_area_, palette, icons, columns_.back().column->folder(),
                         now);
    }
    // Horizontal scrollbar, only when the columns do not fit.
    if (scroller_.max_offset() > 0.0F) {
        const float content = content_width();
        const float thumb = std::max(24.0F, area_.width * area_.width / content);
        const float thumb_x = area_.x + ((area_.width - thumb) * offset / scroller_.max_offset());
        renderer.draw_box(
            {thumb_x, area_.bottom() - kScrollbarHeight - 2.0F, thumb, kScrollbarHeight},
            {.fill = palette.on_surface_variant.with_alpha(0.45F), .radius = {}});
    }
    renderer.pop_clip();

    update_hover();  // content may have moved under a still pointer
}

void ColumnView::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion: {
            pointer_inside_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            if (resizing_) {
                Slot& slot = columns_[*resizing_];
                const float left = area_.x + slot.x - scroller_.offset();
                slot.width =
                    std::clamp(event.x - resize_grab_ - left, kMinColumnWidth, kMaxColumnWidth);
                redraw_();
                break;
            }
            const bool over = divider_at(event.x, event.y).has_value();
            if (over != over_divider_) {
                over_divider_ = over;
                set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
                redraw_();
            }
            update_hover();
            break;
        }
        case Kind::leave:
            pointer_inside_ = false;
            if (over_divider_) {
                over_divider_ = false;
                set_cursor_(ui::CursorShape::standard);
                redraw_();
            }
            update_hover();
            break;
        case Kind::scroll:
            if (event.modifiers.ctrl && event.wheel_y != 0.0F) {
                // Ctrl+wheel: up makes the rows bigger, one size per notch.
                zoom_wheel_ += event.wheel_y;
                while (zoom_wheel_ <= -1.0F) {
                    zoom_wheel_ += 1.0F;
                    zoom(list_size_ + 1);
                }
                while (zoom_wheel_ >= 1.0F) {
                    zoom_wheel_ -= 1.0F;
                    zoom(list_size_ > 0 ? list_size_ - 1 : 0);
                }
                break;
            }
            if (info_pane_ == nullptr || info_area_.width <= 0.0F ||
                !info_pane_->scroll(event, now)) {
                scroll(event, now);
            }
            break;
        case Kind::press:
            if (event.button == ui::PointerButton::right) {
                if (event.y >= area_.y && event.y < area_.bottom()) {
                    right_click(event.x, event.y);
                }
                break;
            }
            if (event.button != ui::PointerButton::left) {
                break;
            }
            if (const std::optional<std::size_t> divider = divider_at(event.x, event.y)) {
                Slot& slot = columns_[*divider];
                if (event.click_count == 2) {
                    slot.width = kDefaultColumnWidth;  // double click: back to the default
                    redraw_();
                    break;
                }
                resizing_ = divider;
                resize_grab_ = event.x - (area_.x + slot.x + slot.width - scroller_.offset());
                update_hover();
                break;
            }
            if (event.y < area_.y || event.y >= area_.bottom()) {
                break;
            }
            if (info_pane_ != nullptr && info_pane_->click(event.x, event.y)) {
                break;
            }
            if (const std::optional<std::size_t> index = column_at(event.x)) {
                reveal_.clear();
                Column& column = *columns_[*index].column;
                focus_on(*index);
                const std::optional<std::size_t> row = column.row_at(event.x, event.y);
                const SelectMode mode = select_mode(event.modifiers, SelectMode::toggle);
                if (row || mode == SelectMode::replace) {
                    column.folder().press(row, mode);  // a plain click on nothing clears
                }
                // A double click opens a file with its application; a folder is open already.
                const fs::Entry* const entry = column.folder().cursor_entry();
                if (event.click_count == 2 && row && mode == SelectMode::replace &&
                    entry != nullptr && !entry->is_directory() && open_) {
                    open_();
                }
                redraw_();
            }
            break;
        case Kind::release:
            if (event.button == ui::PointerButton::left && resizing_) {
                resizing_.reset();
                reveal_focus_ = true;
                const bool over = divider_at(event.x, event.y).has_value();
                if (over != over_divider_) {
                    over_divider_ = over;
                    set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
                }
                redraw_();
            }
            break;
    }
}

void ColumnView::scroll(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.scroll_finger) {
        // Touchpad: the gesture follows the axis it starts on, and the column it starts over.
        if (gesture_axis_ == Axis::none && (event.scroll_x != 0.0F || event.scroll_y != 0.0F)) {
            gesture_axis_ = std::abs(event.scroll_x) > std::abs(event.scroll_y) ? Axis::horizontal
                                                                                : Axis::vertical;
            gesture_column_ = column_at(event.x);
        }
        if (gesture_axis_ == Axis::horizontal) {
            if (event.scroll_x != 0.0F) {
                scroller_.finger(event.scroll_x, now);
            }
            if (event.scroll_stop) {
                scroller_.release(now);
            }
        } else if (gesture_axis_ == Axis::vertical && gesture_column_) {
            ui::Scroller& scroller = columns_[*gesture_column_].column->scroller();
            if (event.scroll_y != 0.0F) {
                scroller.finger(event.scroll_y, now);
            }
            if (event.scroll_stop) {
                scroller.release(now);
            }
        }
        if (event.scroll_stop) {
            gesture_axis_ = Axis::none;
            gesture_column_.reset();
        }
        redraw_();
        return;
    }
    float horizontal_wheel = event.wheel_x;
    if (horizontal_wheel == 0.0F && event.modifiers.shift) {
        horizontal_wheel = event.wheel_y;  // Shift+wheel scrolls sideways
    }
    if (horizontal_wheel != 0.0F) {
        scroller_.wheel(horizontal_wheel, now);
    } else if (const std::optional<std::size_t> index = column_at(event.x)) {
        ui::Scroller& scroller = columns_[*index].column->scroller();
        if (event.wheel_y != 0.0F) {
            scroller.wheel(event.wheel_y, now);
        } else if (event.scroll_y != 0.0F) {
            scroller.finger(event.scroll_y, now);  // other continuous sources: 1:1, no inertia
        }
    }
    redraw_();
}

bool ColumnView::key(const ui::KeyEvent& event, Clock::time_point /*now*/) {
    if (columns_.empty()) {
        return false;
    }
    Folder& folder = columns_[focus_].column->folder();
    const auto page = static_cast<std::ptrdiff_t>(columns_[focus_].column->page_rows());
    // Shift extends the selection, Ctrl moves the cursor alone.
    const SelectMode mode = select_mode(event.modifiers, SelectMode::keep);
    const bool ctrl = event.modifiers.ctrl;
    switch (event.keysym) {
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add:
            if (!ctrl) {
                return false;
            }
            zoom(list_size_ + 1);
            break;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract:
            if (!ctrl) {
                return false;
            }
            zoom(list_size_ > 0 ? list_size_ - 1 : 0);
            break;
        case XKB_KEY_0:
        case XKB_KEY_KP_0:
            if (!ctrl) {
                return false;
            }
            zoom(kUsualListSize);
            break;
        case XKB_KEY_Down:
            folder.move_cursor(1, mode);
            break;
        case XKB_KEY_Up:
            if (event.modifiers.alt) {
                go_left();
            } else {
                folder.move_cursor(-1, mode);
            }
            break;
        case XKB_KEY_Page_Down:
            folder.move_cursor(page, mode);
            break;
        case XKB_KEY_Page_Up:
            folder.move_cursor(-page, mode);
            break;
        case XKB_KEY_Home:
            folder.set_cursor(0, mode);
            break;
        case XKB_KEY_End:
            folder.set_cursor(std::numeric_limits<std::size_t>::max(), mode);
            break;
        case XKB_KEY_space:
            if (!ctrl || !folder.cursor()) {
                return false;  // plain Space is for Quick Look (ROADMAP 5.3)
            }
            folder.set_cursor(folder.cursor(), SelectMode::toggle);
            break;
        case XKB_KEY_a:
        case XKB_KEY_A:
            if (!ctrl) {
                return false;
            }
            folder.select_all();
            break;
        case XKB_KEY_Escape:
            folder.collapse_selection();
            break;
        case XKB_KEY_Right:
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            go_right();
            break;
        case XKB_KEY_Left:
        case XKB_KEY_BackSpace:
            go_left();
            break;
        default:
            return false;
    }
    reveal_.clear();
    reveal_focus_ = true;
    redraw_();
    return true;
}

void ColumnView::right_click(float x, float y) {
    const bool in_pane = info_area_.width > 0.0F && x >= info_area_.x && x < info_area_.right() &&
                         y >= info_area_.y && y < info_area_.bottom();
    if (!in_pane) {
        const std::optional<std::size_t> index = column_at(x);
        if (!index) {
            return;
        }
        reveal_.clear();
        focus_on(*index);
        Folder& folder = columns_[*index].column->folder();
        const std::optional<std::size_t> row = columns_[*index].column->row_at(x, y);
        if (!row) {
            folder.set_cursor(std::nullopt);  // empty space: the folder's own menu
        } else {
            folder.set_cursor(row, folder.is_selected(folder.entries()[*row].name)
                                       ? SelectMode::keep
                                       : SelectMode::replace);
        }
        redraw_();
    }
    if (menu_) {
        menu_(x, y);
    }
}

std::optional<std::pair<float, float>> ColumnView::cursor_anchor() const {
    if (columns_.empty()) {
        return std::nullopt;
    }
    return columns_[focus_].column->cursor_anchor();
}

}  // namespace rummage::views
