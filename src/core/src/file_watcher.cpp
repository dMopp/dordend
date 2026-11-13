#include "rummage/core/file_watcher.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/inotify.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::core {
namespace {

// Writes closed, renames into and out of the directory, deletions. IN_MODIFY is left out on
// purpose: it fires on every write() and after a truncation, before the content is complete.
constexpr std::uint32_t kEvents =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE;

}  // namespace

FileWatcher::FileWatcher(EventLoop& loop, const std::filesystem::path& file, Handler handler,
                         std::chrono::milliseconds settle)
    : loop_{loop},
      name_{file.filename().string()},
      handler_{std::move(handler)},
      settle_{settle},
      fd_{inotify_init1(IN_NONBLOCK | IN_CLOEXEC)} {
    if (fd_ < 0) {
        throw std::system_error{errno, std::generic_category(), "inotify_init1"};
    }
    const std::filesystem::path directory = file.parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (inotify_add_watch(fd_, directory.c_str(), kEvents) < 0) {
        const int saved = errno;
        close(fd_);
        throw std::system_error{saved, std::generic_category(),
                                "inotify_add_watch " + directory.string()};
    }
    source_ = loop_.add_fd(fd_, POLLIN, [this](short /*revents*/) { on_readable(); });
}

FileWatcher::~FileWatcher() {
    loop_.remove(timer_);
    loop_.remove(source_);
    close(fd_);
}

void FileWatcher::on_readable() {
    // Big enough for many events with names; aligned as inotify requires.
    alignas(inotify_event) std::array<char, 4096> buffer{};
    bool relevant = false;
    for (;;) {
        const ssize_t length = read(fd_, buffer.data(), buffer.size());
        if (length <= 0) {
            if (length < 0 && errno != EAGAIN && errno != EINTR) {
                log::warn("file watcher: read failed: {}", std::strerror(errno));  // NOLINT
            }
            break;
        }
        for (std::size_t offset = 0; std::cmp_less(offset, length);) {
            inotify_event event{};
            std::memcpy(&event, buffer.data() + offset, sizeof event);
            const char* name = buffer.data() + offset + sizeof event;
            // On a queue overflow events were lost: assume the file changed.
            if ((event.len > 0 && name_ == name) || (event.mask & IN_Q_OVERFLOW) != 0U) {
                relevant = true;
            }
            if ((event.mask & IN_IGNORED) != 0U) {
                log::warn("file watcher: directory of {} gone, changes no longer followed", name_);
            }
            offset += sizeof event + event.len;
        }
    }
    if (!relevant || timer_ != 0) {
        return;  // a pending timer already covers this change
    }
    timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
        timer_ = 0;
        handler_();
    });
}

}  // namespace rummage::core
