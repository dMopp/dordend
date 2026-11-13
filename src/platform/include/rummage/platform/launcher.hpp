#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {

// Starts programs detached from rummage and reports when they end, without threads or zombies.
//
// A child runs in a session of its own, with the default signal mask and dispositions (rummage
// blocks SIGINT and SIGTERM for its signalfd), stdin, stdout and stderr on /dev/null and no other
// inherited descriptor (unless streams are explicitly redirected). Its pidfd sits in the event
// loop: when the child exits it is reaped there and on_exit learns how. Closing rummage leaves the
// children running.
//
// Loop thread only.
class Launcher {
public:
    using Clock = std::chrono::steady_clock;

    // How a child ended: its exit status, or the signal that killed it; and how long it ran.
    struct Exit {
        int status{0};
        int signal{0};  // nonzero when killed by a signal
        Clock::duration lifetime{};
    };

    explicit Launcher(core::EventLoop& loop);
    ~Launcher();

    Launcher(const Launcher&) = delete;
    Launcher& operator=(const Launcher&) = delete;
    Launcher(Launcher&&) = delete;
    Launcher& operator=(Launcher&&) = delete;

    // Optional stream descriptors (>= 3) are duplicated into the child; ownership stays with
    // the caller. -1 redirects to /dev/null. archive_fd, when supplied, is mapped to fd 3
    // after streams are duplicated, leaving stdin available for archive passwords.
    // Starts argv (argv[0] looked up in the PATH) in directory (empty: rummage's), with env added
    // to rummage's environment ("NAME=value", replacing a variable of the same name). An error
    // when the program cannot be started (not found, not executable); on_exit is then not called.
    std::error_code spawn(const std::vector<std::string>& argv,
                          const std::filesystem::path& directory, std::span<const std::string> env,
                          std::function<void(Exit)> on_exit, int stdout_fd = -1, int stderr_fd = -1,
                          int stdin_fd = -1, int archive_fd = -1);

    // Children started and not yet reaped (tests).
    [[nodiscard]] std::size_t running() const noexcept { return children_.size(); }

private:
    struct Child {
        int pidfd{-1};
        core::EventLoop::SourceId source{0};
        Clock::time_point started;
        std::function<void(Exit)> on_exit;
    };

    void reap(int pidfd);

    core::EventLoop& loop_;
    std::unordered_map<int, Child> children_;  // by pidfd
};

}  // namespace rummage::platform
