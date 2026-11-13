// Live directory model check (ROADMAP 3.3). Not installed.
//
//   rummage-watch DIR
//
// Loads DIR like a view would, then prints every change as it is applied:
//   + name  added    - name  removed    ~ name  updated    old -> new  renamed
// Updates of more than a few changes are summarised on one line. Ctrl+C quits.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <poll.h>
#include <span>
#include <string>
#include <sys/signalfd.h>
#include <unistd.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_model.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"

namespace {

namespace core = rummage::core;
namespace fs = rummage::fs;
using Clock = std::chrono::steady_clock;

// Updates up to this many changes are listed name by name.
constexpr std::size_t kDetailLimit = 10;

std::string stamp(Clock::time_point start) {
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    return std::format("{:8.3f}", seconds);
}

}  // namespace

int main(int argc, char** argv) {
    const std::span args{argv, static_cast<std::size_t>(argc)};
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: rummage-watch DIR\n");
        return 2;
    }
    try {
        // Ctrl+C through the loop, so every destructor runs.
        sigset_t mask{};
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        const int signals = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

        core::EventLoop loop;
        loop.add_fd(signals, POLLIN, [&](short /*revents*/) { loop.quit(0); });
        core::WorkerPool pool{loop};
        fs::DirectoryWatcher watcher{loop};
        const auto mime = fs::MimeDatabase::load();
        fs::DirectoryModel model{loop, pool, watcher, mime};

        const Clock::time_point start = Clock::now();
        model.set_change_handler([&](const fs::ModelChange& change) {
            const std::string at = stamp(start);
            if (change.loaded) {
                std::printf("%s  loaded: %zu entries; watching (Ctrl+C quits)\n", at.c_str(),
                            model.listing().entries().size());
            }
            if (change.reset && model.error()) {
                std::printf("%s  error: %s\n", at.c_str(), model.error().message().c_str());
            }
            if (model.loading()) {
                return;  // chunks of the initial load: summarised by "loaded"
            }
            const std::size_t count = change.added.size() + change.removed.size() +
                                      change.updated.size() + change.renamed.size();
            if (count == 0) {
                return;
            }
            if (count <= kDetailLimit) {
                for (const std::string& name : change.added) {
                    std::printf("%s  + %s\n", at.c_str(), name.c_str());
                }
                for (const std::string& name : change.removed) {
                    std::printf("%s  - %s\n", at.c_str(), name.c_str());
                }
                for (const std::string& name : change.updated) {
                    std::printf("%s  ~ %s\n", at.c_str(), name.c_str());
                }
                for (const auto& [from, to] : change.renamed) {
                    std::printf("%s    %s -> %s\n", at.c_str(), from.c_str(), to.c_str());
                }
            } else {
                std::printf("%s  update: +%zu added, -%zu removed, ~%zu updated, %zu renamed\n",
                            at.c_str(), change.added.size(), change.removed.size(),
                            change.updated.size(), change.renamed.size());
            }
            std::printf("%s    %zu entries now\n", at.c_str(), model.listing().entries().size());
            std::fflush(stdout);
        });
        model.open(args[1]);
        const int code = loop.run();
        close(signals);
        return code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rummage-watch: %s\n", e.what());
        return 1;
    }
}
