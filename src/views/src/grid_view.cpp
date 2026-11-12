#include "nordend/views/grid_view.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "item_style.hpp"
#include "nordend/engines/engines.hpp"
#include "nordend/fs/icon.hpp"
#include "nordend/ui/text_field.hpp"
#include "nordend/views/format.hpp"
#include "nordend/core/i18n.hpp"

namespace nordend::views {
namespace {

constexpr float kPadding = 16.0F;          // around the grid
constexpr float kCellInset = 4.0F;         // highlight box inside the cell
constexpr float kIconTop = 12.0F;          // from the cell's top to the icon
constexpr float kNameGap = 8.0F;           // between the icon and the name
constexpr float kNameMargin = 8.0F;        // either side of the name
constexpr float kCellExtraWidth = 56.0F;   // a cell is the icon plus this, at least
constexpr float kCellMinWidth = 132.0F;    // and room for a short name (P1234567.ORF)
constexpr float kCellExtraHeight = 52.0F;  // icon plus the gaps and one line of name
constexpr float kTextSize = 13.0F;
constexpr float kThumbnailRadius = 4.0F;
constexpr float kScrollbarWidth = 4.0F;
constexpr float kScrollbarMinThumb = 24.0F;

}  // namespace

GridView::GridView(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
                   std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                   std::function<void(ui::CursorShape)> set_cursor, Thumbnails* thumbnails,
                   InfoPane* info_pane)
    : loop_{loop},
      pool_{pool},
      watcher_{watcher},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      thumbnails_{thumbnails},
      info_pane_{info_pane},
      set_cursor_{std::move(set_cursor)} {
    order_.mime = mime_.get();
}

void GridView::show(std::shared_ptr<Folder> folder) {
    if (folder) { folder->set_entry_filter(entry_filter_); }
    folder_ = std::move(folder);
    if (folder_) {
        folder_->set_order(order_);
    }
    scroller_.jump_to(0.0F);
    anchor_.reset();
    hovered_.reset();
    if (folder_) {
        folder_->request_jump();
    }
    redraw_();
}

void GridView::open(const std::filesystem::path& directory) {
    show(std::make_shared<Folder>(loop_, pool_, watcher_, mime_, directory, redraw_, show_hidden_));
}

std::optional<ViewHit> GridView::hit(float x, float y) const {
    if (!folder_ || x < area_.x || x >= area_.right() || y < area_.y || y >= area_.bottom()) {
        return std::nullopt;
    }
    const std::optional<std::size_t> cell = cell_at(x, y);
    return ViewHit{.folder = folder_, .entry = cell, .on_item = cell && on_item(*cell, x, y)};
}

bool GridView::on_item(std::size_t index, float x, float y) const {
    if (index < item_first_ || index - item_first_ >= name_width_.size() || columns_ == 0) {
        return false;
    }
    const float left = area_.x + kPadding + (static_cast<float>(index % columns_) * cell_width_);
    const float top = area_.y + row_top(index / columns_) - scroller_.offset();
    const float icon = drawn_icon_size();
    const ui::Rect picture{left + ((cell_width_ - icon) / 2.0F), top + kIconTop, icon, icon};
    const float width = name_width_[index - item_first_];
    const ui::Rect name{left + ((cell_width_ - width) / 2.0F) - kNameGap,
                        top + kIconTop + icon + (kNameGap / 2.0F), width + (2.0F * kNameGap),
                        kTextSize + kNameGap};
    return picture.contains(x, y) || name.contains(x, y);
}

std::optional<BandLane> GridView::band_lane(const Folder& folder) const {
    if (folder_.get() != &folder) {
        return std::nullopt;
    }
    return BandLane{.area = area_, .offset = scroller_.offset()};
}

std::vector<std::size_t> GridView::band_entries(const Folder& folder,
                                                const ui::Rect& content) const {
    std::vector<std::size_t> touched;
    if (folder_.get() != &folder || columns_ == 0 || cell_width_ <= 0.0F) {
        return touched;
    }
    // The cells whose highlight box the band meets.
    const float height = cell_height();
    const std::size_t count = folder.entries().size();
    const RowRange band_rows = visible_rows(rows(), height, std::max(0.0F, content.y - kPadding),
                                            std::max(content.height, 0.01F));
    for (std::size_t row = band_rows.first; row < band_rows.last; ++row) {
        const float top = row_top(row) + kCellInset;
        const float bottom = row_top(row) + height - kCellInset;
        if (content.bottom() < top || content.y > bottom) {
            continue;
        }
        for (std::size_t column = 0; column < columns_; ++column) {
            const std::size_t index = (row * columns_) + column;
            if (index >= count) {
                break;
            }
            const float left = kPadding + (static_cast<float>(column) * cell_width_) + kCellInset;
            const float right = left + cell_width_ - (2.0F * kCellInset);
            if (content.right() >= left && content.x <= right) {
                touched.push_back(index);
            }
        }
    }
    return touched;
}

void GridView::band_scroll(const Folder& folder, float pixels) {
    if (folder_.get() == &folder) {
        scroller_.jump_to(scroller_.offset() + pixels);
        redraw_();
    }
}

void GridView::set_order(const fs::SortOrder& order) {
    order_ = order;
    order_.mime = mime_.get();
    if (folder_) {
        folder_->set_order(order_);
    }
}

void GridView::set_entry_filter(fs::Listing::Predicate filter) {
    entry_filter_ = std::move(filter);
    if (folder_) {
        folder_->set_entry_filter(entry_filter_);
    }
    redraw_();
}

void GridView::set_show_hidden(bool show) {
    show_hidden_ = show;
    if (folder_) {
        folder_->set_show_hidden(show);
    }
}

bool GridView::loading() const {
    return folder_ && (folder_->model().loading() || folder_->cursor_pending());
}

float GridView::drawn_icon_size() const {
    return cell_width_ > 0.0F ? std::min(icon_size(), std::max(1.0F, cell_width_ - kCellExtraWidth))
                              : icon_size();
}

float GridView::cell_height() const { return drawn_icon_size() + kCellExtraHeight; }

std::size_t GridView::rows() const {
    const std::size_t count = folder_ ? folder_->entries().size() : 0;
    return (count + columns_ - 1) / columns_;
}

float GridView::row_top(std::size_t row) const {
    return kPadding + (static_cast<float>(row) * cell_height());
}

void GridView::layout(float width) {
    const float available = std::max(1.0F, width - (2.0F * kPadding));
    const float min_width = std::max(icon_size() + kCellExtraWidth, kCellMinWidth);
    columns_ = std::max<std::size_t>(1, static_cast<std::size_t>(available / min_width));
    cell_width_ = available / static_cast<float>(columns_);
}

void GridView::enter() {
    const fs::Entry* const entry = folder_->single_selection();
    if (entry == nullptr || !entry->is_directory()) {
        return;
    }
    open(folder_->path_of(*entry));
}

void GridView::go_up() {
    const std::filesystem::path current = folder_->path();
    const std::filesystem::path parent = current.parent_path();
    if (parent == current || current.filename().empty()) {
        return;  // at the root
    }
    open(parent);
    folder_->select_name(current.filename().string());
}

void GridView::set_size_index(std::size_t index) {
    index = std::min(index, kGridIconSizes.size() - 1);
    if (index == size_) {
        return;
    }
    keep_place();
    size_ = index;
    redraw_();
}

void GridView::zoom(std::size_t size) {
    size = std::min(size, kGridIconSizes.size() - 1);
    if (size == size_) {
        return;
    }
    set_size_index(size);
    if (zoomed_) {
        zoomed_(size_);
    }
}

void GridView::set_info_visible(bool visible) {
    if (visible == info_visible_) {
        return;
    }
    keep_place();  // the cells per row change
    info_visible_ = visible;
    redraw_();
}

void GridView::keep_place() {
    // Keep an entry where it is on screen through the next layout: the cursor when it is in
    // sight, else the first visible one.
    if (folder_ && !folder_->entries().empty() && area_.height > 0.0F) {
        const float offset = scroller_.offset();
        const auto first_row =
            static_cast<std::size_t>(std::max(0.0F, offset - kPadding) / cell_height());
        Anchor anchor{.index = std::min(first_row * columns_, folder_->entries().size() - 1),
                      .screen_y = 0.0F};
        if (const std::optional<std::size_t> cursor = folder_->cursor()) {
            const float top = row_top(*cursor / columns_) - offset;
            if (top + cell_height() > 0.0F && top < area_.height) {
                anchor.index = *cursor;
            }
        }
        anchor.screen_y = row_top(anchor.index / columns_) - offset;
        anchor_ = anchor;
    }
}

void GridView::reveal_cursor(Folder::Reveal reveal, Clock::time_point now) {
    const std::optional<std::size_t> cursor = folder_->cursor();
    const float viewport = area_.height;
    if (reveal == Folder::Reveal::none || !cursor) {
        return;
    }
    if (viewport <= 0.0F) {
        folder_->request_jump();  // nowhere to show it yet
        return;
    }
    const float top = row_top(*cursor / columns_) - kPadding;
    const float bottom = top + cell_height() + (2.0F * kPadding);
    const float from = scroller_.destination();
    if (top >= from && bottom <= from + viewport) {
        return;
    }
    if (reveal == Folder::Reveal::jump) {
        scroller_.jump_to(top - ((viewport - cell_height()) / 2.0F) + kPadding);  // centered
    } else if (top < from) {
        scroller_.glide_to(top, now);
    } else {
        scroller_.glide_to(bottom - viewport, now);
    }
}

void GridView::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                    ui::IconSet& icons, Clock::time_point now) {
    if (!folder_) {
        return;
    }
    const float pane_width =
        info_visible_ && info_pane_ != nullptr ? info_pane_->width_in(bounds.width) : 0.0F;
    const bool pane = pane_width > 0.0F;
    area_ = {bounds.x, bounds.y, bounds.width - pane_width, bounds.height};
    pane_edge_ = pane ? area_.right() : -1.0F;
    layout(area_.width);
    const std::vector<fs::Entry>& entries = folder_->entries();
    const float height = cell_height();
    const float content = (static_cast<float>(rows()) * height) + (2.0F * kPadding);
    scroller_.set_extent(content, area_.height);
    if (anchor_) {
        scroller_.jump_to(row_top(anchor_->index / columns_) - anchor_->screen_y);
        anchor_.reset();
    }
    reveal_cursor(folder_->take_reveal(), now);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    const RowRange range = visible_rows(rows(), height, offset - kPadding, area_.height);

    renderer.push_clip(area_);
    const float icon = drawn_icon_size();
    const std::optional<std::size_t> cursor = folder_->cursor();
    const bool cursor_ring = folder_->single_selection() == nullptr;
    ui::FontSystem& fonts = renderer.fonts();
    item_first_ = range.first * columns_;
    name_width_.clear();
    // The cache size that covers the icon in physical pixels.
    const thumbs::Size thumbnail_size =
        thumbs::size_for(static_cast<std::uint32_t>(std::lround(icon * renderer.scale())));
    const std::shared_ptr<const engines::Registry> engines = engines::installed();
    for (std::size_t row = range.first; row < range.last; ++row) {
        const float y = area_.y + row_top(row) - offset;
        for (std::size_t column = 0; column < columns_; ++column) {
            const std::size_t i = (row * columns_) + column;
            if (i >= entries.size()) {
                break;
            }
            const fs::Entry& entry = entries[i];
            const float x = area_.x + kPadding + (static_cast<float>(column) * cell_width_);
            const bool cut = cut_ != nullptr && cut_->holds(folder_->path(), entry);
            const ItemStyle style =
                item_style(palette, entry,
                           {.selected = folder_->is_selected(entry.name),
                            .cursor = cursor == i,
                            .ring = cursor_ring,
                            .focused = true,
                            .hovered = hovered_ == i,
                            .cut = cut,
                            .drop = drop_ != nullptr && drop_->holds(folder_->path(), entry)});
            if (style.fill || style.ring) {
                renderer.draw_box({x + kCellInset, y + kCellInset,
                                   cell_width_ - (2.0F * kCellInset), height - (2.0F * kCellInset)},
                                  {.fill = style.fill.value_or(kTransparent),
                                   .radius = ui::Corners::all(10.0F),
                                   .border_width = style.ring ? 1.5F : 0.0F,
                                   .border = style.ring.value_or(kTransparent)});
            }
            fs::Entry typed = entry;
            typed.mime = folder_->mime_of(entry);
            const ui::Texture* thumbnail = nullptr;
            const engines::Family* family =
                entry.is_directory() ? nullptr : engines->find(*mime_, typed.mime);
            if (thumbnails_ != nullptr && family != nullptr && family->thumbnails) {
                thumbnail =
                    thumbnails_->get(folder_->path_of(entry), entry.modified_ns, thumbnail_size);
            }
            if (thumbnail != nullptr) {
                // Fitted in the icon's square, proportions kept, centered.
                const float fit =
                    icon / static_cast<float>(std::max(thumbnail->width(), thumbnail->height()));
                const float shown_width = static_cast<float>(thumbnail->width()) * fit;
                const float shown_height = static_cast<float>(thumbnail->height()) * fit;
                renderer.draw_image(
                    *thumbnail,
                    {x + ((cell_width_ - shown_width) / 2.0F),
                     y + kIconTop + ((icon - shown_height) / 2.0F), shown_width, shown_height},
                    ui::Corners::all(kThumbnailRadius), cut ? kCutOpacity : 1.0F);
            } else {
                icons.draw_file(renderer, (*mime_).name(typed.mime), fs::icon_name(*mime_, typed),
                                x + ((cell_width_ - icon) / 2.0F), y + kIconTop, icon, style.icon,
                                typed.folder_icon);
            }
            if (edit_ != nullptr && edit_->holds(*folder_, entry)) {
                // One line across the cell, where the name starts.
                const float pad = ui::TextField::kPadding;
                edit_->place({x + kNameMargin - pad, y + kIconTop + icon + kNameGap - pad - 1.0F,
                              std::max(1.0F, cell_width_ - (2.0F * kNameMargin)) + (2.0F * pad),
                              kTextSize + (2.0F * pad) + 6.0F},
                             kTextSize);
                name_width_.push_back(cell_width_ - (2.0F * kNameMargin));
                continue;
            }
            const ui::TextLayout& name = fonts.layout(
                entry.name, kTextSize, renderer.scale(),
                std::max(1.0F, cell_width_ - (2.0F * kNameMargin)), ui::Ellipsis::middle);
            const float name_width = name.width / renderer.scale();
            name_width_.push_back(name_width);
            renderer.draw_layout(name, x + ((cell_width_ - name_width) / 2.0F),
                                 y + kIconTop + icon + kNameGap, style.text);
        }
    }

    const fs::DirectoryModel& model = folder_->model();
    if (model.error()) {
        draw_centered(renderer, model.error().message(), area_, area_.y + 24.0F, 14.0F,
                      palette.error);
    } else if (entries.empty() && !model.loading()) {
        draw_centered(renderer, i18n::tr("Empty folder"), area_, area_.y + 24.0F, 14.0F,
                      palette.on_surface_variant);
    }

    // Scrollbar: a thin thumb, only when there is something to scroll.
    if (scroller_.max_offset() > 0.0F) {
        const float thumb = std::max(kScrollbarMinThumb, area_.height * area_.height / content);
        const float travel = area_.height - thumb;
        const float thumb_y = area_.y + (travel * offset / scroller_.max_offset());
        renderer.draw_box({area_.right() - kScrollbarWidth - 2.0F, thumb_y, kScrollbarWidth, thumb},
                          {.fill = palette.on_surface_variant.with_alpha(0.45F),
                           .radius = ui::Corners::all(kScrollbarWidth / 2.0F)});
    }
    if (drop_ != nullptr && drop_->covers(folder_->path())) {
        renderer.draw_box({area_.x + 2.0F, area_.y + 2.0F, area_.width - 4.0F, area_.height - 4.0F},
                          {.fill = kTransparent,
                           .radius = ui::Corners::all(8.0F),
                           .border_width = 2.0F,
                           .border = palette.primary});
    }
    renderer.pop_clip();
    if (pane) {
        const bool grabbed = pane_drag_ || over_pane_edge_;
        const float line = grabbed ? 2.0F : 1.0F / renderer.scale();
        info_pane_->draw(renderer, {area_.right(), area_.y, pane_width, area_.height}, palette,
                         icons, *folder_, now);
        renderer.fill_rect({area_.right() - (grabbed ? 1.0F : 0.0F), area_.y, line, area_.height},
                           grabbed ? palette.primary : palette.outline_variant);
    }

    folder_->sniff(range.first * columns_, range.last * columns_);
    update_hover();  // content may have moved under a still pointer
}

std::optional<std::size_t> GridView::cell_at(float x, float y) const {
    if (!folder_ || x < area_.x || x >= area_.right() || y < area_.y || y >= area_.bottom()) {
        return std::nullopt;
    }
    const float content_x = x - area_.x - kPadding;
    const float content_y = y - area_.y + scroller_.offset() - kPadding;
    if (content_x < 0.0F || content_y < 0.0F || cell_width_ <= 0.0F) {
        return std::nullopt;
    }
    const auto column = static_cast<std::size_t>(content_x / cell_width_);
    const auto row = static_cast<std::size_t>(content_y / cell_height());
    if (column >= columns_) {
        return std::nullopt;
    }
    const std::size_t index = (row * columns_) + column;
    if (index >= folder_->entries().size()) {
        return std::nullopt;
    }
    return index;
}

bool GridView::on_pane_edge(float x, float y) const {
    return pane_edge_ >= 0.0F && std::abs(x - pane_edge_) <= InfoPane::kEdgeGrab && y >= area_.y &&
           y < area_.bottom();
}

void GridView::update_hover() {
    const std::optional<std::size_t> cell =
        pointer_inside_ && !pane_drag_ ? cell_at(pointer_x_, pointer_y_) : std::nullopt;
    if (cell != hovered_) {
        hovered_ = cell;
        redraw_();
    }
    const bool over = pane_drag_ || (pointer_inside_ && on_pane_edge(pointer_x_, pointer_y_));
    if (over != over_pane_edge_) {
        over_pane_edge_ = over;
        set_cursor_(over ? ui::CursorShape::ew_resize : ui::CursorShape::standard);
        redraw_();
    }
}

void GridView::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    if (!folder_) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion:
            pointer_inside_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            if (pane_drag_ && info_pane_ != nullptr) {
                // The edge follows the pointer; the cells per row change, the cursor's row stays.
                const float width = pane_drag_->start_width + (pane_drag_->start_x - event.x);
                keep_place();
                info_pane_->set_width(width);
            }
            update_hover();
            break;
        case Kind::leave:
            pointer_inside_ = false;
            update_hover();
            break;
        case Kind::scroll:
            if (event.x >= area_.right() && info_pane_ != nullptr &&
                info_pane_->scroll(event, now)) {
                break;
            }
            if (event.modifiers.ctrl && event.wheel_y != 0.0F) {
                // Ctrl+wheel: up makes the cells bigger, one size per notch.
                zoom_wheel_ += event.wheel_y;
                while (zoom_wheel_ <= -1.0F) {
                    zoom_wheel_ += 1.0F;
                    zoom(size_ + 1);
                }
                while (zoom_wheel_ >= 1.0F) {
                    zoom_wheel_ -= 1.0F;
                    zoom(size_ > 0 ? size_ - 1 : 0);
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
                if (event.y >= area_.y && event.y < area_.bottom()) {
                    right_click(event.x, event.y);
                }
                break;
            }
            if (event.button != ui::PointerButton::left || event.y < area_.y ||
                event.y >= area_.bottom()) {
                break;
            }
            if (info_pane_ != nullptr && on_pane_edge(event.x, event.y)) {
                if (event.click_count == 2) {
                    keep_place();
                    info_pane_->set_width(InfoPane::kWidth);  // double click: the default
                } else {
                    pane_drag_ = PaneDrag{.start_x = event.x, .start_width = info_pane_->width()};
                }
                update_hover();
                break;
            }
            if (event.x >= area_.right()) {
                if (info_pane_ != nullptr) {
                    static_cast<void>(info_pane_->click(event.x, event.y));
                }
                break;  // the pane, not a cell
            }
            const std::optional<std::size_t> cell = cell_at(event.x, event.y);
            const SelectMode mode = select_mode(event.modifiers, SelectMode::toggle);
            if (event.click_count == 2 && cell && mode == SelectMode::replace) {
                folder_->set_cursor(cell, mode);
                const fs::Entry* const entry = folder_->cursor_entry();
                if (entry != nullptr && !entry->is_directory() && open_) {
                    open_();  // a double click opens a file with its application
                } else {
                    enter();  // ... and a folder in the view
                }
                break;
            }
            if (cell || mode == SelectMode::replace) {
                folder_->press(cell, mode);  // a plain click on nothing clears
            }
            break;
        }
        case Kind::release:
            if (pane_drag_ && event.button == ui::PointerButton::left) {
                pane_drag_.reset();
                update_hover();
            }
            break;
    }
}

bool GridView::key(const ui::KeyEvent& event, Clock::time_point /*now*/) {
    if (!folder_) {
        return false;
    }
    Folder& folder = *folder_;
    const auto columns = static_cast<std::ptrdiff_t>(columns_);
    const auto visible_rows = static_cast<std::ptrdiff_t>(area_.height / cell_height());
    const std::ptrdiff_t page =
        std::max<std::ptrdiff_t>(1, visible_rows > 1 ? visible_rows - 1 : visible_rows) * columns;
    const std::optional<std::size_t> cursor = folder.cursor();
    const SelectMode mode = select_mode(event.modifiers, SelectMode::keep);
    const bool ctrl = event.modifiers.ctrl;
    switch (event.keysym) {
        case XKB_KEY_Left:
            folder.move_cursor(-1, mode);
            break;
        case XKB_KEY_Right:
            folder.move_cursor(1, mode);
            break;
        case XKB_KEY_Up:
            if (event.modifiers.alt) {
                go_up();
            } else if (!cursor || *cursor >= columns_) {  // not past the first row
                folder.move_cursor(-columns, mode);
            }
            break;
        case XKB_KEY_Down:
            if (event.modifiers.alt) {
                enter();
            } else if (!cursor || (*cursor / columns_) + 1 < rows()) {  // not past the last row
                folder.move_cursor(columns, mode);                      // clamped to the last entry
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
            if (!ctrl || !cursor) {
                return false;  // plain Space is for Quick Look (ROADMAP 5.3)
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
        case XKB_KEY_plus:
        case XKB_KEY_equal:
        case XKB_KEY_KP_Add:
            if (!ctrl) {
                return false;
            }
            zoom(size_ + 1);
            break;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract:
            if (!ctrl) {
                return false;
            }
            zoom(size_ > 0 ? size_ - 1 : 0);
            break;
        case XKB_KEY_0:
        case XKB_KEY_KP_0:
            if (!ctrl) {
                return false;
            }
            zoom(kUsualGridSize);
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

void GridView::right_click(float x, float y) {
    if (!folder_) {
        return;
    }
    if (x < area_.right()) {  // the pane keeps the selection it shows
        const std::optional<std::size_t> cell = cell_at(x, y);
        if (!cell) {
            folder_->set_cursor(std::nullopt);  // empty space: the folder's own menu
        } else {
            folder_->set_cursor(cell, folder_->is_selected(folder_->entries()[*cell].name)
                                          ? SelectMode::keep
                                          : SelectMode::replace);
        }
    }
    if (menu_) {
        menu_(x, y);
    }
}

std::optional<std::pair<float, float>> GridView::cursor_anchor() const {
    const std::optional<std::size_t> cursor = folder_ ? folder_->cursor() : std::nullopt;
    if (!cursor || columns_ == 0) {
        return std::nullopt;
    }
    const std::size_t row = *cursor / columns_;
    const std::size_t column = *cursor % columns_;
    const float x =
        area_.x + kPadding + (static_cast<float>(column) * cell_width_) + (cell_width_ / 2.0F);
    const float y = area_.y + kPadding + (static_cast<float>(row) * cell_height()) +
                    (cell_height() / 2.0F) - scroller_.offset();
    if (y < area_.y || y > area_.bottom()) {
        return std::nullopt;
    }
    return std::pair{x, y};
}

}  // namespace nordend::views
