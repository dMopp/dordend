#include "rummage/fs/directory_watcher.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::fs {
namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                     IN_CLOSE_WRITE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF |
                                     IN_ONLYDIR | IN_EXCL_UNLINK;

}  // namespace

DirectoryWatcher::DirectoryWatcher(core::EventLoop& loop)
    : loop_{loop}, fd_{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)} {
    if (fd_ < 0) {
        throw std::system_error{errno, std::generic_category(), "inotify_init1"};
    }
    source_ = loop_.add_fd(fd_, POLLIN, [this](short /*revents*/) { on_readable(); });
}

DirectoryWatcher::~DirectoryWatcher() {
    loop_.remove(source_);
    close(fd_);  // drops every watch
}

DirectoryWatcher::WatchId DirectoryWatcher::watch(const std::filesystem::path& directory,
                                                  Handler handler, std::error_code& error) {
    const int descriptor = inotify_add_watch(fd_, directory.c_str(), kWatchMask);
    if (descriptor < 0) {
        error = {errno, std::generic_category()};
        return 0;
    }
    error.clear();
    const WatchId id = next_id_++;
    by_descriptor_[descriptor].push_back({id, std::move(handler)});
    descriptor_of_[id] = descriptor;
    return id;
}

void DirectoryWatcher::unwatch(WatchId id) {
    const auto it = descriptor_of_.find(id);
    if (it == descriptor_of_.end()) {
        return;
    }
    const int descriptor = it->second;
    descriptor_of_.erase(it);
    auto& subscribers = by_descriptor_[descriptor];
    std::erase_if(subscribers, [id](const Subscriber& s) { return s.id == id; });
    if (subscribers.empty()) {
        by_descriptor_.erase(descriptor);
        inotify_rm_watch(fd_, descriptor);  // may fail if the kernel dropped it already: fine
    }
}

void DirectoryWatcher::on_readable() {
    std::unordered_map<int, std::vector<DirectoryEvent>> batches;
    std::vector<int> order;  // descriptors in first-event order
    bool overflow = false;
    alignas(inotify_event) std::array<char, 16384> buffer{};
    for (;;) {
        const ssize_t length = read(fd_, buffer.data(), buffer.size());
        if (length <= 0) {
            if (length < 0 && errno != EAGAIN && errno != EINTR) {
                log::warn("directory watcher: read failed: {}",
                          std::error_code{errno, std::generic_category()}.message());
            }
            break;
        }
        for (std::size_t offset = 0; std::cmp_less(offset, length);) {
            inotify_event event{};
            std::memcpy(&event, buffer.data() + offset, sizeof event);
            const char* name_bytes = buffer.data() + offset + sizeof event;
            offset += sizeof event + event.len;

            if ((event.mask & IN_Q_OVERFLOW) != 0U) {
                overflow = true;
                continue;
            }
            using Kind = DirectoryEvent::Kind;
            Kind kind{};
            if ((event.mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0U) {
                kind = Kind::directory_gone;
            } else if ((event.mask & IN_CREATE) != 0U) {
                kind = Kind::created;
            } else if ((event.mask & IN_DELETE) != 0U) {
                kind = Kind::deleted;
            } else if ((event.mask & IN_MOVED_FROM) != 0U) {
                kind = Kind::moved_from;
            } else if ((event.mask & IN_MOVED_TO) != 0U) {
                kind = Kind::moved_to;
            } else if ((event.mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0U) {
                kind = Kind::changed;
            } else {
                continue;  // IN_IGNORED and anything else
            }
            if (!by_descriptor_.contains(event.wd)) {
                continue;  // unwatched meanwhile
            }
            if (!batches.contains(event.wd)) {
                order.push_back(event.wd);
            }
            batches[event.wd].push_back(
                {.kind = kind,
                 .name = event.len > 0 ? std::string{name_bytes} : std::string{},
                 .cookie = event.cookie});
        }
    }
    if (overflow) {
        log::warn("directory watcher: inotify queue overflow, directories will be re-read");
        for (const auto& [descriptor, subscribers] : by_descriptor_) {
            if (!batches.contains(descriptor)) {
                order.push_back(descriptor);
            }
            batches[descriptor].push_back({.kind = DirectoryEvent::Kind::overflow, .name = {}});
        }
    }
    for (const int descriptor : order) {
        // Copy: a handler may unwatch (itself or others) while we iterate.
        const auto it = by_descriptor_.find(descriptor);
        if (it == by_descriptor_.end()) {
            continue;
        }
        const std::vector<Subscriber> subscribers = it->second;
        for (const Subscriber& subscriber : subscribers) {
            if (descriptor_of_.contains(subscriber.id)) {
                subscriber.handler(batches[descriptor]);
            }
        }
    }
}

}  // namespace rummage::fs
