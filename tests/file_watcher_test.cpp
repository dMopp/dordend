#include "rummage/core/file_watcher.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::FileWatcher;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

// Runs the loop for a fixed time: long enough for inotify and the settle timer.
void run_for(EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&loop] { loop.quit(0); });
    loop.run();
}

}  // namespace

TEST_CASE("file watcher: write, other files, coalescing, rename, removal") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "colors.toml";
    EventLoop loop;
    int calls = 0;
    const FileWatcher watcher{loop, file, [&] { ++calls; }, 20ms};

    SUBCASE("several writes in a row are one change") {
        for (int i = 0; i < 5; ++i) {
            dir.write("colors.toml", "x");
        }
        run_for(loop, 150ms);
        CHECK(calls == 1);
    }
    SUBCASE("other files in the directory are ignored") {
        dir.write("config.toml", "x");
        dir.write("colors.toml.tmp", "x");
        run_for(loop, 150ms);
        CHECK(calls == 0);
    }
    SUBCASE("an atomic replacement by rename is seen") {
        const std::filesystem::path temp = dir.write("colors.toml.tmp", "x");
        run_for(loop, 100ms);
        std::filesystem::rename(temp, file);
        run_for(loop, 150ms);
        CHECK(calls == 1);
    }
    SUBCASE("removal is seen, and later changes still are") {
        dir.write("colors.toml", "x");
        run_for(loop, 150ms);
        std::filesystem::remove(file);
        run_for(loop, 150ms);
        dir.write("colors.toml", "y");
        run_for(loop, 150ms);
        CHECK(calls == 3);
    }
}

TEST_CASE("file watcher creates a missing directory") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "rummage" / "colors.toml";
    EventLoop loop;
    int calls = 0;
    const FileWatcher watcher{loop, file, [&] { ++calls; }, 20ms};
    CHECK(std::filesystem::is_directory(file.parent_path()));
    std::ofstream{file} << "x";
    run_for(loop, 150ms);
    CHECK(calls == 1);
}
