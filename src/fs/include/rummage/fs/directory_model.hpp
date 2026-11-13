#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/sort.hpp"
#include "rummage/fs/trash.hpp"

namespace rummage::fs {

// What changed in a DirectoryModel since the last notification. Names, not indices: a view
// looks them up in the listing (and keeps its selection across renames).
struct ModelChange {
    bool reset{false};   // the listing was replaced (open, reload after an overflow, error)
    bool loaded{false};  // the initial load just finished
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> updated;                          // size, date or type changed
    std::vector<std::pair<std::string, std::string>> renamed;  // old name, new name
};

// One directory as the views see it: a sorted listing, loaded in chunks on workers and kept up
// to date by inotify while open.
//
// The watch starts before the read, so nothing that happens during the load is lost. Events are
// gathered for settle, then the changed names are stat'ed together on a worker and applied in
// one notification: a burst (copying a thousand files) is a few updates, not a thousand, and the
// loop thread does no I/O. Renames inside the directory are reported as renames.
//
// The trash folder (trash_path()) is listed the same way from every trash (list_trash): entries
// by original name, each one's real place and .trashinfo in trashed(); the trashes' info/
// folders are watched and the whole listing is read again when they change.
//
// Everything runs on the loop thread. Must not outlive the loop, pool or watcher.
class DirectoryModel {
public:
    using ChangeHandler = std::function<void(const ModelChange& change)>;

    static constexpr std::chrono::milliseconds kDefaultSettle{30};

    DirectoryModel(core::EventLoop& loop, core::WorkerPool& pool, DirectoryWatcher& watcher,
                   std::shared_ptr<const MimeDatabase> mime,
                   std::chrono::milliseconds settle = kDefaultSettle);
    ~DirectoryModel();

    DirectoryModel(const DirectoryModel&) = delete;
    DirectoryModel& operator=(const DirectoryModel&) = delete;
    DirectoryModel(DirectoryModel&&) = delete;
    DirectoryModel& operator=(DirectoryModel&&) = delete;

    // Shows another directory: clears the listing (a reset change), watches and loads it.
    void open(std::filesystem::path path);

    void set_change_handler(ChangeHandler handler) { change_handler_ = std::move(handler); }

    void set_order(SortOrder order);

    // Shows or leaves out the hidden entries, keep shown either way (Listing::set_filter): what
    // leaves the listing is notified as removed, what comes back as added. Entries left out are
    // still watched and kept up to date.
    void set_filter(bool show_hidden, std::string keep);
    void set_predicate(Listing::Predicate predicate);
    // Reread metadata changed inside a child folder, beyond this directory's inotify watch.
    void refresh_entry(const std::string& name);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] const Listing& listing() const noexcept { return listing_; }

    [[nodiscard]] bool loading() const noexcept { return loading_; }

    // Counts the changes notified: a new value means the listing changed (or was replaced).
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    // In the trash folder: the item called name (its real path, original place, deletion date);
    // nullptr elsewhere or for an unknown name.
    [[nodiscard]] const TrashedItem* trashed(const std::string& name) const;

    [[nodiscard]] bool is_trash() const noexcept { return trash_; }

    // Why the directory cannot be shown (cannot be read, was deleted or moved); empty when fine.
    [[nodiscard]] const std::error_code& error() const noexcept { return error_; }

private:
    void start_load();
    void stop();
    // The trash folder: read every trash on a worker, then apply the differences.
    struct TrashListing;
    void load_trash();
    void apply_trash(TrashListing listing);
    void on_events(const std::vector<DirectoryEvent>& events);
    void schedule_refresh();
    void refresh();
    void apply(const std::vector<std::string>& names, std::vector<std::optional<Entry>> entries,
               const std::vector<std::pair<std::string, std::string>>& renames);
    void notify(const ModelChange& change);
    void fail(std::error_code error);

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    DirectoryWatcher& watcher_;
    ReadOptions options_;
    std::chrono::milliseconds settle_;
    ChangeHandler change_handler_;

    std::filesystem::path path_;
    Listing listing_;
    std::unordered_set<std::string> names_;  // what is in the listing
    std::error_code error_;
    bool loading_{false};
    std::uint64_t revision_{0};

    DirectoryWatcher::WatchId watch_{0};
    core::CancelToken load_;
    core::CancelToken refresh_job_;
    bool refresh_running_{false};
    core::EventLoop::SourceId settle_timer_{0};

    // Changes seen and not applied yet.
    std::unordered_set<std::string> dirty_;
    std::unordered_map<std::uint32_t, std::string> moved_from_;  // cookie -> old name
    std::vector<std::pair<std::string, std::string>> renames_;

    bool trash_{false};
    std::unordered_map<std::string, TrashedItem> trashed_;  // by name, as listed
    std::unordered_map<std::string, DirectoryWatcher::WatchId> trash_watches_;  // by info/ dir
    bool trash_again_{false};  // changed while being read: read once more
};

}  // namespace rummage::fs
