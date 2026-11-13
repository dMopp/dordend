#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "rummage/theme/palette.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"

namespace rummage::views {

// Action failures remain readable until dismissed. Concurrent failures are queued.
class ErrorDialog {
public:
    ErrorDialog(std::function<void()> redraw, std::function<void(std::string)> copy);
    void push(std::string detail);

    [[nodiscard]] bool is_open() const { return !messages_.empty(); }

    [[nodiscard]] const std::string& detail() const { return messages_.front(); }

    [[nodiscard]] std::size_t pending() const { return messages_.size(); }

    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    [[nodiscard]] bool clickable_at(float x, float y) const {
        return copy_button_.contains(x, y) || close_button_.contains(x, y);
    }

private:
    void dismiss();
    void scroll(float amount);
    void wrap(ui::Renderer& renderer, float width);
    std::function<void()> redraw_;
    std::function<void(std::string)> copy_;
    std::deque<std::string> messages_;
    std::vector<std::string> lines_;
    float wrapped_width_{0};
    float wrapped_scale_{0};
    float scroll_{0};
    float maximum_scroll_{0};
    float page_height_{0};
    ui::Rect copy_button_;
    ui::Rect close_button_;
};

}  // namespace rummage::views
