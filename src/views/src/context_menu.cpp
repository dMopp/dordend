#include "nordend/views/context_menu.hpp"
#include "nordend/core/i18n.hpp"

#include <algorithm>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace nordend::views {
namespace {

constexpr float kItemHeight = 28.0F;
constexpr float kSeparatorHeight = 9.0F;
constexpr float kPadding = 6.0F;     // above the first item, below the last
constexpr float kInset = 4.0F;       // between the panel's side and an item's highlight
constexpr float kTextInset = 12.0F;  // between the panel's side and the text
constexpr float kKeysGap = 32.0F;    // between a label and its shortcut
constexpr float kArrowWidth = 18.0F;
constexpr float kMinWidth = 180.0F;
constexpr float kTextSize = 13.0F;
constexpr float kEdge = 4.0F;     // kept free between a panel and the window's edge
constexpr float kOverlap = 4.0F;  // a submenu over its menu's side
constexpr float kRadius = 10.0F;
constexpr float kMarkWidth = 18.0F;  // before the labels of a panel with radio items
constexpr float kDot = 6.0F;         // the dot of a checked radio item

bool has_radio(const std::vector<MenuItem>& items) {
    return std::ranges::any_of(items, &MenuItem::radio);
}

bool contains(const ui::Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

std::optional<std::size_t> index_at(const std::vector<ui::Rect>& rects, float x, float y) {
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (contains(rects[i], x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

float text_width(ui::Renderer& renderer, std::string_view text) {
    return renderer.fonts().layout(text, kTextSize, renderer.scale()).width / renderer.scale();
}

// What a submenu shows: its children, else one dimmed line.
std::vector<MenuItem> shown_children(const MenuItem& parent) {
    if (!parent.children.empty()) {
        return parent.children;
    }
    MenuItem line;
    line.label = parent.empty_text;
    line.enabled = false;
    return {line};
}

}  // namespace

ContextMenu::ContextMenu(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ContextMenu::open(float x, float y, std::vector<MenuItem> items, bool keyboard) {
    items_ = std::move(items);
    x_ = x;
    y_ = y;
    main_.reset();
    open_sub_.reset();
    sub_.reset();
    in_sub_ = false;
    panel_ = {};
    sub_panel_ = {};
    rects_.clear();
    sub_rects_.clear();
    if (keyboard) {
        main_ = step(items_, std::nullopt, 1);
    }
    redraw_();
}

void ContextMenu::close() {
    if (!is_open()) {
        return;
    }
    items_.clear();
    main_.reset();
    open_sub_.reset();
    sub_.reset();
    in_sub_ = false;
    redraw_();
}

void ContextMenu::set_submenu(std::string_view id, std::vector<MenuItem> children) {
    const auto it = std::ranges::find(items_, id, &MenuItem::id);
    if (it == items_.end()) {
        return;
    }
    it->children = std::move(children);
    const auto index = static_cast<std::size_t>(it - items_.begin());
    if (open_sub_ == index && in_sub_ && !sub_) {
        sub_ = step(it->children, std::nullopt, 1);  // the keyboard was waiting in it
    }
    redraw_();
}

std::optional<std::size_t> ContextMenu::step(const std::vector<MenuItem>& items,
                                             std::optional<std::size_t> from, int direction) {
    const std::size_t count = items.size();
    if (count == 0) {
        return std::nullopt;
    }
    std::size_t at = from.value_or(direction > 0 ? count - 1 : 0);
    for (std::size_t tries = 0; tries < count; ++tries) {
        at = direction > 0 ? (at + 1) % count : (at + count - 1) % count;
        if (items[at].enabled) {
            return at;
        }
    }
    return from;
}

const std::vector<MenuItem>* ContextMenu::sub_items() const {
    return open_sub_ ? &items_[*open_sub_].children : nullptr;
}

void ContextMenu::open_submenu(std::size_t index, bool keyboard) {
    open_sub_ = index;
    main_ = index;
    sub_.reset();
    in_sub_ = keyboard;
    if (keyboard) {
        sub_ = step(items_[index].children, std::nullopt, 1);
    }
    redraw_();
}

void ContextMenu::choose(const MenuItem& item) {
    const std::function<void()> action = item.action;  // item goes with the menu
    close();
    if (action) {
        action();
    }
}

bool ContextMenu::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    const bool in_sub = in_sub_ && open_sub_.has_value();
    const std::vector<MenuItem>& level = in_sub ? *sub_items() : items_;
    std::optional<std::size_t>& highlighted = in_sub ? sub_ : main_;
    const auto back_out = [this] {
        in_sub_ = false;
        open_sub_.reset();
        sub_.reset();
    };
    switch (event.keysym) {
        case XKB_KEY_Down:
            highlighted = step(level, highlighted, 1);
            break;
        case XKB_KEY_Up:
            highlighted = step(level, highlighted, -1);
            break;
        case XKB_KEY_Home:
            highlighted = step(level, std::nullopt, 1);
            break;
        case XKB_KEY_End:
            highlighted = step(level, std::nullopt, -1);
            break;
        case XKB_KEY_Right:
            if (!in_sub && main_ && items_[*main_].submenu && items_[*main_].enabled) {
                open_submenu(*main_, true);
            }
            break;
        case XKB_KEY_Left:
            if (in_sub) {
                back_out();
            }
            break;
        case XKB_KEY_Escape:
            if (in_sub) {
                back_out();
            } else {
                close();
            }
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (highlighted && level[*highlighted].enabled) {
                if (level[*highlighted].submenu) {
                    if (!in_sub) {
                        open_submenu(*highlighted, true);
                    }
                } else {
                    choose(level[*highlighted]);
                    return true;
                }
            }
            break;
        default:
            break;
    }
    redraw_();
    return true;
}

bool ContextMenu::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return false;
    }
    using Kind = ui::PointerEvent::Kind;
    const bool over_sub = open_sub_ && contains(sub_panel_, event.x, event.y);
    const std::optional<std::size_t> sub_index =
        over_sub ? index_at(sub_rects_, event.x, event.y) : std::nullopt;
    const std::optional<std::size_t> index = !over_sub && contains(panel_, event.x, event.y)
                                                 ? index_at(rects_, event.x, event.y)
                                                 : std::nullopt;
    const std::vector<MenuItem>* children = sub_items();
    switch (event.kind) {
        case Kind::enter:
        case Kind::motion:
            if (over_sub) {
                if (sub_index && children != nullptr && *sub_index < children->size() &&
                    (*children)[*sub_index].enabled) {
                    sub_ = sub_index;
                    in_sub_ = true;
                } else {
                    sub_.reset();
                }
            } else if (index) {
                const MenuItem& item = items_[*index];
                main_ = item.enabled ? index : std::nullopt;
                if (item.submenu && item.enabled) {
                    if (open_sub_ != index) {
                        open_submenu(*index, false);
                    }
                } else {
                    open_sub_.reset();
                    sub_.reset();
                    in_sub_ = false;
                }
            }
            redraw_();
            break;
        case Kind::press:
            if (over_sub) {
                if (sub_index && children != nullptr && *sub_index < children->size() &&
                    (*children)[*sub_index].enabled) {
                    choose((*children)[*sub_index]);
                }
            } else if (index) {
                const MenuItem& item = items_[*index];
                if (item.enabled && item.submenu) {
                    open_submenu(*index, false);
                } else if (item.enabled) {
                    choose(item);
                }
            } else if (!contains(panel_, event.x, event.y)) {
                close();  // a click outside closes the menu and does nothing else
            }
            break;
        default:
            break;
    }
    return true;
}

ui::Rect ContextMenu::measure(ui::Renderer& renderer, const std::vector<MenuItem>& items) {
    float width = kMinWidth;
    float height = 2.0F * kPadding;
    for (const MenuItem& item : items) {
        float line = text_width(renderer, item.label);
        if (!item.keys.empty()) {
            line += kKeysGap + text_width(renderer, i18n::keys(item.keys));
        }
        if (item.submenu) {
            line += kKeysGap + kArrowWidth;
        }
        width =
            std::max(width, line + (2.0F * kTextInset) + (has_radio(items) ? kMarkWidth : 0.0F));
        height += kItemHeight + (item.separator ? kSeparatorHeight : 0.0F);
    }
    return {0.0F, 0.0F, width, height};
}

void ContextMenu::draw_panel(ui::Renderer& renderer, const ui::Rect& panel,
                             const std::vector<MenuItem>& items,
                             std::optional<std::size_t> highlighted, std::vector<ui::Rect>& rects,
                             const theme::Palette& palette) {
    renderer.draw_shadow({panel.x, panel.y + 4.0F, panel.width, panel.height},
                         ui::Corners::all(kRadius), 12.0F, palette.shadow.with_alpha(0.6F));
    renderer.draw_box(panel, {.fill = palette.surface_container_high,
                              .radius = ui::Corners::all(kRadius),
                              .border_width = 1.0F,
                              .border = palette.outline_variant});
    rects.clear();
    const float label_x = panel.x + kTextInset + (has_radio(items) ? kMarkWidth : 0.0F);
    float y = panel.y + kPadding;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const MenuItem& item = items[i];
        if (item.separator) {
            renderer.fill_rect({panel.x + kTextInset, y + (kSeparatorHeight / 2.0F),
                                panel.width - (2.0F * kTextInset), 1.0F},
                               palette.outline_variant);
            y += kSeparatorHeight;
        }
        const ui::Rect rect{panel.x + kInset, y, panel.width - (2.0F * kInset), kItemHeight};
        rects.push_back(rect);
        const bool lit = highlighted == i && item.enabled;
        if (lit) {
            renderer.draw_box(rect, {.fill = palette.hover, .radius = ui::Corners::all(6.0F)});
        }
        ui::Color text = item.enabled ? palette.on_surface : palette.outline;
        ui::Color dim = item.enabled ? palette.on_surface_variant : palette.outline;
        if (lit) {
            text = palette.on_hover;
            dim = palette.on_hover;
        }
        const float text_y = y + ((kItemHeight - kTextSize) / 2.0F) - 2.0F;
        float right = panel.right() - kTextInset;
        if (item.submenu) {
            const float arrow = text_width(renderer, "›");
            renderer.draw_text("›", right - arrow, text_y, kTextSize, dim);
            right -= kArrowWidth;
        }
        float keys_width = 0.0F;
        if (!item.keys.empty()) {
            const std::string keys = i18n::keys(item.keys);
            keys_width = text_width(renderer, keys);
            renderer.draw_text(keys, right - keys_width, text_y, kTextSize, dim);
        }
        if (item.radio && item.checked) {
            renderer.draw_box({panel.x + kTextInset + ((kMarkWidth - kDot) / 2.0F) - 3.0F,
                               y + ((kItemHeight - kDot) / 2.0F), kDot, kDot},
                              {.fill = text, .radius = ui::Corners::all(kDot / 2.0F)});
        }
        const float room = right - keys_width - label_x - (item.keys.empty() ? 0.0F : kKeysGap);
        renderer.draw_text(item.label, label_x, text_y, kTextSize, text, std::max(room, 1.0F));
        y += kItemHeight;
    }
}

void ContextMenu::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                       const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    const ui::Rect size = measure(renderer, items_);
    const float width = std::min(size.width, bounds.width - (2.0F * kEdge));
    float x = x_;
    float y = y_;
    if (x + width > bounds.right() - kEdge) {
        x = std::max(bounds.x + kEdge, x - width);  // opens to the left of the point
    }
    if (y + size.height > bounds.bottom() - kEdge) {
        y = std::max(bounds.y + kEdge, bounds.bottom() - kEdge - size.height);
    }
    panel_ = {x, y, width, size.height};
    draw_panel(renderer, panel_, items_, main_, rects_, palette);

    sub_panel_ = {};
    sub_rects_.clear();
    if (!open_sub_ || *open_sub_ >= rects_.size()) {
        return;
    }
    const std::vector<MenuItem> children = shown_children(items_[*open_sub_]);
    const ui::Rect sub_size = measure(renderer, children);
    const float sub_width = std::min(sub_size.width, bounds.width - (2.0F * kEdge));
    float sub_x = panel_.right() - kOverlap;
    if (sub_x + sub_width > bounds.right() - kEdge) {
        sub_x = std::max(bounds.x + kEdge, panel_.x - sub_width + kOverlap);
    }
    float sub_y = rects_[*open_sub_].y - kPadding;
    if (sub_y + sub_size.height > bounds.bottom() - kEdge) {
        sub_y = std::max(bounds.y + kEdge, bounds.bottom() - kEdge - sub_size.height);
    }
    sub_panel_ = {sub_x, sub_y, sub_width, sub_size.height};
    draw_panel(renderer, sub_panel_, children, sub_, sub_rects_, palette);
}

}  // namespace nordend::views
