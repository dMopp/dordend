#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "rummage/fs/entry.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// What a view shows at a window point: the folder shown there and the entry under it, if any
// (where a drag starts, where a drop goes). on_item: the point is on the entry's icon or name,
// where a press drags it; the rest of its row or cell is empty space, where a band starts.
struct ViewHit {
    std::shared_ptr<Folder> folder;
    std::optional<std::size_t> entry;
    bool on_item{false};
};

// Where a rubber band over a folder's entries runs, as last drawn: their area in the window (the
// band is cut to it) and how far it is scrolled. Points inside it are given to the views as
// content points: x from the area's left, y from the top of the whole list.
struct BandLane {
    ui::Rect area;
    float offset{0.0F};
};

// Where a drag over the window would drop, as the views draw it: the entry called name in
// directory (a folder), or without a name the view (or column) of directory itself.
struct DropMark {
    std::filesystem::path directory;  // empty: nowhere
    std::string name;

    [[nodiscard]] bool holds(const std::filesystem::path& folder, const fs::Entry& entry) const {
        return !name.empty() && folder == directory && entry.name == name;
    }

    // The whole view of folder is where it goes.
    [[nodiscard]] bool covers(const std::filesystem::path& folder) const {
        return name.empty() && !directory.empty() && folder == directory;
    }
};

}  // namespace rummage::views
