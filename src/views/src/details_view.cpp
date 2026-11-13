#include "rummage/views/details_view.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "item_style.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/format.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

using Field = DetailsView::Field;

constexpr float kPadding = 8.0F;       // either side of the table
constexpr float kCellPadding = 10.0F;  // inside each column
constexpr float kRowInset = 2.0F;      // the highlight box, from the row's edges
constexpr float kIconGap = 10.0F;
constexpr float kTitleSize = 12.0F;
constexpr float kSortIcon = 12.0F;
constexpr float kDividerGrab = 4.0F;  // either side of a divider, for the pointer
constexpr float kScrollbarWidth = 4.0F;
constexpr float kScrollbarMinThumb = 24.0F;
constexpr float kItemSlack = 8.0F;  // past the end of a name, still the entry (a press drags it)
// The least a column can be made: the name keeps room for a short file name.
constexpr std::array<float, 4> kMinWidths{160.0F, 64.0F, 96.0F, 80.0F};
constexpr std::array<std::string_view, 4> kTitles{"Name", "Size", "Modified", "Type"};

constexpr std::size_t index(Field field) { return static_cast<std::size_t>(field); }

fs::SortKey sort_key(Field field) {
    switch (field) {
        case Field::size:
            return fs::SortKey::size;
        case Field::modified:
            return fs::SortKey::modified;
        case Field::type:
            return fs::SortKey::type;
        case Field::name:
            break;
    }
    return fs::SortKey::name;
}

bool same_order(const fs::SortOrder& a, const fs::SortOrder& b) {
    return a.key == b.key && a.descending == b.descending &&
           a.directories_first == b.directories_first && a.mime == b.mime;
}

}  // namespace

DetailsView::DetailsView(core::EventLoop& loop, core::WorkerPool& pool,
                         fs::DirectoryWatcher& watcher,
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

void DetailsView::show(std::shared_ptr<Folder> folder) {
    if (folder) { folder->set_entry_filter(entry_filter_); }
    expanded_.clear();
    tree_rows_.clear();
    root_ = folder;
    folder_ = std::move(folder);
    scroller_.jump_to(0.0F);
    hovered_.reset();
    drag_.reset();
    if (folder_) {
        apply_order();
        folder_->request_jump();
    }
    redraw_();
}

void DetailsView::refresh_entry(const std::filesystem::path& path) {
    if (root_ && root_->path() == path.parent_path())
        root_->refresh_entry(path.filename().string());
    if (const auto it = expanded_.find(path.parent_path()); it != expanded_.end())
        it->second->refresh_entry(path.filename().string());
}

void DetailsView::append_rows(const std::shared_ptr<Folder>& folder, std::size_t depth) {
    for (std::size_t i = 0; i < folder->entries().size(); ++i) {
        tree_rows_.push_back({folder, i, depth});
        const auto& entry = folder->entries()[i];
        if (entry.is_directory() && !expanded_.empty()) {
            const auto child = expanded_.find(folder->path_of(entry));
            if (child != expanded_.end())
                append_rows(child->second, depth + 1);
        }
    }
}

void DetailsView::rebuild_rows() {
    tree_rows_.clear();
    if (!root_)
        return;
    append_rows(root_, 0);
    // A deleted, renamed or hidden ancestor releases its watched subtree.
    std::unordered_set<std::filesystem::path> visible;
    if (!expanded_.empty()) {
        for (const auto& row : tree_rows_) {
            const auto& entry = row.folder->entries()[row.entry];
            if (entry.is_directory())
                visible.insert(row.folder->path_of(entry));
        }
    }
    std::erase_if(expanded_, [&](const auto& item) { return !visible.contains(item.first); });
    if (folder_ != root_ && !expanded_.contains(folder_->path())) {
        folder_->set_cursor(std::nullopt);
        folder_ = root_;
    }
}

std::optional<std::size_t> DetailsView::cursor_row() const {
    if (!folder_ || !folder_->cursor())
        return std::nullopt;
    for (std::size_t i = 0; i < tree_rows_.size(); ++i) {
        if (tree_rows_[i].folder == folder_ && tree_rows_[i].entry == *folder_->cursor())
            return i;
    }
    return std::nullopt;
}

void DetailsView::focus_row(std::size_t row, SelectMode mode, bool press) {
    if (row >= tree_rows_.size())
        return;
    const auto target = tree_rows_[row];
    if (folder_ != target.folder) {
        folder_->set_cursor(std::nullopt);
        folder_ = target.folder;
        // Folder selections cannot span different parent directories.
        mode = SelectMode::replace;
    }
    if (press)
        folder_->press(target.entry, mode);
    else
        folder_->set_cursor(target.entry, mode);
}

void DetailsView::move_row(std::ptrdiff_t delta, SelectMode mode) {
    if (tree_rows_.empty())
        return;
    const auto cursor = cursor_row();
    const auto next =
        cursor ? std::clamp(static_cast<std::ptrdiff_t>(*cursor) + delta, std::ptrdiff_t{0},
                            static_cast<std::ptrdiff_t>(tree_rows_.size() - 1))
               : 0;
    focus_row(static_cast<std::size_t>(next), mode);
}

void DetailsView::toggle_row(std::size_t row) {
    if (row >= tree_rows_.size())
        return;
    const auto target = tree_rows_[row];
    const auto& entry = target.folder->entries()[target.entry];
    if (!entry.is_directory() || target.folder->is_trash() || target.depth >= 64)
        return;
    const auto path = target.folder->path_of(entry);
    if (expanded_.contains(path)) {
        focus_row(row, SelectMode::replace);
        std::erase_if(expanded_, [&](const auto& item) {
            const auto relative = item.first.lexically_relative(path);
            return !relative.empty() && *relative.begin() != "..";
        });
    } else {
        auto child =
            std::make_shared<Folder>(loop_, pool_, watcher_, mime_, path, redraw_, show_hidden_);
        child->set_order(order_);
        child->set_entry_filter(entry_filter_);
        expanded_.emplace(path, std::move(child));
    }
    rebuild_rows();
    redraw_();
}

bool DetailsView::disclosure_at(std::size_t row, float x) const {
    const auto& target = tree_rows_[row];
    const float left =
        layout_.left[0] + kCellPadding + static_cast<float>(target.depth) * size_.icon;
    return target.folder->entries()[target.entry].is_directory() && !target.folder->is_trash() &&
           target.depth < 64 && x >= left && x < left + size_.icon &&
           x < layout_.left[0] + layout_.width[0];
}

void DetailsView::open(const std::filesystem::path& directory) {
    show(std::make_shared<Folder>(loop_, pool_, watcher_, mime_, directory, redraw_, show_hidden_));
}

std::optional<ViewHit> DetailsView::hit(float x, float y) const {
    if (!folder_ || x < rows_.x || x >= rows_.right() || y < rows_.y || y >= rows_.bottom()) {
        return std::nullopt;
    }
    const std::optional<std::size_t> row = row_at(x, y);
    bool on_item = false;
    if (row && *row >= item_first_ && *row - item_first_ < item_right_.size()) {
        on_item = x >= layout_.left.at(index(Field::name)) && x < item_right_[*row - item_first_];
    }
    if (row) {
        if (disclosure_at(*row, x))
            return std::nullopt;
        const auto& target = tree_rows_[*row];
        return ViewHit{.folder = target.folder, .entry = target.entry, .on_item = on_item};
    }
    return ViewHit{.folder = root_, .entry = std::nullopt, .on_item = false};
}

std::optional<BandLane> DetailsView::band_lane(const Folder& folder) const {
    if (root_.get() != &folder && !expanded_.contains(folder.path())) {
        return std::nullopt;
    }
    return BandLane{.area = rows_, .offset = scroller_.offset()};
}

std::vector<std::size_t> DetailsView::band_entries(const Folder& folder,
                                                   const ui::Rect& content) const {
    if (root_.get() != &folder && !expanded_.contains(folder.path())) {
        return {};
    }
    std::vector<std::size_t> result;
    for (auto i : rows_touched(content.y, content.bottom(), size_.row, tree_rows_.size())) {
        if (tree_rows_[i].folder.get() == &folder)
            result.push_back(tree_rows_[i].entry);
    }
    return result;
}

void DetailsView::band_scroll(const Folder& folder, float pixels) {
    if (root_.get() == &folder || expanded_.contains(folder.path())) {
        scroller_.jump_to(scroller_.offset() + pixels);
        redraw_();
    }
}

void DetailsView::set_entry_filter(fs::Listing::Predicate filter) {
    entry_filter_ = std::move(filter);
    if (root_) { root_->set_entry_filter(entry_filter_); }
    for (auto& [path, child] : expanded_) { child->set_entry_filter(entry_filter_); }
    rebuild_rows();
    redraw_();
}

void DetailsView::set_show_hidden(bool show) {
    show_hidden_ = show;
    if (root_)
        root_->set_show_hidden(show);
    for (auto& [path, child] : expanded_)
        child->set_show_hidden(show);
    rebuild_rows();
}

bool DetailsView::loading() const {
    if (root_ && (root_->model().loading() || root_->cursor_pending()))
        return true;
    for (const auto& [path, child] : expanded_) {
        if (child->model().loading() || child->cursor_pending())
            return true;
    }
    return false;
}

void DetailsView::sort_by(Field field) {
    const fs::SortKey key = sort_key(field);
    if (order_.key == key) {
        order_.descending = !order_.descending;
    } else {
        order_.key = key;
        order_.descending = false;
    }
    apply_order();
    redraw_();
    if (sorted_) {
        sorted_(order_);
    }
}

void DetailsView::set_sort(fs::SortKey key, bool descending) {
    order_.key = key;
    order_.descending = descending;
    apply_order();
    redraw_();
}

void DetailsView::set_list_size(std::size_t index) {
    list_size_ = std::min(index, kListSizes.size() - 1);
    const ListSize& size = kListSizes.at(list_size_);
    if (size == size_) {
        return;
    }
    // The cursor's row stays where it is on screen.
    const float offset = scroller_.offset();
    const float old_row = size_.row;
    const std::optional<std::size_t> cursor = cursor_row();
    size_ = size;
    if (cursor) {
        // The same height on screen, the whole row still in sight.
        const float screen = std::clamp((static_cast<float>(*cursor) * old_row) - offset, 0.0F,
                                        std::max(0.0F, rows_.height - size_.row));
        scroller_.jump_to(std::max(0.0F, (static_cast<float>(*cursor) * size_.row) - screen));
    } else {
        scroller_.jump_to(offset * size_.row / old_row);
    }
    redraw_();
}

void DetailsView::zoom(std::size_t index) {
    index = std::min(index, kListSizes.size() - 1);
    if (index == list_size_) {
        return;
    }
    set_list_size(index);
    if (zoomed_) {
        zoomed_(index);
    }
}

void DetailsView::set_widths(const std::array<float, 3>& widths) {
    for (std::size_t i = 0; i < widths.size(); ++i) {
        widths_.at(i + 1) = std::max(widths.at(i), kMinWidths.at(i + 1));
    }
    redraw_();
}

void DetailsView::apply_order() {
    if (root_ && !same_order(root_->order(), order_))
        root_->set_order(order_);
    for (auto& [path, child] : expanded_) {
        if (!same_order(child->order(), order_))
            child->set_order(order_);
        child->set_entry_filter(entry_filter_);
    }
    rebuild_rows();
}

float DetailsView::width(Field field) const {
    return field == Field::name ? layout_.width.at(index(Field::name)) : shown_width(index(field));
}

void DetailsView::set_info_visible(bool visible) {
    info_visible_ = visible;
    redraw_();
}

float DetailsView::width_scale() const {
    return size_.detail / kListSizes.at(kUsualListSize).detail;
}

float DetailsView::shown_width(std::size_t column) const {
    return widths_.at(column) * width_scale();
}

void DetailsView::lay_out() {
    // What fits: the fixed columns are left out, last first, while the name would be narrower
    // than its minimum.
    float fixed = 0.0F;
    for (const Field field : {Field::size, Field::modified, Field::type}) {
        layout_.shown.at(index(field)) = true;
        fixed += shown_width(index(field));
    }
    const float table = rows_.width - (2.0F * kPadding);
    for (const Field field : {Field::type, Field::modified, Field::size}) {
        if (table - fixed >= kMinWidths.at(index(Field::name))) {
            break;
        }
        layout_.shown.at(index(field)) = false;
        fixed -= shown_width(index(field));
    }
    // The fixed columns from the right edge; the name takes what is left.
    float right = rows_.right() - kPadding;
    for (const Field field : {Field::type, Field::modified, Field::size}) {
        const std::size_t i = index(field);
        if (!layout_.shown.at(i)) {
            layout_.width.at(i) = 0.0F;
            layout_.left.at(i) = right;
            continue;
        }
        layout_.width.at(i) = shown_width(i);
        layout_.left.at(i) = right - shown_width(i);
        right = layout_.left.at(i);
    }
    layout_.left.at(index(Field::name)) = rows_.x + kPadding;
    layout_.width.at(index(Field::name)) = std::max(1.0F, right - (rows_.x + kPadding));
}

void DetailsView::enter() {
    const fs::Entry* const entry = folder_->single_selection();
    if (entry == nullptr || !entry->is_directory()) {
        return;
    }
    open(folder_->path_of(*entry));
}

void DetailsView::go_up() {
    const std::filesystem::path current = root_->path();
    const std::filesystem::path parent = current.parent_path();
    if (parent == current || current.filename().empty()) {
        return;  // at the root
    }
    open(parent);
    folder_->select_name(current.filename().string());
}

void DetailsView::reveal_cursor(Folder::Reveal reveal, Clock::time_point now) {
    const std::optional<std::size_t> cursor = cursor_row();
    const float viewport = rows_.height;
    if (reveal == Folder::Reveal::none || !cursor) {
        return;
    }
    if (viewport <= 0.0F) {
        folder_->request_jump();  // nowhere to show it yet
        return;
    }
    const float top = static_cast<float>(*cursor) * size_.row;
    const float bottom = top + size_.row;
    const float from = scroller_.destination();
    if (top >= from && bottom <= from + viewport) {
        return;
    }
    if (reveal == Folder::Reveal::jump) {
        scroller_.jump_to(top - ((viewport - size_.row) / 2.0F));  // centered
    } else if (top < from) {
        scroller_.glide_to(top, now);
    } else {
        scroller_.glide_to(bottom - viewport, now);
    }
}

void DetailsView::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                       const theme::Palette& palette, ui::IconSet& icons, Clock::time_point now) {
    if (!folder_) {
        return;
    }
    const float top = bounds.y;
    const float pane_width =
        info_visible_ && info_pane_ != nullptr ? info_pane_->width_in(bounds.width) : 0.0F;
    const bool pane = pane_width > 0.0F;
    const float view_width = bounds.width - pane_width;
    pane_edge_ = pane ? bounds.x + view_width : -1.0F;
    header_ = {bounds.x, top, view_width, kHeaderHeight};
    rows_ = {bounds.x, header_.bottom(), view_width,
             std::max(0.0F, bounds.bottom() - header_.bottom())};
    lay_out();

    // The header: titles, the sort direction, the dividers.
    renderer.fill_rect({header_.x, header_.bottom() - 1.0F, header_.width, 1.0F},
                       palette.outline_variant);
    const fs::SortOrder& order = folder_->order();
    for (const Field field : {Field::name, Field::size, Field::modified, Field::type}) {
        const std::size_t i = index(field);
        if (!layout_.shown.at(i)) {
            continue;
        }
        const float left = layout_.left.at(i);
        const float width = layout_.width.at(i);
        const bool sorted = sort_key(field) == order.key;
        const ui::Color color = sorted ? palette.on_surface : palette.on_surface_variant;
        const std::string_view title = i18n::tr(kTitles.at(i));
        const float title_width = text_width(renderer, title, kTitleSize);
        const float text_y = header_.y + ((kHeaderHeight - kTitleSize) / 2.0F) - 2.0F;
        // Sizes are numbers: right-aligned, title included.
        const float title_x =
            field == Field::size ? left + width - kCellPadding - title_width : left + kCellPadding;
        renderer.draw_text(title, title_x, text_y, kTitleSize, color);
        if (sorted) {
            const float icon_x =
                field == Field::size ? title_x - 4.0F - kSortIcon : title_x + title_width + 4.0F;
            icons.draw(renderer, order.descending ? "chevron-down" : "chevron-up", icon_x,
                       header_.y + ((kHeaderHeight - kSortIcon) / 2.0F), kSortIcon, color);
        }
        if (field != Field::name) {
            const bool active = drag_ && drag_->field == field;
            renderer.fill_rect({left, header_.y + 8.0F, 1.0F, kHeaderHeight - 16.0F},
                               active ? palette.primary : palette.outline_variant);
        }
    }

    // The rows.
    rebuild_rows();
    const auto& entries = tree_rows_;
    const float content = static_cast<float>(entries.size()) * size_.row;
    scroller_.set_extent(content, rows_.height);
    reveal_cursor(folder_->take_reveal(), now);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    const RowRange range = visible_rows(entries.size(), size_.row, offset, rows_.height);

    renderer.push_clip(rows_);
    const std::optional<std::size_t> cursor = cursor_row();
    const bool cursor_ring = folder_->single_selection() == nullptr;
    const float table_left = rows_.x + kPadding;
    const float table_width = rows_.width - (2.0F * kPadding);
    item_first_ = range.first;
    item_right_.clear();
    for (std::size_t i = range.first; i < range.last; ++i) {
        const auto& row = tree_rows_[i];
        const auto& owner = row.folder;
        const fs::Entry& entry = owner->entries()[row.entry];
        const float y = rows_.y + (static_cast<float>(i) * size_.row) - offset;
        const bool selected = owner->is_selected(entry.name);
        const ItemStyle style =
            item_style(palette, entry,
                       {.selected = selected,
                        .cursor = cursor == i,
                        .ring = cursor_ring,
                        .focused = true,
                        .hovered = hovered_ == i,
                        .cut = cut_ != nullptr && cut_->holds(owner->path(), entry),
                        .drop = drop_ != nullptr && drop_->holds(owner->path(), entry)});
        if (style.fill || style.ring) {
            renderer.draw_box(
                {table_left, y + kRowInset, table_width, size_.row - (2.0F * kRowInset)},
                {.fill = style.fill.value_or(kTransparent),
                 .radius = {},
                 .border_width = style.ring ? 1.5F : 0.0F,
                 .border = style.ring.value_or(kTransparent)});
        }
        // The details are quieter than the name, except on the strong selection.
        const ui::Color detail = selected ? style.text : palette.on_surface_variant;
        fs::Entry typed = entry;
        typed.mime = owner->mime_of(entry);

        const float arrow_left =
            layout_.left[0] + kCellPadding + static_cast<float>(row.depth) * size_.icon;
        renderer.push_clip({layout_.left[0], y, layout_.width[0], size_.row});
        if (entry.is_directory() && !owner->is_trash() && row.depth < 64) {
            const auto child = expanded_.find(owner->path_of(entry));
            icons.draw(renderer, child != expanded_.end() ? "chevron-down" : "chevron-right",
                       arrow_left, y + ((size_.row - size_.icon) / 2.0F), size_.icon,
                       child != expanded_.end() && child->second->model().error() ? palette.error
                                                                                  : style.icon);
        }
        const float name_left = arrow_left + size_.icon;
        icons.draw_file(renderer, (*mime_).name(typed.mime), fs::icon_name(*mime_, typed),
                        name_left, y + ((size_.row - size_.icon) / 2.0F), size_.icon, style.icon,
                        typed.folder_icon);
        const float name_x = name_left + size_.icon + kIconGap;
        const float name_room = layout_.left.at(index(Field::name)) +
                                layout_.width.at(index(Field::name)) - kCellPadding - name_x;
        if (edit_ != nullptr && edit_->holds(*owner, entry)) {
            edit_->place(
                {name_x - ui::TextField::kPadding, y + 3.0F,
                 std::max(1.0F, name_room) + (2.0F * ui::TextField::kPadding), size_.row - 6.0F},
                size_.text);
            item_right_.push_back(name_x + name_room);
        } else {
            const float drawn = renderer.draw_text(
                entry.name, name_x, y + ((size_.row - size_.text) / 2.0F) - 2.0F, size_.text,
                style.text, std::max(1.0F, name_room), ui::Ellipsis::middle);
            item_right_.push_back(name_x + drawn + kItemSlack);
        }

        renderer.pop_clip();
        const float detail_y = y + ((size_.row - size_.detail) / 2.0F) - 2.0F;
        const auto cell = [&](Field field, const std::string& text, bool right) {
            if (!layout_.shown.at(index(field))) {
                return;
            }
            const float left = layout_.left.at(index(field)) + kCellPadding;
            const float room =
                std::max(1.0F, layout_.width.at(index(field)) - (2.0F * kCellPadding));
            if (right) {
                const float width = std::min(text_width(renderer, text, size_.detail), room);
                renderer.draw_text(text, left + room - width, detail_y, size_.detail, detail, room);
            } else {
                renderer.draw_text(text, left, detail_y, size_.detail, detail, room);
            }
        };
        const auto child =
            entry.is_directory() ? expanded_.find(owner->path_of(entry)) : expanded_.end();
        std::string size_text = entry.is_directory() ? std::string{"—"} : format_size(entry.size);
        if (child != expanded_.end()) {
            if (child->second->model().loading())
                size_text = i18n::tr("Loading…");
            else if (!child->second->model().error()) {
                const auto count = child->second->entries().size();
                size_text = count == 1 ? std::string{i18n::tr("1 item")} : i18n::trf("{} items", count);
            }
        }
        cell(Field::size, size_text, true);
        cell(Field::modified, format_time(entry.modified_ns), false);
        cell(Field::type,
             child != expanded_.end() && child->second->model().error()
                 ? child->second->model().error().message()
             : typed.mime == fs::kUnknownMime ? std::string{"…"}
                                              : std::string{mime_->name(typed.mime)},
             false);
    }

    const fs::DirectoryModel& model = root_->model();
    if (model.error()) {
        draw_centered(renderer, model.error().message(), rows_, rows_.y + 24.0F, size_.text,
                      palette.error);
    } else if (entries.empty() && !model.loading()) {
        draw_centered(renderer, i18n::tr("Empty folder"), rows_, rows_.y + 24.0F, size_.text,
                      palette.on_surface_variant);
    }

    // Scrollbar: a thin thumb, only when there is something to scroll.
    if (scroller_.max_offset() > 0.0F) {
        const float thumb = std::max(kScrollbarMinThumb, rows_.height * rows_.height / content);
        const float travel = rows_.height - thumb;
        const float thumb_y = rows_.y + (travel * offset / scroller_.max_offset());
        renderer.draw_box({rows_.right() - kScrollbarWidth - 2.0F, thumb_y, kScrollbarWidth, thumb},
                          {.fill = palette.on_surface_variant.with_alpha(0.45F), .radius = {}});
    }
    if (drop_ != nullptr && drop_->covers(root_->path())) {
        renderer.draw_box(
            {rows_.x + 2.0F, rows_.y + 2.0F, rows_.width - 4.0F, rows_.height - 4.0F},
            {.fill = kTransparent, .radius = {}, .border_width = 2.0F, .border = palette.primary});
    }
    renderer.pop_clip();
    if (pane) {
        const float pane_top = header_.y;
        const float pane_height = bounds.bottom() - pane_top;
        info_pane_->draw(renderer, {header_.right(), pane_top, pane_width, pane_height}, palette,
                         icons, *folder_, now);
        const bool grabbed = pane_drag_ || (over_divider_ && on_pane_edge(pointer_x_, pointer_y_));
        const float line = grabbed ? 2.0F : 1.0F / renderer.scale();
        renderer.fill_rect({header_.right() - (grabbed ? 1.0F : 0.0F), pane_top, line, pane_height},
                           grabbed ? palette.primary : palette.outline_variant);
    }

    // Batch contiguous sibling rows for MIME sniffing, as in the flat list.
    for (std::size_t i = range.first; i < range.last;) {
        const auto& first = tree_rows_[i];
        std::size_t last = i + 1;
        while (last < range.last && tree_rows_[last].folder == first.folder &&
               tree_rows_[last].entry == first.entry + last - i)
            ++last;
        first.folder->sniff(first.entry, first.entry + last - i);
        i = last;
    }
    update_hover();  // content may have moved under a still pointer
}

std::optional<std::size_t> DetailsView::row_at(float x, float y) const {
    if (!folder_ || x < rows_.x || x >= rows_.right() || y < rows_.y || y >= rows_.bottom()) {
        return std::nullopt;
    }
    const auto row = static_cast<std::size_t>((y - rows_.y + scroller_.offset()) / size_.row);
    if (row >= tree_rows_.size()) {
        return std::nullopt;
    }
    return row;
}

std::optional<Field> DetailsView::divider_at(float x, float y) const {
    if (y < header_.y || y >= header_.bottom()) {
        return std::nullopt;
    }
    for (const Field field : {Field::size, Field::modified, Field::type}) {
        if (layout_.shown.at(index(field)) &&
            std::abs(x - layout_.left.at(index(field))) <= kDividerGrab) {
            return field;
        }
    }
    return std::nullopt;
}

std::optional<Field> DetailsView::title_at(float x, float y) const {
    if (y < header_.y || y >= header_.bottom()) {
        return std::nullopt;
    }
    for (const Field field : {Field::name, Field::size, Field::modified, Field::type}) {
        const float left = layout_.left.at(index(field));
        if (layout_.shown.at(index(field)) && x >= left &&
            x < left + layout_.width.at(index(field))) {
            return field;
        }
    }
    return std::nullopt;
}

bool DetailsView::on_pane_edge(float x, float y) const {
    return pane_edge_ >= 0.0F && std::abs(x - pane_edge_) <= InfoPane::kEdgeGrab &&
           y >= header_.y && y < rows_.bottom();
}

void DetailsView::update_hover() {
    const bool dragging = drag_ || pane_drag_;
    const std::optional<std::size_t> row =
        pointer_inside_ && !dragging ? row_at(pointer_x_, pointer_y_) : std::nullopt;
    if (row != hovered_) {
        hovered_ = row;
        redraw_();
    }
    const bool over = dragging || (pointer_inside_ && (divider_at(pointer_x_, pointer_y_) ||
                                                       on_pane_edge(pointer_x_, pointer_y_)));
    if (over != over_divider_) {
        over_divider_ = over;
        set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
        redraw_();
    }
}

void DetailsView::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (!folder_) {
        return;
    }
    rebuild_rows();
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion:
            pointer_inside_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            if (pane_drag_ && info_pane_ != nullptr) {
                info_pane_->set_width(pane_drag_->start_width + (pane_drag_->start_x - event.x));
            }
            if (drag_) {
                // The divider follows the pointer: the column on its right grows to the left.
                const std::size_t i = index(drag_->field);
                float others = 0.0F;
                for (const Field field : {Field::size, Field::modified, Field::type}) {
                    others += field == drag_->field ? 0.0F : shown_width(index(field));
                }
                // Dragged in pixels as shown; kept as at the usual size, so a zoom scales it.
                const float most = rows_.width - (2.0F * kPadding) - others - kMinWidths.at(0);
                const float shown = std::clamp(drag_->start_width + (drag_->start_x - event.x),
                                               kMinWidths.at(i) * width_scale(),
                                               std::max(kMinWidths.at(i) * width_scale(), most));
                widths_.at(i) = shown / width_scale();
                redraw_();
            }
            update_hover();
            break;
        case Kind::leave:
            pointer_inside_ = false;
            update_hover();
            break;
        case Kind::scroll:
            if (event.x >= header_.right() && info_pane_ != nullptr &&
                info_pane_->scroll(event, now)) {
                break;
            }
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

            if (event.wheel_y != 0.0F) {
                scroller_.wheel(event.wheel_y, now);
            } else if (event.scroll_y != 0.0F) {
                scroller_.finger(event.scroll_y, now);
            }
            if (event.scroll_finger && event.scroll_stop) {
                scroller_.release(now);
            }
            redraw_();
            break;
        case Kind::press: {
            if (event.button == ui::PointerButton::right) {
                right_click(event.x, event.y);
                break;
            }
            if (event.button != ui::PointerButton::left) {
                break;
            }
            if (info_pane_ != nullptr && on_pane_edge(event.x, event.y)) {
                if (event.click_count == 2) {
                    info_pane_->set_width(InfoPane::kWidth);  // double click: the default
                } else {
                    pane_drag_ = PaneDrag{.start_x = event.x, .start_width = info_pane_->width()};
                }
                update_hover();
                break;
            }
            if (event.x >= header_.right()) {
                if (info_pane_ != nullptr) {
                    static_cast<void>(info_pane_->click(event.x, event.y));
                }
                break;  // the pane, not a row
            }
            if (const std::optional<Field> divider = divider_at(event.x, event.y)) {
                drag_ = Drag{.field = *divider,
                             .start_x = event.x,
                             .start_width = shown_width(index(*divider))};
                redraw_();
                break;
            }
            if (const std::optional<Field> title = title_at(event.x, event.y)) {
                sort_by(*title);
                break;
            }
            if (event.y < rows_.y || event.y >= rows_.bottom()) {
                break;
            }
            const std::optional<std::size_t> row = row_at(event.x, event.y);
            const SelectMode mode = select_mode(event.modifiers, SelectMode::toggle);
            if (row && disclosure_at(*row, event.x)) {
                if (event.click_count == 1)
                    toggle_row(*row);
                break;
            }
            if (event.click_count == 2 && row && mode == SelectMode::replace) {
                focus_row(*row, mode);
                const fs::Entry* const entry = folder_->cursor_entry();
                if (entry != nullptr && !entry->is_directory() && open_) {
                    open_();  // a double click opens a file with its application
                } else {
                    enter();  // ... and a folder in the view
                }
                break;
            }
            if (row || mode == SelectMode::replace) {
                if (row)
                    focus_row(*row, mode, true);
                else {
                    folder_->set_cursor(std::nullopt);
                    folder_ = root_;
                    folder_->press(std::nullopt, mode);
                }
            }
            break;
        }
        case Kind::release:
            if ((drag_ || pane_drag_) && event.button == ui::PointerButton::left) {
                drag_.reset();
                pane_drag_.reset();
                update_hover();
                redraw_();
            }
            break;
    }
}

bool DetailsView::key(const ui::KeyEvent& event, Clock::time_point /*now*/) {
    if (!folder_) {
        return false;
    }
    rebuild_rows();
    Folder& folder = *folder_;
    const auto visible = static_cast<std::ptrdiff_t>(rows_.height / size_.row);
    const std::ptrdiff_t page = std::max<std::ptrdiff_t>(1, visible > 1 ? visible - 1 : visible);
    const std::optional<std::size_t> cursor = folder.cursor();
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
        case XKB_KEY_Up:
            if (event.modifiers.alt) {
                go_up();
            } else {
                move_row(-1, mode);
            }
            break;
        case XKB_KEY_Down:
            if (event.modifiers.alt) {
                enter();
            } else {
                move_row(1, mode);
            }
            break;
        case XKB_KEY_Left: {
            const auto row = cursor_row();
            if (!row)
                break;
            const auto& target = tree_rows_[*row];
            const auto& entry = target.folder->entries()[target.entry];
            if (expanded_.contains(target.folder->path_of(entry)))
                toggle_row(*row);
            else if (target.depth > 0) {
                for (std::size_t i = *row; i > 0; --i) {
                    if (tree_rows_[i - 1].depth < target.depth) {
                        focus_row(i - 1, mode);
                        break;
                    }
                }
            }
            break;
        }
        case XKB_KEY_Right: {
            const auto row = cursor_row();
            if (!row)
                break;
            const auto& target = tree_rows_[*row];
            const auto& entry = target.folder->entries()[target.entry];
            if (!expanded_.contains(target.folder->path_of(entry)))
                toggle_row(*row);
            else if (*row + 1 < tree_rows_.size() && tree_rows_[*row + 1].depth > target.depth)
                focus_row(*row + 1, mode);
            break;
        }
        case XKB_KEY_Page_Down:
            move_row(page, mode);
            break;
        case XKB_KEY_Page_Up:
            move_row(-page, mode);
            break;
        case XKB_KEY_Home:
            focus_row(0, mode);
            break;
        case XKB_KEY_End:
            focus_row(tree_rows_.empty() ? 0 : tree_rows_.size() - 1, mode);
            break;
        case XKB_KEY_space:
            if (!ctrl || !cursor) {
                return false;  // plain Space is for Quick Look
            }
            folder.set_cursor(cursor, SelectMode::toggle);
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
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            enter();
            break;
        case XKB_KEY_BackSpace:
            go_up();
            break;
        default:
            return false;
    }
    redraw_();
    return true;
}

void DetailsView::right_click(float x, float y) {
    if (!folder_) {
        return;
    }
    if (x < header_.right()) {  // the pane keeps the selection it shows
        if (y < rows_.y || y >= rows_.bottom()) {
            return;  // the titles
        }
        const std::optional<std::size_t> row = row_at(x, y);
        if (!row) {
            folder_->set_cursor(std::nullopt);
            folder_ = root_;  // empty space: the root folder's own menu
        } else {
            const auto& target = tree_rows_[*row];
            focus_row(*row, target.folder->is_selected(target.folder->entries()[target.entry].name)
                                ? SelectMode::keep
                                : SelectMode::replace);
        }
    }
    if (menu_) {
        menu_(x, y);
    }
}

std::optional<std::pair<float, float>> DetailsView::cursor_anchor() const {
    const std::optional<std::size_t> cursor = cursor_row();
    if (!cursor) {
        return std::nullopt;
    }
    const float bottom =
        rows_.y + (static_cast<float>(*cursor + 1) * size_.row) - scroller_.offset();
    if (bottom - size_.row < rows_.y || bottom > rows_.bottom()) {
        return std::nullopt;
    }
    return std::pair{rows_.x + 48.0F, bottom};
}

}  // namespace rummage::views
