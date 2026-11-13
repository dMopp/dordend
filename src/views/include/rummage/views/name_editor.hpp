#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "rummage/fs/entry.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/edit_slot.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// Renaming in place (F2, and a new folder's name): a text field over the entry's name, with the
// name selected but for its extension. Enter confirms, Escape cancels, a click elsewhere confirms.
// A confirmed name that cannot be a name is refused at once and the field stays; a good one is
// handed to rename and the field waits, keys swallowed, until finished() says how it went: on
// success it closes, on failure it stays for another try.
//
// Loop thread only.
class NameEditor {
public:
    using Rename =
        std::function<void(std::shared_ptr<Folder> folder, std::string from, std::string to)>;

    // problem shows why a name was refused.
    NameEditor(std::function<void()> redraw, Rename rename,
               std::function<void(std::string problem)> problem);

    // Edits folder's entry called name; a directory's whole name is selected.
    void start(std::shared_ptr<Folder> folder, std::string name, bool directory);

    [[nodiscard]] bool active() const noexcept { return folder_ != nullptr; }

    // A rename is on its way.
    [[nodiscard]] bool busy() const noexcept { return busy_; }

    [[nodiscard]] const std::shared_ptr<Folder>& folder() const noexcept { return folder_; }

    [[nodiscard]] const ui::TextField& field() const noexcept { return field_; }

    // While active every key is the editor's: true.
    bool key(const ui::KeyEvent& event);

    // A press inside the field moves the caret (true); elsewhere it confirms and returns false,
    // so the press goes on to what is under it.
    bool pointer(const ui::PointerEvent& event);

    // Text pasted into the field (its first line, control characters left out).
    void paste(std::string_view text);

    void confirm();
    void cancel();

    // The rename's outcome.
    void finished(bool renamed);

    [[nodiscard]] EditSlot& slot() noexcept { return slot_; }

    // The field over the slot's rect, when the view placed it this frame.
    void draw(ui::Renderer& renderer, const theme::Palette& palette);

private:
    void close();

    std::function<void()> redraw_;
    Rename rename_;
    std::function<void(std::string)> problem_;
    std::shared_ptr<Folder> folder_;
    ui::TextField field_;
    EditSlot slot_;
    bool busy_{false};
};

}  // namespace rummage::views
