#include "rummage/views/context_menu.hpp"
#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace rummage::views {
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
constexpr float kEdge = 4.0F;        // kept free between a panel and the window's edge
constexpr float kOverlap = 4.0F;     // a submenu over its menu's side
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

}  // namespace

ContextMenu::ContextMenu(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ContextMenu::open(float x, float y, std::vector<MenuItem> items, bool keyboard) {
    items_ = std::move(items);
    x_ = x;
    y_ = y;
    active_ = 0;
    levels_.clear();
    if (!items_.empty()) {
        levels_.emplace_back();
        if (keyboard)
            levels_.front().highlighted = step(items_, std::nullopt, 1);
    }
    redraw_();
}

void ContextMenu::close() {
    if (!is_open())
        return;
    items_.clear();
    levels_.clear();
    active_ = 0;
    redraw_();
}

void ContextMenu::set_submenu(std::string_view id, std::vector<MenuItem> children) {
    std::vector<std::size_t> path;
    const auto find = [&](const auto& self, std::vector<MenuItem>& items) -> MenuItem* {
        for (std::size_t i = 0; i < items.size(); ++i) {
            path.push_back(i);
            if (items[i].id == id)
                return &items[i];
            if (auto* found = self(self, items[i].children))
                return found;
            path.pop_back();
        }
        return nullptr;
    };
    auto* found = find(find, items_);
    if (!found)
        return;
    // A configured layout can move a child (e.g. Empty File) out of this dynamic group.
    std::unordered_set<std::string> elsewhere;
    const auto collect = [&](const auto& self, const std::vector<MenuItem>& items) -> void {
        for (const auto& item : items) {
            if (&item == found)
                continue;
            if (!item.id.empty())
                elsewhere.insert(item.id);
            self(self, item.children);
        }
    };
    collect(collect, items_);
    std::erase_if(children, [&](const MenuItem& item) {
        return !item.id.empty() && elsewhere.contains(item.id);
    });
    if (!children.empty()) {
        children.front().separator = false;
    }
    found->children = std::move(children);
    bool opened = levels_.size() > path.size();
    for (std::size_t i = 0; opened && i < path.size(); ++i) {
        opened = levels_[i + 1].parent == path[i];
    }
    if (opened) {
        const bool keyboard = active_ >= path.size();
        levels_.resize(path.size() + 1);
        auto& level = levels_.back();
        level.rects.clear();
        level.highlighted = keyboard ? step(found->children, std::nullopt, 1) : std::nullopt;
        active_ = std::min(active_, path.size());
    }
    redraw_();
}

std::optional<std::size_t> ContextMenu::step(const std::vector<MenuItem>& items,
                                             std::optional<std::size_t> from, int direction) {
    const std::size_t count = items.size();
    if (count == 0)
        return std::nullopt;
    std::size_t at = from.value_or(direction > 0 ? count - 1 : 0);
    for (std::size_t tries = 0; tries < count; ++tries) {
        at = direction > 0 ? (at + 1) % count : (at + count - 1) % count;
        if (items[at].enabled)
            return at;
    }
    return std::nullopt;
}

const std::vector<MenuItem>& ContextMenu::level_items(std::size_t depth) const {
    const auto* items = &items_;
    for (std::size_t i = 1; i <= depth; ++i) {
        items = &(*items)[levels_[i].parent].children;
    }
    return *items;
}

void ContextMenu::open_submenu(std::size_t depth, std::size_t index, bool keyboard) {
    levels_[depth].highlighted = index;
    if (levels_.size() <= depth + 1 || levels_[depth + 1].parent != index) {
        levels_.resize(depth + 1);
        levels_.push_back(
            Level{.parent = index, .highlighted = std::nullopt, .panel = {}, .rects = {}});
    }
    if (keyboard) {
        active_ = depth + 1;
        if (!levels_[active_].highlighted) {
            levels_[active_].highlighted = step(level_items(active_), std::nullopt, 1);
        }
    }
    redraw_();
}

void ContextMenu::choose(const MenuItem& item) {
    const std::function<void()> action = item.action;
    close();
    if (action)
        action();
}

bool ContextMenu::key(const ui::KeyEvent& event) {
    if (!is_open())
        return false;
    const auto& items = level_items(active_);
    const auto selected = levels_[active_].highlighted;
    switch (event.keysym) {
        case XKB_KEY_Down:
        case XKB_KEY_Up:
        case XKB_KEY_Home:
        case XKB_KEY_End: {
            const bool forward = event.keysym == XKB_KEY_Down || event.keysym == XKB_KEY_Home;
            const bool edge = event.keysym == XKB_KEY_Home || event.keysym == XKB_KEY_End;
            levels_[active_].highlighted =
                step(items, edge ? std::nullopt : selected, forward ? 1 : -1);
            levels_.resize(active_ + 1);
            break;
        }
        case XKB_KEY_Right:
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (selected && items[*selected].enabled) {
                if (items[*selected].submenu) {
                    open_submenu(active_, *selected, true);
                } else if (event.keysym != XKB_KEY_Right) {
                    choose(items[*selected]);
                    return true;
                }
            }
            break;
        case XKB_KEY_Left:
        case XKB_KEY_Escape:
            if (active_ > 0) {
                levels_.resize(active_);
                --active_;
            } else if (event.keysym == XKB_KEY_Escape) {
                close();
            }
            break;
        default:
            break;
    }
    redraw_();
    return true;
}

bool ContextMenu::pointer(const ui::PointerEvent& event) {
    if (!is_open())
        return false;
    using Kind = ui::PointerEvent::Kind;
    if (event.kind != Kind::enter && event.kind != Kind::motion && event.kind != Kind::press) {
        return true;
    }
    // Deepest panels are drawn last and receive the pointer first when panels overlap.
    std::optional<std::size_t> over;
    for (std::size_t i = levels_.size(); i > 0; --i) {
        if (contains(levels_[i - 1].panel, event.x, event.y)) {
            over = i - 1;
            break;
        }
    }
    if (!over) {
        if (event.kind == Kind::press)
            close();
        return true;
    }
    const auto depth = *over;
    const auto index = index_at(levels_[depth].rects, event.x, event.y);
    const auto& items = level_items(depth);
    if (index && *index < items.size()) {
        const auto& item = items[*index];
        active_ = depth;
        levels_[depth].highlighted = item.enabled ? index : std::nullopt;
        if (item.enabled && item.submenu) {
            open_submenu(depth, *index, false);
        } else if (item.enabled && event.kind == Kind::press) {
            choose(item);
            return true;
        } else {
            levels_.resize(depth + 1);
        }
    }
    redraw_();
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
    renderer.draw_shadow({panel.x, panel.y + 4.0F, panel.width, panel.height}, {}, 12.0F,
                         palette.shadow.with_alpha(0.6F));
    renderer.draw_box(panel, {.fill = palette.surface_container_high,
                              .radius = {},
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
            renderer.draw_box(rect, {.fill = palette.hover, .radius = {}});
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
                              {.fill = text, .radius = {}});
        }
        const float room = right - keys_width - label_x - (item.keys.empty() ? 0.0F : kKeysGap);
        renderer.draw_text(item.label, label_x, text_y, kTextSize, text, std::max(room, 1.0F));
        y += kItemHeight;
    }
}

void ContextMenu::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                       const theme::Palette& palette) {
    if (!is_open())
        return;
    for (std::size_t depth = 0; depth < levels_.size(); ++depth) {
        const auto& actual = level_items(depth);
        std::vector<MenuItem> placeholder;
        if (actual.empty() && depth > 0) {
            MenuItem line;
            line.label = level_items(depth - 1)[levels_[depth].parent].empty_text;
            line.enabled = false;
            placeholder.push_back(std::move(line));
        }
        const auto& items = actual.empty() ? placeholder : actual;
        const auto size = measure(renderer, items);
        const float width = std::min(size.width, std::max(1.0F, bounds.width - 2.0F * kEdge));
        float x = x_;
        float y = y_;
        if (depth > 0) {
            const auto& parent = levels_[depth - 1];
            x = parent.panel.right() - kOverlap;
            if (x + width > bounds.right() - kEdge) {
                x = parent.panel.x - width + kOverlap;
            }
            y = parent.rects[levels_[depth].parent].y - kPadding;
        } else if (x + width > bounds.right() - kEdge) {
            x -= width;
        }
        x = std::clamp(x, bounds.x + kEdge,
                       std::max(bounds.x + kEdge, bounds.right() - kEdge - width));
        y = std::clamp(y, bounds.y + kEdge,
                       std::max(bounds.y + kEdge, bounds.bottom() - kEdge - size.height));
        auto& level = levels_[depth];
        level.panel = {x, y, width, size.height};
        draw_panel(renderer, level.panel, items, level.highlighted, level.rects, palette);
    }
}

}  // namespace rummage::views
