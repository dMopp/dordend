#pragma once

#include <string_view>
#include <vector>

#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"

namespace rummage::fs {

// The icon for an entry, as a Tabler icon name ("folder", "photo", "aperture"...): the same
// line icons as Noctalia, tinted by the theme. Decided by, in order: the kind (folders, links,
// special files), darktable's .xmp sidecars, a few types worth their own icon (camera RAW, PDF,
// SVG, source code...), then the type's freedesktop generic icon. An empty file gets the icon
// of its name's type; an entry whose type is not known yet (content not sniffed) gets the plain
// file icon.
[[nodiscard]] std::string_view icon_name(const MimeDatabase& db, const Entry& entry);

// The same for a bare type, as for a regular file of that type.
[[nodiscard]] std::string_view icon_name(const MimeDatabase& db, MimeId mime);

// Every icon name the two functions above can return (to check them against the icon font).
[[nodiscard]] std::vector<std::string_view> icon_names_in_use();

}  // namespace rummage::fs
