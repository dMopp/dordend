#include "rummage/views/name_editor.hpp"

#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "rummage/fs/operations.hpp"

namespace rummage::views {

NameEditor::NameEditor(std::function<void()> redraw, Rename rename,
                       std::function<void(std::string problem)> problem)
    : redraw_{std::move(redraw)}, rename_{std::move(rename)}, problem_{std::move(problem)} {}

void NameEditor::start(std::shared_ptr<Folder> folder, std::string name, bool directory) {
    folder_ = std::move(folder);
    busy_ = false;
    slot_ = EditSlot{.folder = folder_.get(), .name = name, .rect = std::nullopt};
    // The name without its extension, as the part one usually changes; a folder's whole name.
    std::size_t end = name.size();
    if (const std::size_t dot = name.rfind('.');
        !directory && dot != std::string::npos && dot > 0) {
        end = dot;
    }
    field_.set_text(std::move(name));
    field_.select(0, end);
    redraw_();
}

bool NameEditor::key(const ui::KeyEvent& event) {
    if (!active()) {
        return false;
    }
    if (busy_) {
        return true;
    }
    const bool plain = !event.modifiers.ctrl && !event.modifiers.alt;
    if (plain && (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter)) {
        confirm();
    } else if (plain && event.keysym == XKB_KEY_Escape) {
        cancel();
    } else if (field_.key(event)) {
        redraw_();
    }
    return true;
}

bool NameEditor::pointer(const ui::PointerEvent& event) {
    if (!active() || event.kind != ui::PointerEvent::Kind::press) {
        return false;
    }
    const ui::Rect& box = field_.bounds();
    const bool inside = slot_.rect && event.x >= box.x && event.x < box.right() &&
                        event.y >= box.y && event.y < box.bottom();
    if (inside && event.button == ui::PointerButton::left) {
        if (!busy_) {
            if (event.click_count >= 2) {
                field_.select_all();
            } else {
                field_.click(event.x, event.modifiers.shift);
            }
            redraw_();
        }
        return true;
    }
    if (!busy_) {
        confirm();
    }
    return false;
}

void NameEditor::paste(std::string_view text) {
    if (!active() || busy_) {
        return;
    }
    std::string line;
    for (const char c : text.substr(0, text.find_first_of("\r\n"))) {
        if (static_cast<unsigned char>(c) >= 0x20U && c != 0x7F) {
            line.push_back(c);
        }
    }
    field_.insert(line);
    redraw_();
}

void NameEditor::confirm() {
    if (!active() || busy_) {
        return;
    }
    const std::string& name = field_.text();
    if (name == slot_.name) {
        close();
        return;
    }
    if (const std::optional<std::string> problem = fs::name_problem(name)) {
        problem_(*problem);
        return;
    }
    busy_ = true;
    rename_(folder_, slot_.name, name);
}

void NameEditor::cancel() {
    if (active() && !busy_) {
        close();
    }
}

void NameEditor::finished(bool renamed) {
    if (!busy_) {
        return;
    }
    busy_ = false;
    if (renamed) {
        close();
    } else {
        redraw_();
    }
}

void NameEditor::close() {
    folder_.reset();
    slot_ = EditSlot{};
    busy_ = false;
    redraw_();
}

void NameEditor::draw(ui::Renderer& renderer, const theme::Palette& palette) {
    if (!active() || !slot_.rect) {
        return;
    }
    field_.draw(renderer, *slot_.rect, slot_.text_size,
                {.background = palette.surface_container_highest,
                 .border = busy_ ? palette.outline : palette.primary,
                 .text = palette.on_surface,
                 .selection = palette.secondary_container,
                 .caret = busy_ ? palette.surface_container_highest : palette.on_surface});
}

}  // namespace rummage::views
