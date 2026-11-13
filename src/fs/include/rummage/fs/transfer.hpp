#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"

// Copying and moving files: the engine behind Ctrl+V. Blocking, long: run it on a thread of its
// own (TransferQueue does).
namespace rummage::fs {

enum class TransferKind : std::uint8_t {
    copy,
    move,
    trash,        // run_trash (trash.hpp): destination is the home trash
    restore,      // run_restore (trash.hpp): names are paths in a trash's files/
    erase,        // run_erase (erase.hpp): deleted for good; destination unused
    empty_trash,  // run_erase: names are trashes, everything in them goes
};

struct TransferRequest {
    TransferKind kind{TransferKind::copy};
    std::filesystem::path source;  // the folder the items are in
    std::vector<std::string> names;
    std::filesystem::path destination;  // the folder they go to
    // Tests: move by copying and deleting even on one file system (the path between disks).
    bool force_copy{false};
    // Tests: trash into this top directory's trash, as if the items were on another disk.
    std::filesystem::path trash_top{};  // NOLINT(readability-redundant-member-init): GCC wants one
                                        // for designated initializers that leave it out
};

// What to do with an item whose name is taken in the destination.
enum class Resolution : std::uint8_t {
    replace,    // the old one goes, once the new one is complete; folders are merged
    skip,       // this item stays where it is
    keep_both,  // the new one takes a free name: "P1 2.ORF", "Trip 2"
    cancel,     // stop here; what is done stays done
};

struct Conflict {
    std::string name;  // in the destination
    std::filesystem::path destination;
    bool directory{false};         // the item is a folder
    bool target_directory{false};  // the name taken is a folder's
    bool merge{true};              // folders may be merged (Replace); not when restoring
};

struct TransferProgress {
    bool preparing{false};  // still adding up the sizes
    std::uint64_t bytes_done{0};
    std::uint64_t bytes_total{0};  // 0 for a move on one disk: nothing is copied
    std::size_t files_done{0};
    std::size_t files_total{0};
    std::string current;  // the item being transferred
};

// An item a transfer placed: where it was and where it is now (undo needs both).
struct PlacedItem {
    std::filesystem::path from;
    std::filesystem::path to;

    bool operator==(const PlacedItem&) const = default;
};

struct TransferResult {
    // Set when nothing was done because the request cannot work: "Not enough space: ...",
    // "Cannot copy Trip into itself".
    std::string refused;
    std::vector<std::string> placed;  // the items' names in the destination, as given
    // The same items, sidecars left out (they follow their photo): copied or moved from where to
    // where; trashed from their place to the trash's files/; restored from files/ to their place.
    std::vector<PlacedItem> items;
    std::size_t files_done{0};
    std::uint64_t bytes_done{0};
    std::vector<std::string> errors;  // "Trip/P1.ORF: Permission denied"
    bool cancelled{false};
};

struct TransferCallbacks {
    std::function<void(const TransferProgress&)> progress;  // often: throttle it
    // Asked for every taken name, on the transfer's thread; it may block until the user answers.
    std::function<Resolution(const Conflict&)> resolve;
};

// Copies or moves request.names from request.source into request.destination.
//
// - A move on one file system is a rename (instant); a copy, or a move between file systems,
//   copies the data (copy_file_range, else read/write) in pieces into a hidden temporary file
//   (".rummage-part-XXXXXX") renamed to its name only when complete, so a cancelled or failed
//   copy leaves nothing half-written under a real name. A new folder is built the same way, as a
//   whole. Permissions, access and modification times and symbolic links are kept.
// - Between file systems a source is deleted only after its copy is complete and flushed to disk
//   (fsync), and a folder only if all of it arrived.
// - A regular file takes its darktable sidecars along (sidecars_of), named after its new name;
//   a conflict on any of them is a conflict on the file.
// - Taken names go to callbacks.resolve, except a copy into its own folder: always keep_both.
//   A file never replaces a folder, nor a folder a file.
// - Refused before anything is done: a folder into itself or below, or a copy bigger than the
//   free space. A move into the same folder does nothing.
// - Cancellation (token) is checked between pieces of a file and between files.
[[nodiscard]] TransferResult run_transfer(const TransferRequest& request,
                                          const core::CancelToken& token,
                                          const TransferCallbacks& callbacks);

// The first free name in directory for name: "P1 2.ORF", "P1 3.ORF"... (a folder: "Trip 2"),
// with the names its sidecars would take free too.
[[nodiscard]] std::string free_name(const std::filesystem::path& directory, const std::string& name,
                                    bool folder, const std::vector<std::string>& sidecars);

}  // namespace rummage::fs
