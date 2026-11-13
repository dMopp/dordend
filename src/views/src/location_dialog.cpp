#include "rummage/core/i18n.hpp"
#include "rummage/views/location_dialog.hpp"

#include <algorithm>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>


namespace rummage::views {
namespace {

constexpr float kCardWidth = 560.0F;
constexpr float kMargin = 24.0F;
constexpr float kPadding = 24.0F;
constexpr float kTitleSize = 16.0F;
constexpr float kTextSize = 13.0F;
constexpr float kFieldHeight = 30.0F;
constexpr float kLineHeight = 20.0F;
constexpr float kCheckSize = 16.0F;
constexpr float kButtonHeight = 34.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kButtonWidth = 116.0F;

}  // namespace

LocationDialog::LocationDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void LocationDialog::open(std::string text, Go go) {
    go_ = std::move(go);
    field_.set_text(std::move(text));
    field_.select_all();
    error_.clear();
    on_toggle_ = false;
    hovered_ = Target::none;
    redraw_();
}

void LocationDialog::close() {
    if (go_) {
        go_ = nullptr;
        redraw_();
    }
}

void LocationDialog::submit() {
    if (!go_) {
        return;
    }
    // go_ may open another card or close this one: keep it until it answered.
    const Go go = go_;
    std::string error = go(field_.text(), add_);
    if (error.empty()) {
        close();
        return;
    }
    error_ = std::move(error);
    on_toggle_ = false;
    redraw_();
}

bool LocationDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            submit();
            return true;
        case XKB_KEY_Escape:
            close();
            return true;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            on_toggle_ = !on_toggle_;
            redraw_();
            return true;
        default:
            break;
    }
    if (on_toggle_) {
        if (event.keysym == XKB_KEY_space) {
            add_ = !add_;
            redraw_();
        }
        return true;
    }
    if (field_.key(event)) {
        error_.clear();  // typing again: what was wrong may be right now
        redraw_();
    }
    return true;
}

void LocationDialog::paste(std::string_view text) {
    if (!is_open() || on_toggle_) {
        return;
    }
    text = text.substr(0, text.find_first_of("\r\n"));
    if (!text.empty()) {
        field_.insert(text);
        error_.clear();
        redraw_();
    }
}

LocationDialog::Target LocationDialog::target_at(float x, float y) const {
    if (!toggle_.empty() && toggle_.contains(x, y)) {
        return Target::add;
    }
    if (!cancel_.empty() && cancel_.contains(x, y)) {
        return Target::cancel;
    }
    if (!go_button_.empty() && go_button_.contains(x, y)) {
        return Target::go;
    }
    return Target::none;
}

bool LocationDialog::clickable_at(float x, float y) const {
    return target_at(x, y) != Target::none;
}

void LocationDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    const Target over = event.kind == Kind::leave ? Target::none : target_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    switch (over) {
        case Target::add:
            add_ = !add_;
            on_toggle_ = true;
            redraw_();
            return;
        case Target::cancel:
            close();
            return;
        case Target::go:
            submit();
            return;
        case Target::none:
            break;
    }
    if (field_at(event.x, event.y)) {
        on_toggle_ = false;
        field_.click(event.x, event.modifiers.shift);
        redraw_();
    }
}

void LocationDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::min(kCardWidth, bounds.width - (2.0F * kMargin));
    const float height = kPadding + 30.0F + kFieldHeight + 8.0F + kLineHeight + 14.0F + kCheckSize +
                         22.0F + kButtonHeight + kPadding;
    const ui::Rect card{bounds.x + ((bounds.width - width) / 2.0F),
                        bounds.y + ((bounds.height - height) / 3.0F), width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14.0F),
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    const float left = card.x + kPadding;
    const float room = width - (2.0F * kPadding);
    float y = card.y + kPadding;
    renderer.draw_text(i18n::tr("Go to Folder or Server"), left, y, kTitleSize, palette.on_surface,
                       room);
    y += 30.0F;

    const ui::FieldColors colors{
        .background = palette.surface_container_highest,
        .border = on_toggle_ ? palette.outline_variant : palette.primary,
        .text = palette.on_surface,
        .selection = palette.secondary_container,
        .caret = on_toggle_ ? palette.surface_container_highest : palette.on_surface};
    field_.draw(renderer, {left, y, room, kFieldHeight}, kTextSize, colors);
    y += kFieldHeight + 8.0F;
    if (error_.empty()) {
        renderer.draw_text(i18n::tr("/path, ~/path, smb://server/share, sftp://user@host/path"),
                           left, y, kTextSize, palette.on_surface_variant, room);
    } else {
        renderer.draw_text(error_, left, y, kTextSize, palette.error, room);
    }
    y += kLineHeight + 14.0F;

    const ui::Rect box{left, y, kCheckSize, kCheckSize};
    toggle_ = {box.x - 4.0F, y - 4.0F, room + 4.0F, kCheckSize + 8.0F};
    renderer.draw_box(box, {.fill = add_ ? palette.primary : palette.surface_container_highest,
                            .radius = ui::Corners::all(4.0F),
                            .border_width = on_toggle_ ? 2.0F : 1.0F,
                            .border = on_toggle_ ? palette.primary : palette.outline});
    if (add_) {
        renderer.fill_rect(box.inset(4.5F), palette.on_primary);
    }
    renderer.draw_text(i18n::tr("Add a server to the sidebar"), box.right() + 10.0F,
                       y + ((kCheckSize - kTextSize) / 2.0F) - 2.0F, kTextSize,
                       hovered_ == Target::add ? palette.on_surface : palette.on_surface_variant,
                       room - kCheckSize - 10.0F);
    y += kCheckSize + 22.0F;

    go_button_ = {card.right() - kPadding - kButtonWidth, y, kButtonWidth, kButtonHeight};
    cancel_ = {go_button_.x - kButtonGap - kButtonWidth, y, kButtonWidth, kButtonHeight};
    for (const bool go : {false, true}) {
        const ui::Rect& button = go ? go_button_ : cancel_;
        const bool hover = hovered_ == (go ? Target::go : Target::cancel);
        ui::Color fill = go ? palette.primary : palette.surface_container_highest;
        ui::Color text = go ? palette.on_primary : palette.on_surface;
        if (hover) {
            fill = go ? palette.primary_container : palette.surface_bright;
            text = go ? palette.on_primary_container : palette.on_surface;
        }
        renderer.draw_box(button, {.fill = fill, .radius = ui::Corners::all(8.0F)});
        renderer.draw_text(go ? i18n::tr("Go   ↵") : i18n::tr("Cancel   Esc"), button.x + 14.0F,
                           button.y + ((kButtonHeight - kTextSize) / 2.0F) - 2.0F, kTextSize, text,
                           button.width - 20.0F);
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
