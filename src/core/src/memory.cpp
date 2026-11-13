#include "rummage/core/memory.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <malloc.h>
#include <map>
#include <set>
#include <string_view>

namespace rummage::core {
namespace {

// "Rss:   1234 kB" -> 1234 * 1024; "drm-memory-vram:  50 KiB" the same.
std::optional<std::uint64_t> kilobytes(std::string_view line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view value = line.substr(colon + 1);
    value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{}) {
        return std::nullopt;
    }
    std::string_view unit{end, static_cast<std::size_t>(value.data() + value.size() - end)};
    unit.remove_prefix(std::min(unit.find_first_not_of(' '), unit.size()));
    if (unit.starts_with("kB") || unit.starts_with("KiB")) {
        return number * 1024;
    }
    if (unit.starts_with("MiB")) {
        return number * 1024 * 1024;
    }
    if (unit.starts_with("GiB")) {
        return number * 1024 * 1024 * 1024;
    }
    return number;  // bytes
}

std::string_view key_of(std::string_view line) { return line.substr(0, line.find(':')); }

// The group a mapping's path falls in.
std::string group_of(std::string_view path) {
    if (path.empty()) {
        return "[anon]";
    }
    if (path.starts_with('[')) {
        return std::string{path.substr(0, path.find(']') + 1)};
    }
    if (path.find(".so") != std::string_view::npos) {
        return "libraries";
    }
    if (path.ends_with(".ttf") || path.ends_with(".otf") || path.ends_with(".ttc") ||
        path.find("/fonts/") != std::string_view::npos) {
        return "fonts";
    }
    if (path.starts_with("/dev/dri/")) {
        return std::string{path};
    }
    if (path.starts_with("/memfd:")) {
        return "memfd";
    }
    return std::string{path};
}

void read_rollup(MemoryReport& report) {
    std::ifstream in{"/proc/self/smaps_rollup"};
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view key = key_of(line);
        const std::uint64_t value = kilobytes(line).value_or(0);
        if (key == "Rss") {
            report.rss = value;
        } else if (key == "Anonymous") {
            report.anonymous = value;
        } else if (key == "Private_Clean") {
            report.private_clean = value;
        } else if (key == "Private_Dirty") {
            report.private_dirty = value;
        } else if (key == "Shared_Clean" || key == "Shared_Dirty") {
            report.shared += value;
        }
    }
}

void read_peak(MemoryReport& report) {
    std::ifstream in{"/proc/self/status"};
    std::string line;
    while (std::getline(in, line)) {
        if (key_of(line) == "VmHWM") {
            report.peak_rss = kilobytes(line).value_or(0);
            return;
        }
    }
}

void read_mappings(MemoryReport& report) {
    std::ifstream in{"/proc/self/smaps"};
    std::map<std::string, MemoryReport::Mapping> groups;
    MemoryReport::Mapping* current = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        // A mapping's header starts with its address range ("7f..-7f.. r-xp ..."); its fields
        // with a capitalised key.
        const char first = line.front();
        const bool header = (first >= '0' && first <= '9') || (first >= 'a' && first <= 'f');
        if (header && line.find(':') > line.find(' ')) {
            // address perms offset dev inode [path]
            std::size_t at = 0;
            for (int field = 0; field < 5 && at != std::string::npos; ++field) {
                at = line.find_first_not_of(' ', line.find(' ', at));
            }
            const std::string path = at == std::string::npos ? std::string{} : line.substr(at);
            const std::string group = group_of(path);
            current = &groups[group];
            current->name = group;
            continue;
        }
        if (current == nullptr) {
            continue;
        }
        const std::string_view key = key_of(line);
        if (key == "Rss") {
            current->rss += kilobytes(line).value_or(0);
        } else if (key == "Anonymous") {
            current->anonymous += kilobytes(line).value_or(0);
        }
    }
    for (auto& [name, mapping] : groups) {
        if (mapping.rss > 0) {
            report.mappings.push_back(std::move(mapping));
        }
    }
    std::ranges::sort(report.mappings, std::ranges::greater{}, &MemoryReport::Mapping::rss);
}

void read_heap(MemoryReport& report) {
    const struct mallinfo2 info = mallinfo2();
    report.heap_arena = info.arena;
    report.heap_mmapped = info.hblkhd;
    report.heap_in_use = info.uordblks + info.hblkhd;
    report.heap_free = info.fordblks;
}

void read_gpu(MemoryReport& report) {
    std::set<std::string> clients;  // a client's fds (dup'ed) count once
    std::error_code ignored;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{"/proc/self/fdinfo", ignored}) {
        std::ifstream in{entry.path()};
        std::string line;
        std::string client;
        std::map<std::string, std::uint64_t> values;
        while (std::getline(in, line)) {
            const std::string_view key = key_of(line);
            if (key == "drm-client-id") {
                client = line.substr(line.find(':') + 1);
            } else if (key.starts_with("drm-memory-")) {
                values[std::string{key}] = kilobytes(line).value_or(0);
            }
        }
        if (client.empty() || values.empty() || !clients.insert(client).second) {
            continue;
        }
        const auto add = [&values](std::optional<std::uint64_t>& total, const char* key) {
            if (const auto it = values.find(key); it != values.end()) {
                total = total.value_or(0) + it->second;
            }
        };
        add(report.gpu_vram, "drm-memory-vram");
        add(report.gpu_gtt, "drm-memory-gtt");
        add(report.gpu_cpu, "drm-memory-cpu");
    }
}

std::string mb(std::uint64_t bytes) {
    return std::format("{:.1f} MB", static_cast<double>(bytes) / 1e6);
}

std::string mb(const std::optional<std::uint64_t>& bytes) {
    return bytes ? mb(*bytes) : std::string{"?"};
}

}  // namespace

void give_back_large_blocks() {
    constexpr int threshold = 16 << 20;  // 16 MB: measured against 1 and 4 MB (benchmarks, 6.3c)
    mallopt(M_MMAP_THRESHOLD, threshold);
}

MemoryReport read_memory_report() {
    MemoryReport report;
    read_rollup(report);
    read_peak(report);
    read_mappings(report);
    read_heap(report);
    read_gpu(report);
    return report;
}

std::vector<std::string> describe(const MemoryReport& report) {
    std::vector<std::string> lines;
    lines.push_back(
        std::format("process: rss {} (peak {}), anonymous {}, private {} clean + {} dirty, "
                    "shared {}",
                    mb(report.rss), mb(report.peak_rss), mb(report.anonymous),
                    mb(report.private_clean), mb(report.private_dirty), mb(report.shared)));
    lines.push_back(std::format("malloc: in use {}, freed but kept {}, arenas {}, mmapped {}",
                                mb(report.heap_in_use), mb(report.heap_free), mb(report.heap_arena),
                                mb(report.heap_mmapped)));
    lines.push_back(std::format("gpu: vram {}, gtt {}, cpu {}", mb(report.gpu_vram),
                                mb(report.gpu_gtt), mb(report.gpu_cpu)));
    constexpr std::size_t shown = 8;
    for (std::size_t i = 0; i < std::min(shown, report.mappings.size()); ++i) {
        const MemoryReport::Mapping& mapping = report.mappings[i];
        lines.push_back(std::format("  mapped {}: rss {} (anonymous {})", mapping.name,
                                    mb(mapping.rss), mb(mapping.anonymous)));
    }
    return lines;
}

void give_back_free_memory() { malloc_trim(0); }

}  // namespace rummage::core
