#pragma once

// How entries are shown and selected (a row of a column, a cell of the grid), shared by the
// views.

#include <optional>

#include "rummage/fs/entry.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/color.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

inline constexpr ui::Color kTransparent{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 0.0F};

// The selection is strong (primary) in the focused view and quiet elsewhere, where it marks the
// path to the focused one. When the cursor is not the whole selection (several entries selected,
// or the cursor on an unselected one), a ring shows where it is.
struct ItemStyle {
    std::optional<ui::Color> fill;
    std::optional<ui::Color> ring;
    ui::Color icon;
    ui::Color chevron;
    ui::Color text;
};

// How much of an entry cut with Ctrl+X shows (icon, name, thumbnail).
inline constexpr float kCutOpacity = 0.45F;

struct ItemState {
    bool selected{false};
    bool cursor{false};
    bool ring{false};  // the cursor needs its own mark
    bool focused{false};
    bool hovered{false};
    bool cut{false};   // cut with Ctrl+X, not pasted yet: faded
    bool drop{false};  // a drag over it would drop into it
};

inline ItemStyle item_style(const theme::Palette& palette, const fs::Entry& entry,
                            const ItemState& item) {
    ItemStyle style{.fill = std::nullopt,
                    .ring = std::nullopt,
                    .icon = entry.is_directory() ? palette.primary : palette.on_surface_variant,
                    .chevron = palette.on_surface_variant,
                    .text = entry.hidden() ? palette.outline : palette.on_surface};
    if (item.selected && item.focused) {
        style.fill = palette.primary;
        style.icon = palette.on_primary;
        style.chevron = palette.on_primary;
        style.text = palette.on_primary;
    } else if (item.selected) {
        style.fill = palette.surface_container_highest;
    } else if (item.hovered) {
        style.fill = palette.surface_container_high;
    }
    if (item.cursor && item.ring && item.focused) {
        style.ring = item.selected ? palette.on_primary : palette.primary;
    }
    if (item.drop) {
        style.fill = palette.primary_container;
        style.ring = palette.primary;
        style.icon = palette.on_primary_container;
        style.chevron = palette.on_primary_container;
        style.text = palette.on_primary_container;
    }
    if (item.cut) {
        style.icon.a *= kCutOpacity;
        style.text.a *= kCutOpacity;
    }
    return style;
}

// The selection mode of a click or a key: Shift extends from the anchor, Ctrl does what `ctrl`
// says (toggle for a click, cursor alone for a key), neither replaces.
inline SelectMode select_mode(const ui::Modifiers& modifiers, SelectMode ctrl) {
    if (modifiers.shift) {
        return SelectMode::extend;
    }
    return modifiers.ctrl ? ctrl : SelectMode::replace;
}

}  // namespace rummage::views
