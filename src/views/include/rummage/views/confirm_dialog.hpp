#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

// The question before something that cannot be undone (deleting for good), on a card over the
// window: Enter does it, Escape gives up; so do its two buttons. While it is open it takes every
// key and click.
//
// Loop thread only.
class ConfirmDialog {
public:
    // confirmed: Enter or the action's button; false: Escape or Cancel.
    using Answer = std::function<void(bool confirmed)>;

    explicit ConfirmDialog(std::function<void()> redraw);

    // title: "Delete “P1.ORF” permanently?"; detail: the line under it ("Adding up the size…",
    // changed later by set_detail); action: the button that does it ("Delete").
    void open(std::string title, std::string detail, std::string action, Answer answer,
              std::string warning = "Nothing goes to the trash: this cannot be undone.");

    // A separate instance can be used for archive passwords without replacing a confirmation.
    void open_password(std::string name, bool retry,
                       std::function<void(std::optional<std::string>)> answer);
    void paste(std::string_view text);

    [[nodiscard]] std::size_t generation() const { return generation_; }

    void set_detail(std::string detail);

    // Closes without answering.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return answer_ != nullptr; }

    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    void pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // One of its buttons is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const {
        return std::ranges::any_of(
            buttons_, [x, y](const ui::Rect& button) { return button.contains(x, y); });
    }

private:
    // The buttons, left to right.
    static constexpr std::size_t kCancel = 0;
    static constexpr std::size_t kAction = 1;
    static constexpr std::size_t kButtons = 2;

    void choose(bool confirmed);

    std::function<void()> redraw_;
    std::string title_;
    std::string detail_;
    std::string action_;
    std::string warning_;
    Answer answer_;
    bool password_mode_{false};
    ui::TextField password_field_;
    std::size_t generation_{0};
    std::array<ui::Rect, kButtons> buttons_{};  // as last drawn
    std::optional<std::size_t> hovered_;
};

}  // namespace rummage::views
