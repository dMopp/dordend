#include "rummage/views/error_dialog.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace rummage::views {
namespace {
constexpr float kTextSize = 13;
constexpr float kLineHeight = 20;

bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xc0U) == 0x80U; }
}  // namespace

ErrorDialog::ErrorDialog(std::function<void()> redraw, std::function<void(std::string)> copy)
    : redraw_{std::move(redraw)}, copy_{std::move(copy)} {}

void ErrorDialog::push(std::string detail) {
    messages_.push_back(std::move(detail));
    redraw_();
}

void ErrorDialog::dismiss() {
    messages_.pop_front();
    lines_.clear();
    scroll_ = 0;
    maximum_scroll_ = 0;
    redraw_();
}

void ErrorDialog::scroll(float amount) {
    scroll_ = std::clamp(scroll_ + amount, 0.0F, maximum_scroll_);
    redraw_();
}

bool ErrorDialog::key(const ui::KeyEvent& event) {
    if (!is_open())
        return false;
    if (event.modifiers.ctrl && (event.keysym == XKB_KEY_c || event.keysym == XKB_KEY_C)) {
        if (copy_)
            copy_(detail());
    } else if (!event.modifiers.ctrl && !event.modifiers.alt) {
        switch (event.keysym) {
            case XKB_KEY_Return:
            case XKB_KEY_KP_Enter:
            case XKB_KEY_Escape:
                dismiss();
                break;
            case XKB_KEY_Down:
                scroll(kLineHeight);
                break;
            case XKB_KEY_Up:
                scroll(-kLineHeight);
                break;
            case XKB_KEY_Page_Down:
                scroll(page_height_);
                break;
            case XKB_KEY_Page_Up:
                scroll(-page_height_);
                break;
            case XKB_KEY_Home:
                scroll(-maximum_scroll_);
                break;
            case XKB_KEY_End:
                scroll(maximum_scroll_);
                break;
            default:
                break;
        }
    }
    return true;
}

void ErrorDialog::pointer(const ui::PointerEvent& event) {
    if (!is_open())
        return;
    if (event.kind == ui::PointerEvent::Kind::scroll)
        scroll(event.wheel_y != 0 ? event.wheel_y * 3 * kLineHeight : event.scroll_y);
    if (event.kind == ui::PointerEvent::Kind::press && event.button == ui::PointerButton::left) {
        if (close_button_.contains(event.x, event.y))
            dismiss();
        else if (copy_button_.contains(event.x, event.y) && copy_)
            copy_(detail());
    }
}

void ErrorDialog::wrap(ui::Renderer& renderer, float width) {
    if (!lines_.empty() && wrapped_width_ == width && wrapped_scale_ == renderer.scale())
        return;
    lines_.clear();
    wrapped_width_ = width;
    wrapped_scale_ = renderer.scale();
    std::string_view remaining = detail();
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        auto paragraph = remaining.substr(0, newline);
        if (paragraph.empty())
            lines_.emplace_back();
        while (!paragraph.empty()) {
            std::size_t low = 0;
            std::size_t high = std::min(paragraph.size(), std::size_t{512});
            while (low < high) {
                const auto middle = low + (high - low + 1) / 2;
                const float measured =
                    renderer.fonts()
                        .layout(paragraph.substr(0, middle), kTextSize, renderer.scale())
                        .width /
                    renderer.scale();
                if (measured <= width)
                    low = middle;
                else
                    high = middle - 1;
            }
            auto count = low;
            while (count > 0 && count < paragraph.size() && continuation(paragraph[count]))
                --count;
            if (count == 0) {
                count = 1;
                while (count < paragraph.size() && continuation(paragraph[count]))
                    ++count;
            }
            if (count < paragraph.size()) {
                const auto space = paragraph.rfind(' ', count);
                if (space != std::string_view::npos && space != 0)
                    count = space;
            }
            lines_.emplace_back(paragraph.substr(0, count));
            paragraph.remove_prefix(count);
            if (!paragraph.empty() && paragraph.front() == ' ')
                paragraph.remove_prefix(1);
        }
        if (newline == std::string_view::npos)
            break;
        remaining.remove_prefix(newline + 1);
    }
}

void ErrorDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                       const theme::Palette& palette) {
    if (!is_open())
        return;
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::max(100.0F, std::min(760.0F, bounds.width - 32));
    const float height = std::max(160.0F, std::min(520.0F, bounds.height - 32));
    const ui::Rect card{bounds.x + (bounds.width - width) / 2,
                        bounds.y + (bounds.height - height) / 2, width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = {},
                             .border_width = 1,
                             .border = palette.outline_variant});
    const auto title = pending() == 1 ? std::string{"Action failed"}
                                      : std::format("Action failed · {} reports", pending());
    renderer.draw_text(title, card.x + 20, card.y + 18, 16, palette.on_surface, width - 40);
    const ui::Rect content{card.x + 20, card.y + 50, width - 40, height - 124};
    wrap(renderer, content.width);
    page_height_ = content.height;
    maximum_scroll_ =
        std::max(0.0F, static_cast<float>(lines_.size()) * kLineHeight - content.height);
    scroll_ = std::min(scroll_, maximum_scroll_);
    renderer.push_clip(content);
    float y = content.y - scroll_;
    for (const auto& line : lines_) {
        if (y + kLineHeight >= content.y && y < content.bottom())
            renderer.draw_text(line, content.x, y, kTextSize, palette.on_surface);
        y += kLineHeight;
    }
    renderer.pop_clip();
    renderer.draw_text(
        maximum_scroll_ > 0 ? "Scroll for more · Ctrl+C copies details" : "Ctrl+C copies details",
        content.x, card.bottom() - 68, 12, palette.on_surface_variant, content.width);
    close_button_ = {card.right() - 128, card.bottom() - 48, 108, 32};
    copy_button_ = {close_button_.x - 144, close_button_.y, 136, 32};
    for (const bool copy : {true, false}) {
        const auto& button = copy ? copy_button_ : close_button_;
        renderer.draw_box(
            button,
            {.fill = copy ? palette.surface_container_highest : palette.primary, .radius = {}});
        renderer.draw_text(copy ? "Copy Details" : "Close   ↵", button.x + 12, button.y + 6, 13,
                           copy ? palette.on_surface : palette.on_primary, button.width - 24);
    }
}

}  // namespace rummage::views
