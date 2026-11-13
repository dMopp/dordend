// Finding files benchmark (ROADMAP 8b). Not installed.
//
//   rummage-bench-find [--hidden] [--runs N] DIR QUERY...
//
// Per run and query, as Ctrl+F does it: FileFinder starts listing DIR on the worker pool and the
// query is set at once (typed as the palette opens). Prints the time to the first results, to the
// end of the listing, and how many entries; then the time to match the whole list again with each
// query (a new query typed once the listing is done). Run 1 after dropping the caches is the cold
// number (`echo 3 | sudo tee /proc/sys/vm/drop_caches` before it).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/memory.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/views/file_finder.hpp"

namespace {

namespace core = rummage::core;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
    try {
        core::give_back_large_blocks();
        bool hidden = false;
        int runs = 3;
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--hidden") {
                hidden = true;
            } else if (arg == "--runs" && i + 1 < argc) {
                runs = std::atoi(argv[++i]);
            } else {
                args.emplace_back(arg);
            }
        }
        if (args.size() < 2) {
            std::fputs("usage: rummage-bench-find [--hidden] [--runs N] DIR QUERY...\n", stderr);
            return 2;
        }
        const std::filesystem::path root = args[0];
        const std::vector<std::string> queries(args.begin() + 1, args.end());

        core::EventLoop loop;
        core::WorkerPool pool{loop};
        const auto anonymous_mb = [] {
            return static_cast<double>(core::read_memory_report().anonymous) / 1e6;
        };
        std::printf("anonymous memory at start: %.1f MB\n", anonymous_mb());
        std::printf("%u workers\n", static_cast<unsigned>(pool.thread_count()));
        for (int run = 1; run <= runs; ++run) {
            for (const std::string& query : queries) {
                double first = -1.0;
                double done = -1.0;
                bool listening = true;  // the first phase: the handler ends the loop
                const Clock::time_point start = Clock::now();
                rummage::views::FileFinder finder{
                    pool, [&] {
                        if (first < 0.0 && !finder.results().empty()) {
                            first = ms_since(start);
                        }
                        if (listening && !finder.scanning() && !finder.matching()) {
                            listening = false;
                            done = ms_since(start);
                            loop.quit(0);
                        }
                    }};
                finder.start(root, hidden);
                finder.set_query(query);
                const auto watchdog = loop.add_timer(120s, 0ms, [&loop] { loop.quit(1); });
                loop.run();
                loop.remove(watchdog);
                const std::string best =
                    finder.results().empty() ? std::string{"-"} : finder.results().front().path;

                // The same query again over the whole list, as when typed after the listing.
                finder.set_query({});
                const Clock::time_point again = Clock::now();
                double rematch = -1.0;
                finder.set_query(query);
                if (finder.matching()) {
                    // Answers come through the changed handler above: wait for the last.
                    const auto poll = loop.add_timer(1ms, 1ms, [&] {
                        if (!finder.matching()) {
                            rematch = ms_since(again);
                            loop.quit(0);
                        }
                    });
                    loop.run();
                    loop.remove(poll);
                }
                std::printf(
                    "run %d  %-12s first %8.2f ms  listed %9.2f ms  %8zu entries%s  "
                    "match all %7.2f ms  paths %5.1f MB  best: %s\n",
                    run, query.c_str(), first, done, finder.entries(),
                    finder.walked().truncated ? " (stopped)" : "", rematch,
                    static_cast<double>(finder.bytes()) / 1e6, best.c_str());
                std::printf("  anonymous memory with the paths: %.1f MB\n", anonymous_mb());
            }
            core::give_back_free_memory();
            std::printf("  anonymous memory once let go: %.1f MB\n", anonymous_mb());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rummage-bench-find: %s\n", e.what());
        return 1;
    }
}
