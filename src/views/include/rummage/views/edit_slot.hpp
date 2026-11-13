#pragma once

#include <optional>
#include <string>

#include "rummage/fs/entry.hpp"
#include "rummage/ui/geometry.hpp"

namespace rummage::views {

class Folder;

// Where the name being edited is on screen. The editor says which entry; the view that shows it
// notes where it drew the name (and leaves the name out), and the editor draws its field there.
struct EditSlot {
    const Folder* folder{nullptr};  // nullptr: nothing is being edited
    std::string name;
    std::optional<ui::Rect> rect;  // from the last draw; nullopt: not on screen
    float text_size{14.0F};

    [[nodiscard]] bool holds(const Folder& in, const fs::Entry& entry) const {
        return folder == &in && entry.name == name;
    }

    // The view drew entry's name here: rect is the room for it (the field's box).
    void place(const ui::Rect& room, float size) {
        rect = room;
        text_size = size;
    }
};

}  // namespace rummage::views
