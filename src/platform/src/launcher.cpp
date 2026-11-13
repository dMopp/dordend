#include "rummage/platform/launcher.hpp"

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): unistd.h hides it in C++

namespace rummage::platform {
namespace {

// posix_spawn's attributes and file actions, released on every path.
struct SpawnSetup {
    SpawnSetup() {
        posix_spawnattr_init(&attr);
        posix_spawn_file_actions_init(&actions);
    }

    ~SpawnSetup() {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);
    }

    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    SpawnSetup(SpawnSetup&&) = delete;
    SpawnSetup& operator=(SpawnSetup&&) = delete;

    posix_spawnattr_t attr{};
    posix_spawn_file_actions_t actions{};
};

// rummage's environment with env's variables replacing or added to it.
std::vector<std::string> child_environment(std::span<const std::string> env) {
    const auto name_of = [](std::string_view variable) {
        return variable.substr(0, variable.find('='));
    };
    std::vector<std::string> out;
    for (char** variable = environ; variable != nullptr && *variable != nullptr; ++variable) {
        const std::string_view own{*variable};
        bool replaced = false;
        for (const std::string& added : env) {
            replaced = replaced || name_of(added) == name_of(own);
        }
        if (!replaced) {
            out.emplace_back(own);
        }
    }
    out.insert(out.end(), env.begin(), env.end());
    return out;
}

// A NULL-terminated array of pointers into strings, as exec wants it.
std::vector<char*> c_array(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (std::string& string : strings) {
        out.push_back(string.data());
    }
    out.push_back(nullptr);
    return out;
}

}  // namespace

Launcher::Launcher(core::EventLoop& loop) : loop_{loop} {}

Launcher::~Launcher() {
    // The children go on; rummage only stops listening.
    for (auto& [pidfd, child] : children_) {
        loop_.remove(child.source);
        close(pidfd);
    }
}

std::error_code Launcher::spawn(const std::vector<std::string>& argv,
                                const std::filesystem::path& directory,
                                std::span<const std::string> env, std::function<void(Exit)> on_exit,
                                int stdout_fd, int stderr_fd, int stdin_fd, int archive_fd) {
    if (argv.empty() || (stdout_fd != -1 && stdout_fd < 3) || (stderr_fd != -1 && stderr_fd < 3) ||
        (stdin_fd != -1 && stdin_fd < 3) || (archive_fd != -1 && archive_fd < 3)) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    SpawnSetup setup;
    sigset_t none{};
    sigemptyset(&none);
    sigset_t all{};
    sigfillset(&all);
    posix_spawnattr_setsigmask(&setup.attr, &none);
    posix_spawnattr_setsigdefault(&setup.attr, &all);
    posix_spawnattr_setflags(&setup.attr,
                             POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    for (const int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        const int source = fd == STDOUT_FILENO   ? stdout_fd
                           : fd == STDERR_FILENO ? stderr_fd
                                                 : stdin_fd;
        if (source >= 3) {
            posix_spawn_file_actions_adddup2(&setup.actions, source, fd);
        } else {
            posix_spawn_file_actions_addopen(&setup.actions, fd, "/dev/null",
                                             fd == STDIN_FILENO ? O_RDONLY : O_WRONLY, 0);
        }
    }
    if (archive_fd >= 3)
        posix_spawn_file_actions_adddup2(&setup.actions, archive_fd, 3);
    posix_spawn_file_actions_addclosefrom_np(&setup.actions, archive_fd >= 3 ? 4 : 3);
    if (!directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&setup.actions, directory.c_str());
    }

    std::vector<std::string> args = argv;
    std::vector<std::string> environment = child_environment(env);
    const std::vector<char*> c_args = c_array(args);
    const std::vector<char*> c_env = c_array(environment);
    int pidfd = -1;
    const int error = pidfd_spawnp(&pidfd, c_args.front(), &setup.actions, &setup.attr,
                                   c_args.data(), c_env.data());
    if (error != 0) {
        return {error, std::generic_category()};
    }
    log::debug("launcher: started {} (pidfd {})", argv.front(), pidfd);
    const core::EventLoop::SourceId source =
        loop_.add_fd(pidfd, POLLIN, [this, pidfd](short /*revents*/) { reap(pidfd); });
    children_.emplace(pidfd, Child{.pidfd = pidfd,
                                   .source = source,
                                   .started = Clock::now(),
                                   .on_exit = std::move(on_exit)});
    return {};
}

void Launcher::reap(int pidfd) {
    const auto it = children_.find(pidfd);
    if (it == children_.end()) {
        return;
    }
    Child child = std::move(it->second);
    children_.erase(it);
    loop_.remove(child.source);

    siginfo_t info{};
    Exit exit{.lifetime = Clock::now() - child.started};
    if (waitid(P_PIDFD, static_cast<id_t>(pidfd), &info, WEXITED) == 0) {
        if (info.si_code == CLD_EXITED) {
            exit.status = info.si_status;
        } else {
            exit.signal = info.si_status;
        }
    }
    close(pidfd);
    log::debug("launcher: pidfd {} ended, status {} signal {}", pidfd, exit.status, exit.signal);
    if (child.on_exit) {
        child.on_exit(exit);
    }
}

}  // namespace rummage::platform
