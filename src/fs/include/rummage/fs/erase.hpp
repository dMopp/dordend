#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer.hpp"

// Deleting for good: what Shift+Delete, Delete in the trash folder and "Empty Trash" do. Nothing
// goes to a trash and nothing can be undone. Blocking: plan_erase on a worker, run_erase on the
// transfer thread (TransferQueue does, for TransferKind::erase and empty_trash).
namespace rummage::fs {

// What a permanent delete removes, found before asking.
struct ErasePlan {
    // Each item, then the darktable sidecars that go with it.
    std::vector<std::filesystem::path> items;
    std::vector<std::string> names;  // the items as the user knows them (original names in a trash)
    std::size_t sidecars{0};         // of items, those not in the request (they go along unasked)
    std::size_t entries{0};          // files, links and folders removed, at every level
    std::uint64_t bytes{0};          // the files' sizes; a hard link counted once
    std::vector<std::string> errors;  // items not found: "P1.ORF: No such file or directory"
};

// What request removes:
// - erase: request.names in request.source, a regular file with its darktable sidecars
//   (sidecars_of); when request.source is the trash folder (trash_path()), request.names are
//   paths in trashes' files/ and the sidecars those Restore would take along (trashed_groups).
// - empty_trash: request.names are trashes (each with files/ and info/): all their items.
// A name picked along with its photo counts once. Cancellation stops the counting.
[[nodiscard]] ErasePlan plan_erase(const TransferRequest& request, const core::CancelToken& token);

// Removes what plan_erase finds: what a folder holds first, links never followed; a folder of the
// user's that cannot be written or entered is opened up (u+rwx) first, as it is going anyway. A
// photo that stays keeps its sidecars. A trashed item's .trashinfo goes once the item is gone, and
// its trash's directorysizes loses the folder's line; empty_trash also removes the .trashinfo
// files whose item is gone (orphans included). Cancellation (token) is checked between entries:
// what is gone is gone, a folder half emptied stays.
//
// The result: placed = the names of the items removed (as ErasePlan::names, sidecars left out),
// files_done = entries removed, bytes_done = their sizes; errors as run_transfer's.
[[nodiscard]] TransferResult run_erase(const TransferRequest& request,
                                       const core::CancelToken& token,
                                       const TransferCallbacks& callbacks);

}  // namespace rummage::fs
