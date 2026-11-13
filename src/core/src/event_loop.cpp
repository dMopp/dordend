#include "rummage/core/event_loop.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::core {
namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::runtime_error{std::string{what} + ": " + std::strerror(errno)};
}

timespec to_timespec(std::chrono::milliseconds ms) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(ms);
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(ms - seconds);
    return {.tv_sec = seconds.count(), .tv_nsec = nanoseconds.count()};
}

}  // namespace

struct EventLoop::Source {
    SourceId id;
    int fd;
    short events;
    FdCallback callback;
    FdOptions options;
    bool owns_fd;  // timers own their timerfd
    bool removed;
};

EventLoop::EventLoop() : wake_fd_{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)} {
    if (wake_fd_ < 0) {
        throw_errno("eventfd");
    }
    add_fd(wake_fd_, POLLIN, [this](short /*revents*/) {
        std::uint64_t count = 0;
        while (read(wake_fd_, &count, sizeof(count)) == sizeof(count)) {
        }
        run_posted();
    });
}

EventLoop::~EventLoop() {
    for (const Source& source : sources_) {
        if (source.owns_fd && !source.removed) {
            close(source.fd);
        }
    }
    close(wake_fd_);
}

EventLoop::SourceId EventLoop::add_fd(int fd, short events, FdCallback callback,
                                      FdOptions options) {
    const SourceId id = next_id_++;
    sources_.push_back({.id = id,
                        .fd = fd,
                        .events = events,
                        .callback = std::move(callback),
                        .options = std::move(options),
                        .owns_fd = false,
                        .removed = false});
    return id;
}

EventLoop::SourceId EventLoop::add_timer(std::chrono::milliseconds delay,
                                         std::chrono::milliseconds interval,
                                         std::function<void()> callback) {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        throw_errno("timerfd_create");
    }
    // A zero it_value disarms a timer: fire "now" as 1 ns instead.
    itimerspec spec{.it_interval = to_timespec(interval), .it_value = to_timespec(delay)};
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1;
    }
    timerfd_settime(fd, 0, &spec, nullptr);

    const SourceId id = next_id_++;
    const bool once = interval.count() == 0;
    sources_.push_back(
        {.id = id,
         .fd = fd,
         .events = POLLIN,
         .callback =
             [this, fd, id, once, callback = std::move(callback)](short /*revents*/) {
                 std::uint64_t expirations = 0;
                 if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
                     return;
                 }
                 if (once) {
                     remove(id);
                 }
                 callback();
             },
         .options = {},
         .owns_fd = true,
         .removed = false});
    return id;
}

void EventLoop::remove(SourceId id) {
    for (Source& source : sources_) {
        if (source.id == id && !source.removed) {
            source.removed = true;  // erased after the current dispatch
            if (source.owns_fd) {
                close(source.fd);
            }
            return;
        }
    }
}

void EventLoop::post(std::function<void()> fn) {
    {
        const std::scoped_lock lock{posted_mutex_};
        posted_.push_back(std::move(fn));
    }
    const std::uint64_t one = 1;
    // Can only fail if the counter would overflow, and then the loop is awake anyway.
    [[maybe_unused]] const auto written = write(wake_fd_, &one, sizeof(one));
}

void EventLoop::run_posted() {
    std::vector<std::function<void()>> batch;
    {
        const std::scoped_lock lock{posted_mutex_};
        batch.swap(posted_);
    }
    for (auto& fn : batch) {
        fn();
    }
}

int EventLoop::run() {
    running_ = true;
    std::vector<pollfd> fds;
    std::vector<SourceId> ids;
    while (running_) {
        std::erase_if(sources_, [](const Source& source) { return source.removed; });

        for (const Source& source : sources_) {
            if (source.options.before_poll) {
                source.options.before_poll();
            }
        }
        // before_poll may have quit (e.g. lost Wayland connection) or removed sources.
        if (!running_) {
            break;
        }

        fds.clear();
        ids.clear();
        for (const Source& source : sources_) {
            if (!source.removed) {
                fds.push_back({.fd = source.fd, .events = source.events, .revents = 0});
                ids.push_back(source.id);
            }
        }
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno != EINTR) {
                log::error("poll: {}", std::strerror(errno));
                return 1;
            }
            for (pollfd& fd : fds) {
                fd.revents = 0;
            }
        }

        // Callbacks may add sources (appended, not visited this round) or remove any of them.
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto it = std::ranges::find(sources_, ids[i], &Source::id);
            if (it == sources_.end() || it->removed) {
                continue;
            }
            if (fds[i].revents != 0 || it->options.notify_always) {
                // Copy: the callback may add sources and reallocate sources_.
                const FdCallback callback = it->callback;
                callback(fds[i].revents);
            }
        }
    }
    return exit_code_;
}

void EventLoop::quit(int code) {
    exit_code_ = code;
    running_ = false;
}

}  // namespace rummage::core
