#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

#include "rummage/fs/entry.hpp"

namespace rummage::views {

// The entries cut with Ctrl+X and not pasted yet: the views draw them faded.
struct CutMarks {
    std::filesystem::path directory;  // empty: nothing is cut
    std::unordered_set<std::string> names;

    [[nodiscard]] bool holds(const std::filesystem::path& folder, const fs::Entry& entry) const {
        return !names.empty() && folder == directory && names.contains(entry.name);
    }
};

}  // namespace rummage::views
