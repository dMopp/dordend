#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

// Leveled logging to stderr. Thread-safe, flushed per line so a crash keeps the tail.
// The level comes from $RUMMAGE_LOG (trace, debug, info, warn, error, off); default is
// debug in a debug build, info otherwise.
namespace rummage::log {

enum class Level : std::uint8_t { trace, debug, info, warn, error, off };

[[nodiscard]] std::string_view to_string(Level level) noexcept;

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

// Reads $RUMMAGE_LOG once; an unknown value is reported and ignored.
void init_from_env();

void write(Level level, std::string_view message);

template <class... Args>
void message(Level level, std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(level)) {
        return;  // no formatting cost for a filtered message
    }
    write(level, std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::trace, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::warn, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    message(Level::error, fmt, std::forward<Args>(args)...);
}

}  // namespace rummage::log
