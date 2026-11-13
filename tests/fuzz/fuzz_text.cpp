// Fuzzes the text family (ROADMAP 3b) the way the base engine runs it: the first byte picks the
// limits (small ones too, to reach the cuts), the type and the name, the rest is the file; read
// from a memfd as the helper reads and coloured, and decoded as the start of a longer file.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

#include "rummage/formats/text/read.hpp"
#include "rummage/formats/text/syntax.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    static constexpr std::array<rummage::text::Limits, 4> kLimits{{
        {},
        {.max_bytes = 8192, .max_line_bytes = 256, .max_lines = 40, .tab_width = 4},
        {.max_bytes = 64, .max_line_bytes = 3, .max_lines = 0, .tab_width = 8},
        {.max_bytes = 7, .max_line_bytes = 1, .max_lines = 2, .tab_width = 1},
    }};
    static constexpr std::array<std::string_view, 16> kTypes{"text/plain",
                                                             "text/x-c++src",
                                                             "text/rust",
                                                             "text/x-python3",
                                                             "text/javascript",
                                                             "text/x-lua",
                                                             "application/x-shellscript",
                                                             "application/x-fishscript",
                                                             "text/x-cmake",
                                                             "text/x-makefile",
                                                             "application/json",
                                                             "application/toml",
                                                             "application/yaml",
                                                             "application/x-desktop",
                                                             "text/markdown",
                                                             "text/x-patch"};
    rummage::text::Limits limits = kLimits.at(data[0] % kLimits.size());
    const std::string_view mime = kTypes.at((data[0] / kLimits.size()) % kTypes.size());
    // Languages found by name only (with text/plain): ignore files, .gitattributes.
    // A log is read from its end (Limits::tail).
    static constexpr std::array<std::string_view, 4> kNames{"fuzz", ".gitignore", ".gitattributes",
                                                            "fuzz.log"};
    const std::string_view name =
        kNames.at((data[0] / (kLimits.size() * kTypes.size())) % kNames.size());
    const std::span<const std::uint8_t> file{data + 1, size - 1};
    limits.tail = name.ends_with(".log");
    static_cast<void>(rummage::formats::text::decode(file, true, limits));
    const int fd = ::memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    if (::write(fd, file.data(), file.size()) == static_cast<ssize_t>(file.size())) {
        rummage::text::Loaded loaded = rummage::formats::text::read(fd, limits);
        rummage::formats::text::colour(loaded.text, name, mime);
    }
    ::close(fd);
    return 0;
}
