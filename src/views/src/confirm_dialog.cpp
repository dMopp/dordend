#include "rummage/views/confirm_dialog.hpp"
#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace rummage::views {
namespace {

constexpr float kCardWidth = 480.0F;
constexpr float kMargin = 24.0F;
constexpr float kPadding = 24.0F;
constexpr float kTitleSize = 16.0F;
constexpr float kTextSize = 13.0F;
constexpr float kButtonHeight = 34.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kCancelWidth = 116.0F;
constexpr float kActionWidth = 116.0F;

bool contains(const ui::Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

}  // namespace

ConfirmDialog::ConfirmDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ConfirmDialog::open(std::string title, std::string detail, std::string action, Answer answer,
                         std::string warning) {
    ++generation_;
    password_mode_ = false;
    password_field_.set_text({});
    warning_ = std::move(warning);
    title_ = std::move(title);
    detail_ = std::move(detail);
    action_ = std::move(action);
    answer_ = std::move(answer);
    hovered_.reset();
    redraw_();
}

void ConfirmDialog::open_password(std::string name, bool retry,
                                  std::function<void(std::optional<std::string>)> answer) {
    open(
        "Password for “" + name + "”",
        retry ? "Password rejected or archive damaged. Try again." : "Enter the archive password.",
        "Extract",
        [this, answer = std::move(answer)](bool confirmed) {
            std::optional<std::string> password;
            if (confirmed)
                password = password_field_.text();
            password_field_.set_text({});
            answer(std::move(password));
        },
        "");
    password_mode_ = true;
}

void ConfirmDialog::paste(std::string_view text) {
    if (is_open() && password_mode_ &&
        text.find_first_of(std::string_view{"\r\n\0", 3}) == std::string_view::npos) {
        password_field_.insert(text);
        redraw_();
    }
}

void ConfirmDialog::set_detail(std::string detail) {
    detail_ = std::move(detail);
    redraw_();
}

void ConfirmDialog::close() {
    if (answer_) {
        answer_ = nullptr;
        password_field_.set_text({});
        redraw_();
    }
}

void ConfirmDialog::choose(bool confirmed) {
    Answer answer = std::exchange(answer_, nullptr);
    redraw_();
    if (answer) {
        answer(confirmed);
    }
}

bool ConfirmDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    if (password_mode_ && event.keysym != XKB_KEY_Return && event.keysym != XKB_KEY_KP_Enter &&
        event.keysym != XKB_KEY_Escape) {
        if (event.text.find_first_of(std::string_view{"\r\n\0", 3}) == std::string::npos)
            password_field_.key(event);
        redraw_();
        return true;
    }
    if (event.modifiers.ctrl || event.modifiers.alt) {
        return true;
    }
    switch (event.keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(true);
            break;
        case XKB_KEY_Escape:
            choose(false);
            break;
        default:
            break;
    }
    return true;
}

void ConfirmDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    if (password_mode_ && event.kind == Kind::press && event.button == ui::PointerButton::left &&
        password_field_.bounds().contains(event.x, event.y)) {
        password_field_.click(event.x, false);
        redraw_();
    }
    std::optional<std::size_t> over;
    if (event.kind != Kind::leave) {
        for (std::size_t i = 0; i < buttons_.size(); ++i) {
            if (contains(buttons_.at(i), event.x, event.y)) {
                over = i;
            }
        }
    }
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind == Kind::press && event.button == ui::PointerButton::left && over) {
        choose(*over == kAction);
    }
}

void ConfirmDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                         const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::min(kCardWidth, bounds.width - (2.0F * kMargin));
    const float height = kPadding + 30.0F + 22.0F + 36.0F + kButtonHeight + kPadding;
    const ui::Rect card{bounds.x + ((bounds.width - width) / 2.0F),
                        bounds.y + ((bounds.height - height) / 2.0F), width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = {},
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    const float left = card.x + kPadding;
    const float room = width - (2.0F * kPadding);
    float y = card.y + kPadding;
    renderer.draw_text(title_, left, y, kTitleSize, palette.on_surface, room, ui::Ellipsis::middle);
    y += 30.0F;
    renderer.draw_text(detail_, left, y, kTextSize, palette.on_surface, room);
    y += 22.0F;
    if (password_mode_) {
        password_field_.draw(renderer, {left, y - 2.0F, room, 30.0F}, kTextSize,
                             {.background = palette.surface_container_highest,
                              .border = palette.primary,
                              .text = palette.on_surface,
                              .selection = palette.primary_container,
                              .caret = palette.primary},
                             true);
    } else {
        renderer.draw_text(warning_, left, y, kTextSize, palette.on_surface_variant, room);
    }
    y += 36.0F;

    buttons_.at(kAction) = {card.right() - kPadding - kActionWidth, y, kActionWidth, kButtonHeight};
    buttons_.at(kCancel) = {buttons_.at(kAction).x - kButtonGap - kCancelWidth, y, kCancelWidth,
                            kButtonHeight};
    for (std::size_t i = 0; i < kButtons; ++i) {
        const ui::Rect& button = buttons_.at(i);
        const bool action = i == kAction;
        ui::Color fill = action ? (password_mode_ ? palette.primary : palette.error)
                                : palette.surface_container_highest;
        ui::Color text =
            action ? (password_mode_ ? palette.on_primary : palette.on_error) : palette.on_surface;
        if (hovered_ == i) {
            fill = action ? (password_mode_ ? palette.primary_container : palette.error_container)
                          : palette.surface_bright;
            text = action ? (password_mode_ ? palette.on_primary_container
                                            : palette.on_error_container)
                          : palette.on_surface;
        }
        renderer.draw_box(button, {.fill = fill, .radius = {}});
        const std::string label =
            action ? std::format("{}   ↵", action_) : std::string{i18n::tr("Cancel   Esc")};
        renderer.draw_text(label, button.x + 14.0F,
                           button.y + ((kButtonHeight - kTextSize) / 2.0F) - 2.0F, kTextSize, text,
                           button.width - 20.0F);
    }
    renderer.pop_clip();
}

}  // namespace rummage::views
