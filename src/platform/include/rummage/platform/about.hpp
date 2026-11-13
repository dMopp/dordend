#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What `rummage --about` tells about the system (ROADMAP 7.5d), to paste into bug reports.
namespace rummage::platform {

struct AboutLine {
    std::string label;
    std::string value;

    bool operator==(const AboutLine&) const = default;
};

// os-release's PRETTY_NAME, else its NAME (quotes and escapes as the spec has them); empty when
// it has neither.
[[nodiscard]] std::string distribution_name(std::string_view os_release);

// The version in a program's --version line: "noctalia v5.4.0" -> "5.4.0",
// "umbriel 0.1.0 (abc1234)" -> "0.1.0 (abc1234)"; empty when the line has none.
[[nodiscard]] std::string version_from(std::string_view line);

// A running process of this user called name (as /proc/PID/comm has it): its executable, as
// /proc/PID/exe, the very program running even when it was upgraded since. nullopt when none.
[[nodiscard]] std::optional<std::filesystem::path> running(std::string_view name);

// The first line program --version prints, given at most wait; empty when it fails or takes
// longer (it is then killed).
[[nodiscard]] std::string version_of(const std::filesystem::path& program,
                                     std::chrono::milliseconds wait = std::chrono::seconds{1});

// The distribution, the kernel, the desktop, and the versions of Noctalia and Umbriel when they
// are running; a line is left out when its value is not known. Runs those two programs: not on
// the loop thread.
[[nodiscard]] std::vector<AboutLine> system_about();

}  // namespace rummage::platform
