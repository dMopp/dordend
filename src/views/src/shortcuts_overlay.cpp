#include "rummage/views/shortcuts_overlay.hpp"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/views/shortcuts.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr float kMaxCardWidth = 960.0F;
constexpr float kMargin = 32.0F;   // around the card, at least
constexpr float kPadding = 28.0F;  // inside the card
constexpr float kTitleSize = 16.0F;
constexpr float kHeadHeight = 64.0F;
constexpr float kGroupSize = 13.0F;
constexpr float kGroupHeight = 36.0F;
constexpr float kGroupGap = 4.0F;  // between groups
constexpr float kRowHeight = 26.0F;
constexpr float kTextSize = 13.0F;

float rows_height(std::string_view group) {
    return static_cast<float>(std::ranges::count_if(shortcuts(),
                                                    [group](const Shortcut& shortcut) {
                                                        return shortcut.group == group &&
                                                               !shortcut.keys.empty();
                                                    })) *
           kRowHeight;
}

}  // namespace

std::vector<std::string_view> overlay_groups() {
    return {"General", "Moving",  "Selecting", "Files",     "Tabs",
            "Panes",   "Sidebar", "Zoom",      "Quick Look"};
}

ShortcutsOverlay::ShortcutsOverlay(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ShortcutsOverlay::open() {
    if (!open_) {
        open_ = true;
        offset_ = 0.0F;
        dragging_ = false;
        keyboard_focus_ = false;
        redraw_();
    }
}

void ShortcutsOverlay::close() {
    if (open_) {
        open_ = false;
        dragging_ = false;
        keyboard_focus_ = false;
        redraw_();
    }
}

void ShortcutsOverlay::layout(const ui::Rect& bounds) {
    bounds_ = bounds;
    const float width = std::max(0.0F, std::min(kMaxCardWidth, bounds.width - 2.0F * kMargin));
    content_ = 0.0F;
    for (const auto& group : groups_) {
        content_ += kGroupHeight + kGroupGap + (group.expanded ? rows_height(group.name) : 0.0F);
    }
    content_ -= kGroupGap;
    // Keep the card stable when a group opens or closes.
    const float height = std::max(0.0F, std::min(bounds.height - 2.0F * kMargin, 720.0F));
    card_ = {bounds.x + (bounds.width - width) / 2.0F, bounds.y + (bounds.height - height) / 2.0F,
             width, height};
    viewport_ = {card_.x + kPadding, card_.y + kPadding + kHeadHeight,
                 std::max(0.0F, width - 2.0F * kPadding - 16.0F),
                 std::max(0.0F, height - 2.0F * kPadding - kHeadHeight)};
    close_button_ = {card_.right() - kPadding - 32.0F, card_.y + 18.0F, 32.0F, 32.0F};
    track_ = {viewport_.right() + 8.0F, viewport_.y, 8.0F, viewport_.height};
    max_scroll_ = std::max(0.0F, content_ - viewport_.height);
    offset_ = std::clamp(offset_, 0.0F, max_scroll_);
    float y = viewport_.y - offset_;
    for (auto& group : groups_) {
        group.header = {viewport_.x, y, viewport_.width, kGroupHeight};
        y += kGroupHeight + kGroupGap + (group.expanded ? rows_height(group.name) : 0.0F);
    }
}

ui::Rect ShortcutsOverlay::thumb() const {
    const float height =
        content_ > 0.0F
            ? std::min(track_.height, std::max(28.0F, track_.height * viewport_.height / content_))
            : track_.height;
    const float y = max_scroll_ > 0.0F ? offset_ / max_scroll_ * (track_.height - height) : 0.0F;
    return {track_.x, track_.y + y, track_.width, height};
}

void ShortcutsOverlay::scroll_to(float offset) {
    offset_ = std::clamp(offset, 0.0F, max_scroll_);
    layout(bounds_);
    redraw_();
}

void ShortcutsOverlay::pointer(const ui::PointerEvent& event) {
    if (!open_) {
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::release) {
        dragging_ = false;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll) {
        scroll_to(offset_ + (event.wheel_y != 0.0F ? event.wheel_y * 48.0F : event.scroll_y));
    } else if (event.kind == ui::PointerEvent::Kind::press &&
               event.button == ui::PointerButton::left) {
        if (!card_.contains(event.x, event.y) || close_button_.contains(event.x, event.y)) {
            close();
        } else if (max_scroll_ > 0.0F && track_.contains(event.x, event.y)) {
            const auto handle = thumb();
            if (handle.contains(event.x, event.y)) {
                dragging_ = true;
                drag_y_ = event.y - handle.y;
            } else {
                scroll_to(offset_ + (event.y < handle.y ? -viewport_.height : viewport_.height));
            }
        } else if (viewport_.contains(event.x, event.y)) {
            for (std::size_t i = 0; i < groups_.size(); ++i) {
                if (groups_[i].header.contains(event.x, event.y)) {
                    focused_group_ = i;
                    keyboard_focus_ = false;
                    set_expanded(!groups_[i].expanded);
                    break;
                }
            }
        }
    } else if (event.kind == ui::PointerEvent::Kind::motion && dragging_) {
        const float travel = track_.height - thumb().height;
        if (travel > 0.0F) {
            scroll_to((event.y - track_.y - drag_y_) / travel * max_scroll_);
        }
    }
}

void ShortcutsOverlay::key(const ui::KeyEvent& event) {
    if (!open_) {
        return;
    }
    if ((event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) &&
        (event.modifiers == ui::Modifiers{} || event.modifiers == ui::Modifiers{.shift = true})) {
        const bool backwards = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab;
        focused_group_ = keyboard_focus_ ? (focused_group_ + (backwards ? groups_.size() - 1 : 1)) %
                                               groups_.size()
                                         : (backwards ? groups_.size() - 1 : 0);
        keyboard_focus_ = true;
        reveal_group();
        return;
    }
    if (event.modifiers != ui::Modifiers{}) {
        return;
    }
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_space:
            keyboard_focus_ = true;
            set_expanded(!groups_[focused_group_].expanded);
            break;
        case XKB_KEY_Left:
        case XKB_KEY_Right:
            keyboard_focus_ = true;
            set_expanded(event.keysym == XKB_KEY_Right);
            break;
        case XKB_KEY_Up:
            scroll_to(offset_ - kRowHeight);
            break;
        case XKB_KEY_Down:
            scroll_to(offset_ + kRowHeight);
            break;
        case XKB_KEY_Page_Up:
            scroll_to(offset_ - viewport_.height);
            break;
        case XKB_KEY_Page_Down:
            scroll_to(offset_ + viewport_.height);
            break;
        case XKB_KEY_Home:
            scroll_to(0.0F);
            break;
        case XKB_KEY_End:
            scroll_to(max_scroll_);
            break;
        default:
            break;
    }
}

void ShortcutsOverlay::set_expanded(bool expanded) {
    groups_[focused_group_].expanded = expanded;
    layout(bounds_);
    reveal_group();
}

void ShortcutsOverlay::reveal_group() {
    if (viewport_.empty()) {
        redraw_();
        return;
    }
    const auto header = groups_[focused_group_].header;
    if (header.y < viewport_.y) {
        scroll_to(offset_ + header.y - viewport_.y);
    } else if (header.bottom() > viewport_.bottom()) {
        scroll_to(offset_ + header.bottom() - viewport_.bottom());
    } else {
        redraw_();
    }
}

void ShortcutsOverlay::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                            const theme::Palette& palette) {
    if (!open_) {
        return;
    }
    layout(bounds);
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const auto& card = card_;
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = {},
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    renderer.draw_text(i18n::tr("Keyboard shortcuts"), card.x + kPadding, card.y + kPadding, kTitleSize,
                       palette.on_surface,
                       std::max(1.0F, close_button_.x - card.x - kPadding - 8.0F));
    renderer.draw_text("×", close_button_.x + 9.0F, close_button_.y + 4.0F, 20.0F,
                       palette.on_surface);
    renderer.draw_text(i18n::tr("Click a group to expand · Tab to select · Enter to toggle"),
                       card.x + kPadding, card.y + kPadding + 28.0F, 11.0F,
                       palette.on_surface_variant, std::max(1.0F, viewport_.width));
    renderer.push_clip(viewport_);

    const float keys_width = std::min(240.0F, viewport_.width * 0.38F);
    for (std::size_t i = 0; i < groups_.size(); ++i) {
        const auto& group = groups_[i];
        const auto& header = group.header;
        const bool focused = keyboard_focus_ && focused_group_ == i;
        renderer.draw_box(header, {.fill = palette.surface_container_high,
                                   .radius = {},
                                   .border_width = focused ? 1.0F : 0.0F,
                                   .border = palette.primary});
        renderer.fill_rect({header.x, header.bottom() - 1.0F, header.width, 1.0F},
                           palette.outline_variant);
        renderer.draw_text(group.expanded ? "▾" : "▸", header.x + 4.0F, header.y + 9.0F, kGroupSize,
                           palette.primary);
        renderer.draw_text(i18n::tr(group.name), header.x + 24.0F, header.y + 9.0F, kGroupSize,
                           palette.on_surface);
        if (!group.expanded) {
            continue;
        }
        float y = header.bottom();
        for (const Shortcut& shortcut : shortcuts()) {
            if (shortcut.group != group.name || shortcut.keys.empty()) {
                continue;
            }
            renderer.draw_text(i18n::keys(shortcut.keys), header.x + 24.0F, y + 5.0F, kTextSize,
                               palette.primary, std::max(1.0F, keys_width - 36.0F));
            renderer.draw_text(i18n::tr(shortcut.action), header.x + keys_width, y + 5.0F, kTextSize,
                               palette.on_surface, std::max(1.0F, viewport_.width - keys_width));
            y += kRowHeight;
        }
    }
    renderer.pop_clip();
    if (max_scroll_ > 0.0F) {
        renderer.draw_box(track_, {.fill = palette.outline_variant, .radius = {}});
        renderer.draw_box(thumb(), {.fill = palette.primary, .radius = {}});
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
