#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rummage/fs/mime.hpp"
#include "rummage/platform/desktop_entry.hpp"

namespace rummage::platform {

// The [Default Applications], [Added Associations] and [Removed Associations] of mimeapps.list
// files, and the [MIME Cache] of mimeinfo.cache, by type: lists of desktop file IDs.
struct MimeApps {
    struct Lists {
        std::vector<std::string> defaults;
        std::vector<std::string> added;
        std::vector<std::string> removed;
        std::vector<std::string> cached;  // the entries that declare the type
    };

    std::unordered_map<std::string, Lists> types;

    // Adds a file's text below what is already there (a lower-priority file of the same dir).
    // With a database, types are filed under their canonical name (aliases resolved, as GLib
    // does): "image/x-targa" adds to "image/x-tga".
    void add(std::string_view text, const fs::MimeDatabase* database = nullptr);
};

// The installed applications and the user's choices of default applications, read the way
// GLib's GDesktopAppInfo reads them (freedesktop MIME Applications Associations 1.0.1 and Desktop
// Entry 1.5 specs), so rummage picks the same application that `gio open` would.
//
// The dirs, highest priority first: every config dir (only mimeapps.list), then every data dir's
// "applications" folder (mimeapps.list, the legacy defaults.list, mimeinfo.cache, desktop
// entries, subfolders giving IDs like "kde-okular.desktop"). In a dir, "<desktop>-mimeapps.list"
// for each $XDG_CURRENT_DESKTOP comes before mimeapps.list. A desktop ID found in a dir hides the
// same ID in lower dirs.
//
// Which entries declare a type comes from mimeinfo.cache (update-desktop-database writes it), not
// from the entries' MimeType keys: GLib 2.88 does the same (measured: an entry missing from the
// cache is not offered, a stale cache line still is).
//
// Immutable after load(): safe to share between threads. Loading reads a small file per
// installed application (21 entries in 0.5 ms on the development machine): cheap enough to do
// on every open, which keeps it current without watching anything.
class Applications {
public:
    // Blocking: run it on a worker.
    [[nodiscard]] static Applications load(const Environment& environment,
                                           const fs::MimeDatabase& database);

    // The default application for a type, in GLib's order: for the type, then each parent
    // (aliases resolved, subclasses breadth first), the defaults of every dir, then the added
    // associations and the cached entries declaring the type (minus the removed associations of
    // their dir and the dirs above); the first that is installed wins. nullptr when none is.
    [[nodiscard]] const DesktopEntry* default_for(std::string_view mime,
                                                  const fs::MimeDatabase& database) const;

    // The installed applications associated with a type, in GLib's order (what `gio mime TYPE`
    // lists as "Registered applications" with parents, "Recommended applications" without): the
    // added associations and the cached entries declaring the type, dir by dir, minus the
    // removed ones; then the same for each parent. Defaults are not added: callers put
    // default_for() first.
    [[nodiscard]] std::vector<const DesktopEntry*> all_for(std::string_view mime,
                                                           const fs::MimeDatabase& database,
                                                           bool parents = true) const;

    // The application with this desktop ID, if installed: found, not Hidden, an Application with
    // an Exec line whose program (and TryExec) is in the PATH. nullptr otherwise.
    [[nodiscard]] const DesktopEntry* find(std::string_view id) const;

    [[nodiscard]] std::size_t entry_count() const noexcept;

    // Every type that some mimeapps.list or mimeinfo.cache names, canonical (rummage-open's
    // check against gio).
    [[nodiscard]] std::vector<std::string> known_types() const;

private:
    struct Dir {
        MimeApps lists;
        std::unordered_map<std::string, DesktopEntry> entries;  // by desktop ID
    };

    // mime unaliased, then (with parents) its parents breadth first.
    [[nodiscard]] static std::vector<std::string> types_for(std::string_view mime,
                                                            const fs::MimeDatabase& database,
                                                            bool parents);
    // Adds the apps associated with type in every dir to hits, the removed ones to blocked.
    void associate(const std::string& type, std::vector<std::string>& hits,
                   std::vector<std::string>& blocked) const;

    // Is id defined in a dir above dirs_[index]?
    [[nodiscard]] bool masked(std::size_t index, const std::string& id) const;

    std::vector<Dir> dirs_;
    std::string path_;  // $PATH, for TryExec and Exec
};

}  // namespace rummage::platform
