#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "rummage/fs/places.hpp"

namespace rummage::fs {

// KDE's [Desktop Entry] Icon from a .directory file. Relative ./ paths are resolved
// against the folder; an empty result means no override.
[[nodiscard]] std::string parse_folder_icon(std::string_view text,
                                            const std::filesystem::path& folder);

struct FolderIconSetting {
    std::string icon;
    std::string error;
};

// Workers only. Preserve unrelated .directory content; empty icon removes the override.
// Refuse symlink/non-regular metadata and oversized files rather than overwriting them.
[[nodiscard]] FolderIconSetting read_folder_icon_setting(const std::filesystem::path& folder);
[[nodiscard]] std::string save_folder_icon(const std::filesystem::path& folder,
                                           std::string_view icon);
[[nodiscard]] std::string update_folder_icon_text(std::string_view text, std::string_view icon);

// Blocking: construct once per listing on a worker, then resolve each directory.
// Explicit .directory icons take precedence over XDG special-folder icons.
class FolderIcons {
public:
    explicit FolderIcons(const PlaceSources& sources = PlaceSources::from_environment());
    [[nodiscard]] std::string for_folder(const std::filesystem::path& folder) const;
    [[nodiscard]] std::string default_for_folder(const std::filesystem::path& folder) const;

private:
    std::vector<std::pair<std::filesystem::path, std::string>> standard_;
};

}  // namespace rummage::fs
