#include <array>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <doctest/doctest.h>

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, not in a header

namespace {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

struct Probed {
    std::string out;  // what the probe printed
    int status{0};    // its wait status
};

// Runs the sandbox probe with one argument and collects what it printed and how it ended.
Probed probe(const char* what) {
    std::array<int, 2> ends{-1, -1};
    REQUIRE(::pipe2(ends.data(), O_CLOEXEC) == 0);
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    std::string program = RUMMAGE_SANDBOX_PROBE;
    std::string arg = what;
    std::array<char*, 3> argv{program.data(), arg.data(), nullptr};
    pid_t pid = -1;
    const int spawned =
        ::posix_spawn(&pid, program.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(ends[1]);
    REQUIRE(spawned == 0);
    Probed result;
    std::array<char, 256> buffer{};
    ssize_t got = 0;
    while ((got = ::read(ends[0], buffer.data(), buffer.size())) > 0) {
        result.out.append(buffer.data(), static_cast<std::size_t>(got));
    }
    ::close(ends[0]);
    ::waitpid(pid, &result.status, 0);
    return result;
}

bool killed_by_seccomp(const Probed& probed) {
    return WIFSIGNALED(probed.status) && WTERMSIG(probed.status) == SIGSYS;
}

}  // namespace

TEST_CASE("sandbox: what a picture needs still works") {
    const Probed allowed = probe("allowed");
    CHECK(allowed.out == "ok\n");
    CHECK(WIFEXITED(allowed.status));
}

TEST_CASE("sandbox: no file can be opened, no path looked at") {
    CHECK(probe("open").out == "EACCES\n");
    CHECK(probe("stat").out == "EACCES\n");
}

TEST_CASE("sandbox: no network, no programs, no new processes") {
    for (const char* what : {"socket", "exec", "fork"}) {
        INFO(what);
        const Probed probed = probe(what);
        CHECK(killed_by_seccomp(probed));
        CHECK(probed.out.empty());
    }
}

TEST_CASE("sandbox: at most 4 GiB of memory" * doctest::skip(kSanitized)) {
    CHECK(probe("memory").out == "refused\n");
}

TEST_CASE("sandbox: landlock alone keeps files and TCP out") {
    const Probed probed = probe("landlock");
    if (probed.out == "no landlock\n") {
        MESSAGE("landlock is not available on this kernel");
        return;
    }
    CHECK(probed.out == "EACCES\nEACCES\n");  // the file, then the connection
}
