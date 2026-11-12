#include "nordend/views/conflict_dialog.hpp"
#include "nordend/core/i18n.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace nordend::views {
namespace {

constexpr float kCardWidth = 480.0F;
constexpr float kMargin = 24.0F;
constexpr float kPadding = 24.0F;
constexpr float kTitleSize = 16.0F;
constexpr float kTextSize = 13.0F;
constexpr float kButtonHeight = 34.0F;
constexpr float kButtonGap = 8.0F;
constexpr float kBox = 16.0F;  // the checkbox

constexpr std::array<std::string_view, 3> kLabels{"Skip   S", "Replace   R", "Keep both   ↵"};
constexpr std::array<float, 3> kWidths{96.0F, 124.0F, 140.0F};

bool contains(const ui::Rect& rect, float x, float y) {
    return x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

}  // namespace

ConflictDialog::ConflictDialog(std::function<void()> redraw) : redraw_{std::move(redraw)} {}

void ConflictDialog::open(const fs::Conflict& conflict, std::string destination, Answer answer) {
    conflict_ = conflict;
    destination_ = std::move(destination);
    answer_ = std::move(answer);
    for_the_rest_ = false;
    hovered_.reset();
    redraw_();
}

void ConflictDialog::close() {
    if (answer_) {
        answer_ = nullptr;
        redraw_();
    }
}

void ConflictDialog::choose(fs::Resolution resolution) {
    if (resolution == fs::Resolution::replace && !can_replace()) {
        return;
    }
    Answer answer = std::exchange(answer_, nullptr);
    redraw_();
    if (answer) {
        answer(resolution, for_the_rest_ && resolution != fs::Resolution::cancel);
    }
}

void ConflictDialog::toggle_rest() {
    for_the_rest_ = !for_the_rest_;
    redraw_();
}

bool ConflictDialog::key(const ui::KeyEvent& event) {
    if (!is_open()) {
        return false;
    }
    if (event.modifiers.ctrl || event.modifiers.alt) {
        return true;
    }
    switch (event.keysym) {
        case XKB_KEY_r:
        case XKB_KEY_R:
            choose(fs::Resolution::replace);
            break;
        case XKB_KEY_s:
        case XKB_KEY_S:
            choose(fs::Resolution::skip);
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            choose(fs::Resolution::keep_both);
            break;
        case XKB_KEY_a:
        case XKB_KEY_A:
            toggle_rest();
            break;
        case XKB_KEY_Escape:
            choose(fs::Resolution::cancel);
            break;
        default:
            break;
    }
    return true;
}

std::optional<std::size_t> ConflictDialog::button_at(float x, float y) const {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (contains(buttons_.at(i), x, y)) {
            return i;
        }
    }
    return std::nullopt;
}

void ConflictDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open()) {
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    const std::optional<std::size_t> over =
        event.kind == Kind::leave ? std::nullopt : button_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left) {
        return;
    }
    if (contains(checkbox_, event.x, event.y)) {
        toggle_rest();
    } else if (over == kSkip) {
        choose(fs::Resolution::skip);
    } else if (over == kReplace) {
        choose(fs::Resolution::replace);
    } else if (over == kKeepBoth) {
        choose(fs::Resolution::keep_both);
    }
}

void ConflictDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                          const theme::Palette& palette) {
    if (!is_open()) {
        return;
    }
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::min(kCardWidth, bounds.width - (2.0F * kMargin));
    const float height = kPadding + 24.0F + 26.0F + 36.0F + kButtonHeight + kPadding;
    const ui::Rect card{bounds.x + ((bounds.width - width) / 2.0F),
                        bounds.y + ((bounds.height - height) / 2.0F), width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14.0F),
                             .border_width = 1.0F,
                             .border = palette.outline_variant});
    renderer.push_clip(card);
    const float left = card.x + kPadding;
    const float room = width - (2.0F * kPadding);
    float y = card.y + kPadding;
    renderer.draw_text(i18n::trf("“{}” is already in {}", conflict_.name, destination_), left, y,
                       kTitleSize, palette.on_surface, room, ui::Ellipsis::middle);
    y += 30.0F;
    std::string_view note = i18n::tr("Replace keeps the new one; the old one goes once it is complete.");
    if (!can_replace()) {
        note = conflict_.directory == conflict_.target_directory
                   ? i18n::tr("A folder is not replaced: keep both or skip.")
                   : i18n::tr("A file and a folder cannot replace each other.");
    } else if (conflict_.directory) {
        note = i18n::tr("Replace merges the folders: files with the same name are replaced.");
    }
    renderer.draw_text(note, left, y, kTextSize, palette.on_surface_variant, room);
    y += 30.0F;

    // "The same for the rest", then the buttons, right-aligned.
    checkbox_ = {left, y + 1.0F, kBox, kBox};
    renderer.draw_box(checkbox_,
                      {.fill = for_the_rest_ ? palette.primary : palette.surface_container_high,
                       .radius = ui::Corners::all(4.0F),
                       .border_width = for_the_rest_ ? 0.0F : 1.5F,
                       .border = palette.outline});
    if (for_the_rest_) {
        renderer.fill_rect({left + 4.0F, y + 5.0F, kBox - 8.0F, kBox - 8.0F}, palette.on_primary);
    }
    const float label = renderer.draw_text(i18n::tr("Do the same for the rest"), left + kBox + 10.0F, y,
                                           kTextSize, palette.on_surface, room - kBox - 10.0F);
    renderer.draw_text("A", left + kBox + 10.0F + label + 10.0F, y, kTextSize, palette.primary);
    checkbox_.width = kBox + 10.0F + label;  // the label is clickable too
    const std::string_view stop = i18n::tr("Escape stops");
    const float stop_width =
        renderer.fonts().layout(stop, kTextSize, renderer.scale()).width / renderer.scale();
    renderer.draw_text(stop, card.right() - kPadding - stop_width, y, kTextSize,
                       palette.on_surface_variant);
    y += 36.0F;

    float x = card.right() - kPadding;
    for (std::size_t i = kButtons; i-- > 0;) {
        // At least the usual width, wider when a translation needs it.
        const float button_width = std::max(
            kWidths.at(i),
            (renderer.fonts().layout(i18n::tr(kLabels.at(i)), kTextSize, renderer.scale()).width /
             renderer.scale()) +
                28.0F);
        x -= button_width;
        buttons_.at(i) = {x, y, button_width, kButtonHeight};
        x -= kButtonGap;
    }
    for (std::size_t i = 0; i < kButtons; ++i) {
        const ui::Rect& button = buttons_.at(i);
        const bool main = i == kKeepBoth;
        const bool usable = i != kReplace || can_replace();
        ui::Color fill = main ? palette.primary : palette.surface_container_highest;
        if (hovered_ == i && usable) {
            fill = main ? palette.primary_container : palette.surface_bright;
        }
        renderer.draw_box(button, {.fill = fill, .radius = ui::Corners::all(8.0F)});
        ui::Color text = main ? palette.on_primary : palette.on_surface;
        if (!usable) {
            text = palette.outline;
        }
        renderer.draw_text(i18n::tr(kLabels.at(i)), button.x + 14.0F,
                           button.y + ((kButtonHeight - kTextSize) / 2.0F) - 2.0F, kTextSize, text,
                           button.width - 20.0F);
    }
    renderer.pop_clip();
}

}  // namespace nordend::views
