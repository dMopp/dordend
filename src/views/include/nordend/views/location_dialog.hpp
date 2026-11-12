#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "nordend/theme/palette.hpp"
#include "nordend/ui/geometry.hpp"
#include "nordend/ui/renderer.hpp"
#include "nordend/ui/seat.hpp"
#include "nordend/ui/text_field.hpp"

namespace nordend::views {

// "Go to Folder or Server" (Ctrl+L), on a card over the window: one line for a path or a server's
// address, and whether a server goes into the sidebar too. Enter goes, Escape gives up; so do its
// buttons. A wrong address keeps the card open with what is wrong under the line. While it is open
// it takes every key and click.
//
// Loop thread only.
class LocationDialog {
public:
    // text as typed, and whether to add a server to the sidebar. Returns what is wrong with it
    // (the card stays open and shows it), or empty: it went there and the card closes.
    using Go = std::function<std::string(const std::string& text, bool add)>;

    explicit LocationDialog(std::function<void()> redraw);

    // text: where the field starts (the folder shown), all of it selected.
    void open(std::string text, Go go);

    void close();

    [[nodiscard]] bool is_open() const noexcept { return go_ != nullptr; }

    [[nodiscard]] const std::string& text() const noexcept { return field_.text(); }

    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    [[nodiscard]] bool add() const noexcept { return add_; }

    // Every key while open: true. Tab moves between the line and "Add to the sidebar".
    bool key(const ui::KeyEvent& event);

    // Ctrl+V: text into the line (its first line).
    void paste(std::string_view text);

    void pointer(const ui::PointerEvent& event);

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette);

    // A button or the toggle is there, as last drawn.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    // The line is there, as last drawn.
    [[nodiscard]] bool field_at(float x, float y) const { return field_.bounds().contains(x, y); }

private:
    enum class Target : std::uint8_t { none, add, cancel, go };

    [[nodiscard]] Target target_at(float x, float y) const;
    void submit();

    std::function<void()> redraw_;
    Go go_;
    ui::TextField field_;
    std::string error_;
    bool add_{true};
    bool on_toggle_{false};  // the keys go to the toggle, not the line
    ui::Rect toggle_{};      // as last drawn
    ui::Rect cancel_{};
    ui::Rect go_button_{};
    Target hovered_{Target::none};
};

}  // namespace nordend::views
