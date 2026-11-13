// MIME detection check (ROADMAP 3.2). Not installed.
//
//   rummage-mime PATH...
//
// Prints "<type>\t<name>" for each file, or for each entry of a directory, using the full
// detection (kind, name, content when needed); timings go to stderr. Compare with GLib:
//   gio info -a standard::content-type FILE
#include "rummage/fs/mime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory.hpp"
#include "rummage/fs/entry.hpp"

namespace {

namespace core = rummage::core;
namespace fs = rummage::fs;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: rummage-mime PATH...\n");
        return 2;
    }
    try {
        Clock::time_point start = Clock::now();
        const auto db = fs::MimeDatabase::load();
        std::fprintf(stderr, "database: %zu globs, %zu magic sections, head %zu bytes, %.2f ms\n",
                     db->glob_count(), db->magic_count(), db->magic_extent(), ms_since(start));

        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::filesystem::path path{args[i]};
            std::vector<fs::Entry> entries;
            std::filesystem::path directory = path;
            if (std::filesystem::is_directory(path)) {
                const fs::ReadResult result = fs::read_directory(
                    path, core::CancelToken{}, {.mime = db}, [&](std::vector<fs::Entry> chunk) {
                        for (fs::Entry& entry : chunk) {
                            entries.push_back(std::move(entry));
                        }
                    });
                if (result.error) {
                    std::fprintf(stderr, "%s: %s\n", path.c_str(), result.error.message().c_str());
                    continue;
                }
            } else {
                // A single file: read its parent and keep the one entry.
                directory = path.parent_path().empty() ? "." : path.parent_path();
                fs::read_directory(directory, core::CancelToken{}, {.mime = db},
                                   [&](std::vector<fs::Entry> chunk) {
                                       for (fs::Entry& entry : chunk) {
                                           if (entry.name == path.filename().string()) {
                                               entries.push_back(std::move(entry));
                                           }
                                       }
                                   });
            }
            std::size_t from_name = 0;
            for (const fs::Entry& entry : entries) {
                from_name += entry.mime != fs::kUnknownMime ? 1 : 0;
            }
            start = Clock::now();
            for (const fs::Entry& entry : entries) {
                const fs::MimeId mime =
                    entry.mime != fs::kUnknownMime ? entry.mime : db->detect(directory, entry);
                std::printf("%s\t%s\n", std::string{db->name(mime)}.c_str(), entry.name.c_str());
            }
            std::fprintf(stderr,
                         "%s: %zu entries, %zu decided by kind or name, rest by content in "
                         "%.2f ms\n",
                         path.c_str(), entries.size(), from_name, ms_since(start));
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rummage-mime: %s\n", e.what());
        return 1;
    }
}
