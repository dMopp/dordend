// Directory loading benchmark (ROADMAP 3.1). Not installed.
//
//   rummage-bench-dir --make N DIR   create DIR with N files named like a camera folder
//   rummage-bench-dir [--runs N] [--list] DIR
//
// Per run: first the async path the views use (worker pool -> chunks -> sorted Listing on the
// loop thread): time to the first chunk merged and to the complete list; then readdir alone,
// readdir + stat (read_directory), sorting by name and MIME types from names (the async path
// types entries by name too, as the views will). Run 1 is shown apart: after dropping the
// caches its async columns are the cold numbers (the other columns are warm by then). The summary
// is over the other runs.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/sort.hpp"

namespace {

namespace core = rummage::core;
namespace fs = rummage::fs;
using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Sample {
    double readdir_ms{0};
    double read_ms{0};
    double sort_ms{0};
    double mime_ms{0};
    double first_chunk_ms{0};
    double complete_ms{0};
    std::size_t entries{0};
    std::size_t typed{0};  // entries whose type the name alone decides
};

// Camera-folder names: P1234567.ORF with a darktable sidecar for one file in three, some JPEGs,
// a few subdirectories.
int make(std::size_t count, const std::filesystem::path& dir) {
    if (std::filesystem::exists(dir)) {
        std::fprintf(stderr, "%s already exists; give a new directory\n", dir.c_str());
        return 2;
    }
    std::filesystem::create_directories(dir);
    for (std::size_t i = 0; i < count; ++i) {
        std::string name;
        if (i % 100 == 0) {
            std::filesystem::create_directory(dir / std::format("Session {}", i / 100));
            continue;
        }
        switch (i % 3) {
            case 0:
                name = std::format("P{:07}.ORF.xmp", 1000000 + i - 1);
                break;
            case 1:
                name = std::format("P{:07}.ORF", 1000000 + i);
                break;
            default:
                name = std::format("IMG_{}.JPG", i);
                break;
        }
        std::ofstream{dir / name};
    }
    std::printf("created %zu entries in %s\n", count, dir.c_str());
    return 0;
}

double readdir_only(const std::filesystem::path& dir) {
    const Clock::time_point start = Clock::now();
    DIR* handle = opendir(dir.c_str());
    if (handle == nullptr) {
        return -1;
    }
    while (readdir(handle) != nullptr) {
    }
    closedir(handle);
    return ms_since(start);
}

Sample run_once(const std::filesystem::path& dir,
                const std::shared_ptr<const fs::MimeDatabase>& mime) {
    Sample sample;

    // The path the views use goes first, so run 1 measures it on a cold cache.
    {
        core::EventLoop loop;
        core::WorkerPool pool{loop};
        fs::Listing listing;
        const Clock::time_point start = Clock::now();
        fs::load_directory(
            pool, dir,
            [&](std::vector<fs::Entry> chunk) {
                listing.add(std::move(chunk));
                if (sample.first_chunk_ms == 0) {
                    sample.first_chunk_ms = ms_since(start);
                }
            },
            [&](const fs::ReadResult& /*result*/) {
                sample.complete_ms = ms_since(start);
                loop.quit(0);
            },
            {.mime = mime});
        loop.run();
    }

    sample.readdir_ms = readdir_only(dir);

    std::vector<fs::Entry> entries;
    Clock::time_point start = Clock::now();
    const fs::ReadResult result =
        fs::read_directory(dir, core::CancelToken{}, {}, [&](std::vector<fs::Entry> chunk) {
            std::ranges::move(chunk, std::back_inserter(entries));
        });
    sample.read_ms = ms_since(start);
    if (result.error) {
        throw std::runtime_error{std::format("{}: {}", dir.string(), result.error.message())};
    }
    sample.entries = entries.size();

    start = Clock::now();
    std::ranges::sort(entries, fs::SortOrder{});
    sample.sort_ms = ms_since(start);

    // Types from names, as the loader does with a database (no file opened).
    start = Clock::now();
    for (const fs::Entry& entry : entries) {
        sample.typed += mime->by_name(entry.name).certain ? 1U : 0U;
    }
    sample.mime_ms = ms_since(start);
    return sample;
}

void print_row(std::string_view label, const Sample& s) {
    std::printf("%-10s %9.2f %9.2f %9.2f %9.2f %11.2f %10.2f\n", std::string{label}.c_str(),
                s.readdir_ms, s.read_ms, s.sort_ms, s.mime_ms, s.first_chunk_ms, s.complete_ms);
}

}  // namespace

int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    try {
        if (args.size() == 4 && std::string_view{args[1]} == "--make") {
            return make(std::stoul(args[2]), args[3]);
        }
        int runs = 10;
        bool list = false;
        std::filesystem::path dir;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string_view arg{args[i]};
            if (arg == "--runs" && i + 1 < args.size()) {
                runs = std::max(2, std::stoi(args[++i]));
            } else if (arg == "--list") {
                list = true;
            } else {
                dir = arg;
            }
        }
        if (dir.empty()) {
            std::fprintf(stderr,
                         "usage: rummage-bench-dir --make N DIR\n"
                         "       rummage-bench-dir [--runs N] [--list] DIR\n");
            return 2;
        }

        const Clock::time_point load_start = Clock::now();
        const auto mime = fs::MimeDatabase::load();
        const double mime_load_ms = ms_since(load_start);

        std::vector<Sample> samples;
        samples.reserve(static_cast<std::size_t>(runs));
        for (int i = 0; i < runs; ++i) {
            samples.push_back(run_once(dir, mime));
        }
        std::printf("%s: %zu entries, %d runs, %u worker threads\n\n", dir.c_str(),
                    samples.front().entries, runs,
                    std::max(2U, std::thread::hardware_concurrency() / 2));
        std::printf("MIME database loaded in %.2f ms; %zu of %zu entries typed by name alone\n\n",
                    mime_load_ms, samples.front().typed, samples.front().entries);
        std::printf("%-10s %9s %9s %9s %9s %11s %10s\n", "ms", "readdir", "+stat", "sort", "mime",
                    "1st chunk", "complete");
        print_row("run 1", samples.front());
        const std::vector<Sample> rest(samples.begin() + 1, samples.end());
        // Each column summarised on its own.
        const auto summary = [&](double quantile) {
            const auto column = [&](double Sample::* field) {
                std::vector<double> values;
                values.reserve(rest.size());
                for (const Sample& s : rest) {
                    values.push_back(s.*field);
                }
                std::ranges::sort(values);
                return values[static_cast<std::size_t>(quantile *
                                                       static_cast<double>(values.size() - 1))];
            };
            return Sample{.readdir_ms = column(&Sample::readdir_ms),
                          .read_ms = column(&Sample::read_ms),
                          .sort_ms = column(&Sample::sort_ms),
                          .mime_ms = column(&Sample::mime_ms),
                          .first_chunk_ms = column(&Sample::first_chunk_ms),
                          .complete_ms = column(&Sample::complete_ms)};
        };
        print_row("min", summary(0.0));
        print_row("median", summary(0.5));
        print_row("max", summary(1.0));

        if (list) {
            std::vector<fs::Entry> entries;
            fs::read_directory(dir, core::CancelToken{}, {}, [&](std::vector<fs::Entry> chunk) {
                std::ranges::move(chunk, std::back_inserter(entries));
            });
            std::ranges::sort(entries, fs::SortOrder{});
            std::printf("\n");
            for (const fs::Entry& entry : entries) {
                std::printf("%s%s\n", entry.name.c_str(), entry.is_directory() ? "/" : "");
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rummage-bench-dir: %s\n", e.what());
        return 1;
    }
}
