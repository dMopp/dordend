#include "rummage/fs/directory_model.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::DirectoryModel;
using rummage::fs::DirectoryWatcher;
using rummage::fs::Entry;
using rummage::fs::ModelChange;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

// Runs the loop until done() holds (checked after every iteration's timer) or 5 s pass.
bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

void run_for(EventLoop& loop, std::chrono::milliseconds time) {
    loop.add_timer(time, 0ms, [&loop] { loop.quit(0); });
    loop.run();
}

std::vector<std::string> names(const DirectoryModel& model) {
    std::vector<std::string> out;
    for (const Entry& entry : model.listing().entries()) {
        out.push_back(entry.name);
    }
    return out;
}

const Entry* find(const DirectoryModel& model, std::string_view name) {
    const auto& entries = model.listing().entries();
    const auto it = std::ranges::find(entries, name, &Entry::name);
    return it == entries.end() ? nullptr : &*it;
}

// A model on a temporary directory, with every change recorded.
struct Fixture {
    TempDir dir;
    EventLoop loop;
    WorkerPool pool{loop, 2};
    DirectoryWatcher watcher{loop};
    DirectoryModel model{loop, pool, watcher, rummage::fs::MimeDatabase::load(), 10ms};
    std::vector<ModelChange> changes;
    bool loaded = false;

    Fixture() {
        model.set_change_handler([this](const ModelChange& change) {
            changes.push_back(change);
            loaded = loaded || change.loaded;
        });
    }

    void open() {
        model.open(dir.path);
        REQUIRE(run_until(loop, [this] { return loaded; }));
        changes.clear();
    }

    // Waits until the listing satisfies done, then a little longer to catch extra notifications.
    void settle(const std::function<bool()>& done) {
        REQUIRE(run_until(loop, done));
        run_for(loop, 40ms);
    }

    [[nodiscard]] std::vector<std::string> all(
        std::vector<std::string> ModelChange::* field) const {
        std::vector<std::string> out;
        for (const ModelChange& change : changes) {
            out.insert(out.end(), (change.*field).begin(), (change.*field).end());
        }
        std::ranges::sort(out);
        return out;
    }
};

}  // namespace

TEST_CASE("model: initial load, sorted, then loaded") {
    Fixture f;
    f.dir.write("b.txt", "b");
    f.dir.write("a10.txt", "a");
    f.dir.write("a9.txt", "a");
    std::filesystem::create_directory(f.dir.path / "sub");
    f.model.open(f.dir.path);
    CHECK(f.model.loading());
    REQUIRE(run_until(f.loop, [&] { return f.loaded; }));
    CHECK_FALSE(f.model.loading());
    CHECK_FALSE(f.model.error());
    CHECK(f.changes.front().reset);
    CHECK(names(f.model) == std::vector<std::string>{"sub", "a9.txt", "a10.txt", "b.txt"});
    CHECK(f.model.listing().entries().front().mime == rummage::fs::MimeDatabase::kDirectory);
}

TEST_CASE("model: create, write, delete, mkdir") {
    Fixture f;
    f.dir.write("keep.txt", "1");
    f.open();

    f.dir.write("new.txt", "hello");
    std::filesystem::create_directory(f.dir.path / "folder");
    f.settle([&] { return find(f.model, "new.txt") != nullptr && find(f.model, "folder"); });
    CHECK(f.all(&ModelChange::added) == std::vector<std::string>{"folder", "new.txt"});
    CHECK(find(f.model, "new.txt")->size == 5);
    CHECK(find(f.model, "folder")->is_directory());

    f.changes.clear();
    f.dir.write("keep.txt", "now longer");
    f.settle([&] {
        const Entry* keep = find(f.model, "keep.txt");
        return keep != nullptr && keep->size == 10;
    });
    CHECK(f.all(&ModelChange::updated) == std::vector<std::string>{"keep.txt"});
    CHECK(f.all(&ModelChange::added).empty());

    f.changes.clear();
    std::filesystem::remove(f.dir.path / "new.txt");
    f.settle([&] { return find(f.model, "new.txt") == nullptr; });
    CHECK(f.all(&ModelChange::removed) == std::vector<std::string>{"new.txt"});
    CHECK(names(f.model) == std::vector<std::string>{"folder", "keep.txt"});
}

TEST_CASE("model: renames inside the directory, and moves in and out") {
    Fixture f;
    f.dir.write("old.txt", "x");
    std::filesystem::create_directory(f.dir.path / "elsewhere");
    f.open();

    std::filesystem::rename(f.dir.path / "old.txt", f.dir.path / "new.txt");
    f.settle([&] { return find(f.model, "new.txt") != nullptr; });
    REQUIRE(f.changes.size() == 1);
    REQUIRE(f.changes[0].renamed.size() == 1);
    CHECK(f.changes[0].renamed[0] == std::pair<std::string, std::string>{"old.txt", "new.txt"});
    CHECK(f.changes[0].added.empty());
    CHECK(f.changes[0].removed.empty());
    CHECK(find(f.model, "old.txt") == nullptr);

    // Out of the watched directory: a removal. Back in: an addition.
    f.changes.clear();
    std::filesystem::rename(f.dir.path / "new.txt", f.dir.path / "elsewhere" / "moved.txt");
    f.settle([&] { return find(f.model, "new.txt") == nullptr; });
    CHECK(f.all(&ModelChange::removed) == std::vector<std::string>{"new.txt"});
    f.changes.clear();
    std::filesystem::rename(f.dir.path / "elsewhere" / "moved.txt", f.dir.path / "back.txt");
    f.settle([&] { return find(f.model, "back.txt") != nullptr; });
    CHECK(f.all(&ModelChange::added) == std::vector<std::string>{"back.txt"});
}

TEST_CASE("model: a burst of 500 files arrives in a few notifications") {
    Fixture f;
    f.open();
    for (int i = 0; i < 500; ++i) {
        f.dir.write("f" + std::to_string(i), "");
    }
    f.settle([&] { return f.model.listing().entries().size() == 500; });
    CHECK(f.all(&ModelChange::added).size() == 500);
    CHECK(f.changes.size() < 20);
    MESSAGE("500 files in " << f.changes.size() << " notifications");
}

TEST_CASE("model: changes during the load are neither lost nor doubled") {
    Fixture f;
    for (int i = 0; i < 300; ++i) {
        f.dir.write("f" + std::to_string(i), "");
    }
    f.model.open(f.dir.path);  // watching and loading
    f.dir.write("during.txt", "");
    std::filesystem::remove(f.dir.path / "f0");
    f.settle([&] { return f.loaded && find(f.model, "during.txt") && !find(f.model, "f0"); });
    CHECK(f.model.listing().entries().size() == 300);
    std::vector<std::string> sorted = names(f.model);
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());  // no duplicates
}

TEST_CASE("model: the directory is deleted, or cannot be opened") {
    Fixture f;
    const std::filesystem::path sub = f.dir.path / "sub";
    std::filesystem::create_directory(sub);
    f.dir.write("sub/a", "");
    f.model.open(sub);
    REQUIRE(run_until(f.loop, [&] { return f.loaded; }));
    std::filesystem::remove_all(sub);
    f.settle([&] { return static_cast<bool>(f.model.error()); });
    CHECK(f.model.error() == std::errc::no_such_file_or_directory);
    CHECK(f.model.listing().entries().empty());

    f.model.open(f.dir.path / "absent");
    CHECK(f.model.error() == std::errc::no_such_file_or_directory);
    CHECK_FALSE(f.model.loading());
}

TEST_CASE("model: two models on one directory share the watch") {
    Fixture f;
    DirectoryModel second{f.loop, f.pool, f.watcher, nullptr, 10ms};
    bool second_loaded = false;
    second.set_change_handler([&](const ModelChange& c) { second_loaded |= c.loaded; });
    f.open();
    second.open(f.dir.path);
    REQUIRE(run_until(f.loop, [&] { return second_loaded; }));

    f.dir.write("x", "");
    f.settle([&] { return find(f.model, "x") && find(second, "x"); });

    second.open(f.dir.path / "absent");  // drops its watch; the first must keep working
    f.dir.write("y", "");
    f.settle([&] { return find(f.model, "y") != nullptr; });
}
