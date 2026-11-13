#pragma once

#include <ctime>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer.hpp"

// The trash, as the freedesktop Trash specification 1.0 has it: what Delete does. Blocking: run
// it on the transfer thread (TransferQueue does, for TransferKind::trash).
namespace rummage::fs {

// The home trash: $XDG_DATA_HOME/Trash, else ~/.local/share/Trash; empty without a home.
[[nodiscard]] std::filesystem::path home_trash();

// The top directory of path's file system: its highest ancestor on the same device (the mount
// point). path must exist; it is made absolute and its links resolved first.
[[nodiscard]] std::filesystem::path mount_top(const std::filesystem::path& path);

struct TrashDir {
    std::filesystem::path path;  // holds files/ and info/
    // Paths in its .trashinfo files are relative to this; empty for the home trash (absolute).
    std::filesystem::path relative_to;
};

// The user's trash in topdir (a mount point): topdir/.Trash/uid when topdir/.Trash is a real
// folder (not a link) with the sticky bit, else topdir/.Trash-uid; made when missing, refused
// when it is not a folder owned by uid. Its files/ and info/ are made too. nullopt (and error) when
// none can be used.
[[nodiscard]] std::optional<TrashDir> top_trash(const std::filesystem::path& topdir, uid_t uid,
                                                std::error_code& error);

// "photo.ORF" as RFC 2396 escapes it, '/' kept when keep_slash: what .trashinfo files and
// directorysizes hold (GLib's g_uri_escape_string).
[[nodiscard]] std::string escape_path(std::string_view path, bool keep_slash);

// The contents of a .trashinfo file: original (absolute, or relative to the trash's top
// directory), deleted at when in local time.
[[nodiscard]] std::string trash_info(const std::filesystem::path& original, std::time_t when);

// The name the id-th copy of name takes in a trash, as GLib numbers them: "P1.ORF", "P1.2.ORF",
// "P1.3.ORF"; "Trip", "Trip.2".
[[nodiscard]] std::string trash_name(std::string_view name, int id);

// The trash as a folder the views show ("trash:///", not a real path): every trash merged, the
// home trash and those of the disks (DirectoryModel lists it).
[[nodiscard]] const std::filesystem::path& trash_path();
[[nodiscard]] bool is_trash(const std::filesystem::path& path);

// Where the trashes are looked for.
struct TrashSources {
    std::filesystem::path home;  // the home trash
    // The top directories whose .Trash/uid and .Trash-uid count; nullopt: every mount point
    // in /proc/self/mountinfo.
    std::optional<std::vector<std::filesystem::path>> tops;
    uid_t uid{0};

    // home_trash(), every mount point, geteuid().
    [[nodiscard]] static TrashSources from_environment();
};

// The sources the trash folder reads (loop thread; tests set their own). Until set:
// TrashSources::from_environment(), taken on first use.
void set_trash_sources(TrashSources sources);
[[nodiscard]] const TrashSources& trash_sources();

// Every mount point in /proc/self/mountinfo's text (its fifth field), octal escapes decoded
// ("\040" is a space).
[[nodiscard]] std::vector<std::filesystem::path> mount_points(std::string_view mountinfo);

// The trashes of sources that exist (home first), as TrashDir: files/ and info/ inside.
[[nodiscard]] std::vector<TrashDir> existing_trashes(const TrashSources& sources);

// What a .trashinfo file says: the original path (absolute: a relative one is joined to
// relative_to) and the deletion date as written ("2026-10-29T21:25:31"). nullopt when it is not
// one ([Trash Info] first, a Path).
struct TrashInfo {
    std::filesystem::path original;
    std::string deleted;
};

[[nodiscard]] std::optional<TrashInfo> parse_trash_info(std::string_view text,
                                                        const std::filesystem::path& relative_to);

// One item in a trash.
struct TrashedItem {
    // As the trash folder shows it: the original name; when several items have the same one,
    // the older keeps it and the others are numbered as trash_name() does ("P1.2.ORF").
    std::string name;
    std::filesystem::path path;  // in files/
    TrashInfo info;
};

// Every item of every trash in sources with its .trashinfo (an item without one is left out, as
// trash programs do), named as above. Blocking: run it on a worker.
[[nodiscard]] std::vector<TrashedItem> list_trash(const TrashSources& sources);

// Puts trashed items back (TransferKind::restore): request.names are their paths in files/.
// Each goes to its original path, the folder made again if it is gone; a taken name goes to
// callbacks.resolve (replace: a file over a file; a folder is never merged, Replace is not
// offered), then its .trashinfo is removed. A photo takes along its darktable sidecars that are in
// the same trash from the same folder, named after it. Never copied between disks.
//
// The result: placed = the original names as restored, files_done = items, sidecars included.
[[nodiscard]] TransferResult run_restore(const TransferRequest& request,
                                         const core::CancelToken& token,
                                         const TransferCallbacks& callbacks);

// A trashed item as a permanent delete finds it: its .trashinfo and the sidecars Restore would
// take along with it (paths in files/).
struct TrashedGroup {
    std::filesystem::path item;  // in files/
    TrashInfo info;
    std::vector<std::filesystem::path> sidecars;
};

// items (paths in trashes' files/) with their info and sidecars, each trash read once; nullopt
// for an item that is not in a trash any more (or has no .trashinfo).
[[nodiscard]] std::vector<std::optional<TrashedGroup>> trashed_groups(
    std::span<const std::filesystem::path> items);

// The .trashinfo of an item in a trash's files/.
[[nodiscard]] std::filesystem::path info_file_of(const std::filesystem::path& item);

// Drops the lines of trash's directorysizes whose folder is gone from files/ (the file goes
// when none is left).
void prune_directory_sizes(const std::filesystem::path& trash);

// Moves request.names from request.source to the trash; request.destination is the home trash
// (home_trash(), or a test's). Each item goes to the trash of its file system: the home trash on
// the home trash's one, else its top directory's (top_trash); never copied between file systems:
// an item that has no trash there stays and is an error. A regular file takes its darktable
// sidecars along, each with its own .trashinfo. Folders are listed in directorysizes. The trash
// and what it holds are not trashed again.
//
// The result: placed = the names trashed (not their sidecars), files_done = items trashed,
// sidecars included; errors as run_transfer's.
[[nodiscard]] TransferResult run_trash(const TransferRequest& request,
                                       const core::CancelToken& token,
                                       const TransferCallbacks& callbacks);

}  // namespace rummage::fs
