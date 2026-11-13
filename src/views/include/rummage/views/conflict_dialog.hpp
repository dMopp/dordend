#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

#include "rummage/fs/transfer.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// The question a copy or a move asks when a name is taken, on a card over the window: Replace
// (R), Skip (S), Keep both (Enter), "the same for the rest" (A), Escape stops the transfer.
// While it is open it takes every key and click.
//
// Loop thread only.
class ConflictDialog {
public:
    using Answer = std::function<void(fs::Resolution resolution, bool for_the_rest)>;

    explicit ConflictDialog(std::function<void()> redraw);

    // destination: the folder's name, for the title.
    void open(const fs::Conflict& conflict, std::string destination, Answer answer);

    // Closes without answering (the transfer was stopped elsewhere).
    void close();

    [[nodiscard]] bool is_open() const noexcept { return answer_ != nullptr; }

    [[nodiscard]] const fs::Conflict& conflict() const noexcept { return conflict_; }

    // A file and a folder, or two folders that are not merged (restoring): Replace is not
    // offered.
    [[nodiscard]] bool can_replace() const noexcept {
        return conflict_.directory == conflict_.target_directory &&
               (!conflict_.directory || conflict_.merge);
    }

    // Every key while open: true.
    bool key(const ui::KeyEvent& event);

    void pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // A button or the "for the rest" checkbox is there.
    [[nodiscard]] bool clickable_at(float x, float y) const {
        return button_at(x, y).has_value() || checkbox_.contains(x, y);
    }

private:
    // The buttons, left to right.
    static constexpr std::size_t kSkip = 0;
    static constexpr std::size_t kReplace = 1;
    static constexpr std::size_t kKeepBoth = 2;
    static constexpr std::size_t kButtons = 3;

    void choose(fs::Resolution resolution);
    void toggle_rest();
    [[nodiscard]] std::optional<std::size_t> button_at(float x, float y) const;

    std::function<void()> redraw_;
    fs::Conflict conflict_;
    std::string destination_;
    Answer answer_;
    bool for_the_rest_{false};
    std::array<ui::Rect, kButtons> buttons_{};  // as last drawn
    ui::Rect checkbox_{};
    std::optional<std::size_t> hovered_;
};

}  // namespace rummage::views
