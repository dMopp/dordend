#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "rummage/core/event_loop.hpp"

namespace rummage::fs {

struct DirectoryEvent {
    enum class Kind : std::uint8_t {
        created,         // a name appeared (new file, link, directory)
        deleted,         // a name went away
        moved_from,      // renamed away; a moved_to with the same cookie may follow
        moved_to,        // renamed in; cookie pairs it with its moved_from
        changed,         // content written and closed, or metadata changed
        directory_gone,  // the watched directory was deleted, moved or unmounted
        overflow,        // the kernel queue overflowed: events were lost, re-read everything
    };

    Kind kind;
    std::string name;  // inside the watched directory; empty for directory_gone and overflow
    std::uint32_t cookie{0};
};

// Watches directories through one inotify instance shared by all of them (the kernel limits
// instances per user, and Miller columns open several directories). Events reach each
// directory's handler on the loop thread, in kernel order, one batch per read.
//
// Must not outlive the loop. Not copyable or movable: the loop callback keeps a pointer to it.
class DirectoryWatcher {
public:
    using Handler = std::function<void(const std::vector<DirectoryEvent>& events)>;
    using WatchId = std::uint64_t;

    explicit DirectoryWatcher(core::EventLoop& loop);
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;
    DirectoryWatcher(DirectoryWatcher&&) = delete;
    DirectoryWatcher& operator=(DirectoryWatcher&&) = delete;

    // Starts watching a directory; 0 and error set on failure (ENOENT, EACCES, ENOSPC when the
    // per-user watch limit is reached...). The same directory may be watched more than once.
    WatchId watch(const std::filesystem::path& directory, Handler handler, std::error_code& error);

    // Stops one watch; unknown ids are ignored. Safe from inside a handler.
    void unwatch(WatchId id);

private:
    struct Subscriber {
        WatchId id;
        Handler handler;
    };

    void on_readable();

    core::EventLoop& loop_;
    int fd_{-1};
    core::EventLoop::SourceId source_{0};
    WatchId next_id_{1};
    // Kernel watch descriptor -> its subscribers (inotify returns the same descriptor when one
    // directory is watched twice).
    std::unordered_map<int, std::vector<Subscriber>> by_descriptor_;
    std::unordered_map<WatchId, int> descriptor_of_;
};

}  // namespace rummage::fs
