#include "rummage/views/column.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "draw_helpers.hpp"
#include "item_style.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/format.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr float kRowInset = 6.0F;  // highlight box inside the column
constexpr float kIconX = 14.0F;
constexpr float kIconGap = 10.0F;
constexpr float kChevronSize = 14.0F;
constexpr float kRightPadding = 12.0F;
constexpr float kScrollbarWidth = 4.0F;
constexpr float kScrollbarMinThumb = 24.0F;
constexpr float kItemSlack = 8.0F;  // past the end of a name, still the entry (a press drags it)

}  // namespace

Column::Column(std::shared_ptr<Folder> folder) : folder_{std::move(folder)} {}

void Column::set_size(const ListSize& size) {
    if (size == size_) {
        return;
    }
    const float offset = scroller_.offset();
    const std::optional<std::size_t> cursor = folder_->cursor();
    const float old_row = size_.row;
    size_ = size;
    if (cursor) {
        // The same height on screen, the whole row still in sight.
        const float screen = std::clamp((static_cast<float>(*cursor) * old_row) - offset, 0.0F,
                                        std::max(0.0F, rows_area_.height - size_.row));
        scroller_.jump_to(std::max(0.0F, (static_cast<float>(*cursor) * size_.row) - screen));
    } else {
        scroller_.jump_to(offset * size_.row / old_row);
    }
}

std::size_t Column::page_rows() const {
    const auto rows = static_cast<std::size_t>(rows_area_.height / size_.row);
    return std::max<std::size_t>(1, rows > 1 ? rows - 1 : rows);
}

void Column::reveal_cursor(Folder::Reveal reveal, Clock::time_point now) {
    const std::optional<std::size_t> cursor = folder_->cursor();
    const float viewport = rows_area_.height;
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

void Column::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                  ui::IconSet& icons, bool focused, Clock::time_point now, EditSlot* edit,
                  const CutMarks* cut, const DropMark* drop) {
    const std::vector<fs::Entry>& entries = folder_->entries();
    rows_area_ = bounds;
    const float content = static_cast<float>(entries.size()) * size_.row;
    scroller_.set_extent(content, bounds.height);
    reveal_cursor(folder_->take_reveal(), now);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    const RowRange range = visible_rows(entries.size(), size_.row, offset, bounds.height);

    renderer.push_clip(bounds);
    const float name_x = bounds.x + kIconX + size_.icon + kIconGap;
    item_first_ = range.first;
    item_right_.clear();
    const std::optional<std::size_t> cursor = folder_->cursor();
    const bool cursor_ring = folder_->single_selection() == nullptr;
    for (std::size_t i = range.first; i < range.last; ++i) {
        const fs::Entry& entry = entries[i];
        const float y = bounds.y + (static_cast<float>(i) * size_.row) - offset;
        const ItemStyle style = item_style(palette, entry,
                                           {.selected = folder_->is_selected(entry.name),
                                            .cursor = cursor == i,
                                            .ring = cursor_ring,
                                            .focused = focused,
                                            .hovered = hovered_ == i,
                                            .cut = cut != nullptr && cut->holds(path(), entry),
                                            .drop = drop != nullptr && drop->holds(path(), entry)});
        if (style.fill || style.ring) {
            renderer.draw_box({bounds.x + kRowInset, y + 1.0F, bounds.width - (2.0F * kRowInset),
                               size_.row - 2.0F},
                              {.fill = style.fill.value_or(kTransparent),
                               .radius = {},
                               .border_width = style.ring ? 1.5F : 0.0F,
                               .border = style.ring.value_or(kTransparent)});
        }
        fs::Entry typed = entry;
        typed.mime = folder_->mime_of(entry);
        icons.draw_file(renderer, (folder_->mime()).name(typed.mime),
                        fs::icon_name(folder_->mime(), typed), bounds.x + kIconX,
                        y + ((size_.row - size_.icon) / 2.0F), size_.icon, style.icon,
                        typed.folder_icon);
        float name_right = bounds.right() - kRightPadding;
        if (entry.is_directory()) {
            name_right -= kChevronSize + 6.0F;
            icons.draw(renderer, "chevron-right", bounds.right() - kRightPadding - kChevronSize,
                       y + ((size_.row - kChevronSize) / 2.0F), kChevronSize, style.chevron);
        }
        if (edit != nullptr && edit->holds(*folder_, entry)) {
            edit->place({name_x - ui::TextField::kPadding, y + 3.0F,
                         std::max(1.0F, name_right - name_x) + (2.0F * ui::TextField::kPadding),
                         size_.row - 6.0F},
                        size_.text);
            item_right_.push_back(name_right);
            continue;
        }
        const float drawn = renderer.draw_text(
            entry.name, name_x, y + ((size_.row - size_.text) / 2.0F) - 2.0F, size_.text,
            style.text, std::max(1.0F, name_right - name_x), ui::Ellipsis::middle);
        item_right_.push_back(name_x + drawn + kItemSlack);
    }

    const fs::DirectoryModel& model = folder_->model();
    if (model.error()) {
        draw_centered(renderer, model.error().message(), bounds, bounds.y + 24.0F, size_.text,
                      palette.error);
    } else if (entries.empty() && !model.loading()) {
        draw_centered(renderer, i18n::tr("Empty folder"), bounds, bounds.y + 24.0F, size_.text,
                      palette.on_surface_variant);
    }

    if (drop != nullptr && drop->covers(path())) {
        renderer.draw_box(
            {bounds.x + 2.0F, bounds.y + 2.0F, bounds.width - 4.0F, bounds.height - 4.0F},
            {.fill = kTransparent, .radius = {}, .border_width = 2.0F, .border = palette.primary});
    }

    // Scrollbar: a thin thumb, only when there is something to scroll.
    if (scroller_.max_offset() > 0.0F) {
        const float thumb = std::max(kScrollbarMinThumb, bounds.height * bounds.height / content);
        const float travel = bounds.height - thumb;
        const float thumb_y = bounds.y + (travel * offset / scroller_.max_offset());
        renderer.draw_box(
            {bounds.right() - kScrollbarWidth - 2.0F, thumb_y, kScrollbarWidth, thumb},
            {.fill = palette.on_surface_variant.with_alpha(0.45F), .radius = {}});
    }
    renderer.pop_clip();

    folder_->sniff(range.first, range.last);
}

std::optional<std::size_t> Column::row_at(float x, float y) const {
    if (x < rows_area_.x || x >= rows_area_.right() || y < rows_area_.y ||
        y >= rows_area_.bottom()) {
        return std::nullopt;
    }
    const float content_y = y - rows_area_.y + scroller_.offset();
    const auto row = static_cast<std::size_t>(content_y / size_.row);
    if (row >= folder_->entries().size()) {
        return std::nullopt;
    }
    return row;
}

bool Column::on_item(std::size_t row, float x) const {
    if (row < item_first_ || row - item_first_ >= item_right_.size()) {
        return false;
    }
    return x >= rows_area_.x + kRowInset && x < item_right_[row - item_first_];
}

std::vector<std::size_t> Column::rows_in(const ui::Rect& content) const {
    return rows_touched(content.y, content.bottom(), size_.row, folder_->entries().size());
}

bool Column::set_hovered(std::optional<std::size_t> row) {
    if (row == hovered_) {
        return false;
    }
    hovered_ = row;
    return true;
}

std::optional<std::pair<float, float>> Column::cursor_anchor() const {
    const std::optional<std::size_t> cursor = folder_->cursor();
    if (!cursor) {
        return std::nullopt;
    }
    const float bottom =
        rows_area_.y + (static_cast<float>(*cursor + 1) * size_.row) - scroller_.offset();
    if (bottom - size_.row < rows_area_.y || bottom > rows_area_.bottom()) {
        return std::nullopt;
    }
    return std::pair{rows_area_.x + 32.0F, bottom};
}

}  // namespace rummage::views
