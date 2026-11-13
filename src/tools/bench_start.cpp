// Start-up benchmark (ROADMAP 7.6). Not installed.
//
//   rummage-bench-start [-n RUNS] [--keep-open] -- COMMAND [ARGS...]
//
// Runs COMMAND (rummage) RUNS times, one after the other. Each run gets RUMMAGE_STARTUP_NS, the
// steady-clock time just before its exec, and RUMMAGE_STARTUP_QUIT, so rummage prints
// "startup: STEP MS" for each step of its start and closes once its folder is shown. Prints
// every run (and rummage's warnings and errors), then the median, minimum and maximum of each step.
// --keep-open leaves RUMMAGE_STARTUP_QUIT out (one run to look at). A run silent for 10 s is
// stopped and counts as failed. Cold-cache runs: drop the page cache before each run (as root) and
// call this with -n 1.
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <map>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kUsage =
    "usage: rummage-bench-start [-n RUNS] [--keep-open] -- COMMAND [ARGS...]\n";

constexpr std::string_view kPrefix = "startup: ";
constexpr int kRunTimeoutMs = 10'000;

struct Step {
    std::string name;
    double ms{0.0};
};

// "startup: first-draw 41.20 ms" -> {first-draw, 41.2}; other lines are passed through.
bool parse(std::string_view line, Step& step) {
    if (!line.starts_with(kPrefix)) {
        return false;
    }
    line.remove_prefix(kPrefix.size());
    const std::size_t space = line.find(' ');
    if (space == std::string_view::npos) {
        return false;
    }
    step.name = std::string{line.substr(0, space)};
    step.ms = std::strtod(std::string{line.substr(space + 1)}.c_str(), nullptr);
    return true;
}

// One run: the steps it printed, in order; empty when it failed.
std::vector<Step> run(char** command, bool keep_open) {
    std::array<int, 2> pipe_fds{};
    if (pipe(pipe_fds.data()) != 0) {
        std::perror("pipe");
        return {};
    }
    const auto started = std::chrono::steady_clock::now();
    const pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        return {};
    }
    if (child == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[1]);
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const std::string ns =
            std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        setenv("RUMMAGE_STARTUP_NS", ns.c_str(), 1);  // NOLINT(concurrency-mt-unsafe): one thread
        if (!keep_open) {
            setenv("RUMMAGE_STARTUP_QUIT", "1", 1);  // NOLINT(concurrency-mt-unsafe)
        }
        execvp(command[0], command);
        std::perror(command[0]);
        _exit(127);
    }
    close(pipe_fds[1]);
    std::vector<Step> steps;
    std::string pending;
    std::array<char, 4096> buffer{};
    bool timed_out = false;
    for (;;) {
        // A run that never shows its folder (no frame callback) is stopped, not waited for.
        pollfd readable{.fd = pipe_fds[0], .events = POLLIN, .revents = 0};
        if (keep_open ? poll(&readable, 1, -1) <= 0 : poll(&readable, 1, kRunTimeoutMs) <= 0) {
            timed_out = true;
            kill(child, SIGTERM);
            break;
        }
        const ssize_t got = read(pipe_fds[0], buffer.data(), buffer.size());
        if (got <= 0) {
            break;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(got));
        for (std::size_t end = pending.find('\n'); end != std::string::npos;
             end = pending.find('\n')) {
            const std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            Step step;
            if (parse(line, step)) {
                steps.push_back(step);
            } else if (line.find("] info ") == std::string::npos) {
                std::fprintf(stderr, "  | %s\n", line.c_str());  // warnings and errors
            }
        }
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    const double total =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    if (timed_out) {
        std::fprintf(stderr, "run stopped: nothing for %d s (steps so far: %zu)\n",
                     kRunTimeoutMs / 1000, steps.size());
        return {};
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "run failed (status %d)\n", status);
        return {};
    }
    steps.push_back({.name = "exit", .ms = total});
    return steps;
}

int bench(int argc, char** argv) {
    int runs = 10;
    bool keep_open = false;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--") {
            ++i;
            break;
        }
        if (arg == "-n" && i + 1 < argc) {
            runs = std::atoi(argv[++i]);
        } else if (arg == "--keep-open") {
            keep_open = true;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
    }
    if (i >= argc || runs < 1) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    char** command = argv + i;

    std::vector<std::string> order;
    std::map<std::string, std::vector<double>> times;
    for (int r = 1; r <= runs; ++r) {
        const std::vector<Step> steps = run(command, keep_open);
        if (steps.empty()) {
            return 1;
        }
        std::string line = std::format("run {:2}:", r);
        for (const Step& step : steps) {
            if (!times.contains(step.name)) {
                order.push_back(step.name);
            }
            times[step.name].push_back(step.ms);
            line += std::format("  {} {:.1f}", step.name, step.ms);
        }
        std::puts(line.c_str());
    }
    std::puts("");
    std::printf("%-20s %9s %9s %9s  (ms since exec, %d runs)\n", "step", "median", "min", "max",
                runs);
    for (const std::string& name : order) {
        std::vector<double>& values = times[name];
        std::ranges::sort(values);
        const std::size_t n = values.size();
        const double median =
            n % 2 == 1 ? values[n / 2] : (values[(n / 2) - 1] + values[n / 2]) / 2.0;
        std::printf("%-20s %9.1f %9.1f %9.1f\n", name.c_str(), median, values.front(),
                    values.back());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return bench(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "rummage-bench-start: %s\n", error.what());
        return 1;
    }
}
