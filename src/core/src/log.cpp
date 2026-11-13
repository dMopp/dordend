#include "rummage/core/log.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace rummage::log {
namespace {

#if defined(NDEBUG)
constexpr Level kDefaultLevel = Level::info;
#else
constexpr Level kDefaultLevel = Level::debug;
#endif

constexpr std::array kLevelNames{std::string_view{"trace"}, std::string_view{"debug"},
                                 std::string_view{"info"},  std::string_view{"warn"},
                                 std::string_view{"error"}, std::string_view{"off"}};

std::atomic<Level> g_level{kDefaultLevel};
std::mutex g_write_mutex;

// Monotonic seconds since the first log line: enough to read startup timings.
double uptime_seconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point kStart = Clock::now();
    return std::chrono::duration<double>(Clock::now() - kStart).count();
}

}  // namespace

std::string_view to_string(Level level) noexcept {
    return kLevelNames.at(static_cast<std::size_t>(level));
}

void set_level(Level level) noexcept { g_level.store(level, std::memory_order_relaxed); }

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

bool enabled(Level level) noexcept { return level != Level::off && level >= log::level(); }

void init_from_env() {
    const char* value = std::getenv("RUMMAGE_LOG");
    if (value == nullptr) {
        return;
    }
    const std::string_view wanted{value};
    for (std::size_t i = 0; i < kLevelNames.size(); ++i) {
        if (kLevelNames.at(i) == wanted) {
            set_level(static_cast<Level>(i));
            return;
        }
    }
    warn("RUMMAGE_LOG='{}' is not a log level; keeping '{}'", wanted, to_string(level()));
}

void write(Level level, std::string_view message) {
    const std::string line =
        std::format("[{:9.3f}] {:<5} {}\n", uptime_seconds(), to_string(level), message);
    const std::scoped_lock lock{g_write_mutex};
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

}  // namespace rummage::log
