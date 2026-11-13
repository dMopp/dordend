#include "rummage/platform/about.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

#include "rummage/core/file.hpp"

namespace rummage::platform {
namespace {

std::string_view trim(std::string_view text) {
    constexpr std::string_view blank = " \t\r\n";
    const std::size_t first = text.find_first_not_of(blank);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(blank) - first + 1);
}

// An os-release value: in double or single quotes or bare, with \" \\ \$ \` escapes.
std::string unquote(std::string_view value) {
    value = trim(value);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
        }
        out += value[i];
    }
    return out;
}

// A file descriptor closed on every path.
class Fd {
public:
    explicit Fd(int fd = -1) : fd_{fd} {}

    ~Fd() { reset(); }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

    void reset() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

}  // namespace

std::string distribution_name(std::string_view os_release) {
    std::string pretty;
    std::string name;
    while (!os_release.empty()) {
        const std::size_t newline = os_release.find('\n');
        const std::string_view line = trim(os_release.substr(0, newline));
        os_release =
            newline == std::string_view::npos ? std::string_view{} : os_release.substr(newline + 1);
        if (line.starts_with("PRETTY_NAME=")) {
            pretty = unquote(line.substr(12));
        } else if (line.starts_with("NAME=")) {
            name = unquote(line.substr(5));
        }
    }
    return pretty.empty() ? name : pretty;
}

std::string version_from(std::string_view line) {
    line = trim(line.substr(0, line.find('\n')));
    // The first word that starts with a digit, or with "v" and a digit; the rest of the line
    // after it (a revision in parentheses) goes with it.
    std::size_t at = 0;
    while (at < line.size()) {
        const std::size_t end = std::min(line.find(' ', at), line.size());
        std::string_view word = line.substr(at, end - at);
        if (word.size() >= 2 && (word[0] == 'v' || word[0] == 'V') &&
            std::isdigit(static_cast<unsigned char>(word[1])) != 0) {
            return std::string{line.substr(at + 1)};
        }
        if (!word.empty() && std::isdigit(static_cast<unsigned char>(word[0])) != 0) {
            return std::string{line.substr(at)};
        }
        at = end + 1;
    }
    return {};
}

std::optional<std::filesystem::path> running(std::string_view name) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{"/proc", error}) {
        const std::string pid = entry.path().filename().string();
        if (pid.empty() || !std::ranges::all_of(pid, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        struct stat info{};
        if (stat(entry.path().c_str(), &info) != 0 || info.st_uid != getuid()) {
            continue;  // someone else's
        }
        // /proc's files say they are empty: read what is there, not what the size says.
        std::ifstream file{entry.path() / "comm"};
        std::string comm;
        if (std::getline(file, comm) && trim(comm) == name) {
            return entry.path() / "exe";
        }
    }
    return std::nullopt;
}

std::string version_of(const std::filesystem::path& program, std::chrono::milliseconds wait) {
    std::array<int, 2> ends{-1, -1};
    if (pipe2(ends.data(), O_CLOEXEC) != 0) {
        return {};
    }
    Fd read_end{ends[0]};
    Fd write_end{ends[1]};
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);  // nothing of ours
    posix_spawnattr_t attr{};
    posix_spawnattr_init(&attr);
    sigset_t none{};
    sigemptyset(&none);
    sigset_t all{};
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::string path = program.string();
    std::string flag = "--version";
    std::array<char*, 3> argv{path.data(), flag.data(), nullptr};
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    write_end.reset();  // the child's copy is all that is left: EOF when it is done
    if (spawned != 0) {
        return {};
    }

    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    bool done = false;
    while (!done && out.size() < 4096) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            break;
        }
        pollfd watched{.fd = read_end.get(), .events = POLLIN, .revents = 0};
        const int ready = poll(&watched, 1, static_cast<int>(left.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }
        std::array<char, 512> buffer{};
        const ssize_t got = read(read_end.get(), buffer.data(), buffer.size());
        if (got > 0) {
            out.append(buffer.data(), static_cast<std::size_t>(got));
        } else if (got == 0 || errno != EINTR) {
            done = true;
        }
    }
    if (!done) {
        kill(pid, SIGKILL);  // too slow, or it does not stop talking
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!done || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return {};
    }
    return std::string{trim(std::string_view{out}.substr(0, out.find('\n')))};
}

std::vector<AboutLine> system_about() {
    std::vector<AboutLine> lines;
    const auto add = [&lines](std::string label, std::string value) {
        if (!value.empty()) {
            lines.push_back({.label = std::move(label), .value = std::move(value)});
        }
    };
    core::TextFile os = core::read_text_file("/etc/os-release");
    if (!os.exists) {
        os = core::read_text_file("/usr/lib/os-release");
    }
    add("Distribution", distribution_name(os.text));
    utsname system{};
    if (uname(&system) == 0) {
        add("Kernel", std::format("{} {}", static_cast<const char*>(system.sysname),
                                  static_cast<const char*>(system.release)));
    }
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");  // NOLINT(concurrency-mt-unsafe)
    add("Desktop", desktop != nullptr ? desktop : "");
    for (const auto& [label, name] :
         {std::pair{"Noctalia", "noctalia"}, std::pair{"Umbriel", "umbriel"}}) {
        if (const std::optional<std::filesystem::path> program = running(name)) {
            add(label, version_from(version_of(*program)));
        }
    }
    return lines;
}

}  // namespace rummage::platform
