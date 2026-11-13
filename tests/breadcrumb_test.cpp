#include "rummage/views/breadcrumb.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::Crumb;
using rummage::views::crumbs_of;
using rummage::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

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

std::string cursor_name(const Folder& folder) {
    const rummage::fs::Entry* entry = folder.cursor_entry();
    return entry == nullptr ? std::string{} : entry->name;
}

// root/a/b/c
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    Fixture() { fs::create_directories(root / "a" / "b" / "c"); }

    // The folder shown has loaded and its cursor landed.
    bool settled() {
        return run_until(loop, [&] { return !browser.loading(); });
    }
};

}  // namespace

TEST_CASE("breadcrumb: steps from the home or the root") {
    CHECK(crumbs_of("/home/me/Pictures/2026", "/home/me") ==
          std::vector<Crumb>{{.path = "/home/me", .label = "", .icon = "home"},
                             {.path = "/home/me/Pictures", .label = "Pictures", .icon = ""},
                             {.path = "/home/me/Pictures/2026", .label = "2026", .icon = ""}});
    CHECK(crumbs_of("/home/me", "/home/me") ==
          std::vector<Crumb>{{.path = "/home/me", .label = "", .icon = "home"}});
    CHECK(crumbs_of("/etc/xdg/", "/home/me") ==
          std::vector<Crumb>{{.path = "/", .label = "", .icon = "server"},
                             {.path = "/etc", .label = "etc", .icon = ""},
                             {.path = "/etc/xdg", .label = "xdg", .icon = ""}});
    CHECK(crumbs_of("/", "/home/me") ==
          std::vector<Crumb>{{.path = "/", .label = "", .icon = "server"}});
    // A sibling whose name starts like the home is not in it.
    CHECK(crumbs_of("/home/me2", "/home/me").front().icon == "server");
    CHECK(crumbs_of("/home/me/x", "").front().icon == "server");
}

TEST_CASE("breadcrumb: going up in the grid and the details") {
    for (const Browser::Mode mode : {Browser::Mode::grid, Browser::Mode::details}) {
        Fixture f;
        f.browser.open(f.root / "a" / "b" / "c");
        f.browser.set_mode(mode);
        REQUIRE(f.settled());
        f.browser.go_to(f.root / "a", "b");
        REQUIRE(f.settled());
        const std::shared_ptr<Folder> folder =
            mode == Browser::Mode::grid ? f.browser.grid().folder() : f.browser.details().folder();
        CHECK(folder->path() == f.root / "a");
        CHECK(cursor_name(*folder) == "b");
        CHECK(f.browser.mode() == mode);
    }
}

TEST_CASE("breadcrumb: going up in the columns") {
    Fixture f;
    f.browser.reveal(f.root, f.root / "a" / "b" / "c");
    REQUIRE(f.settled());
    auto& columns = f.browser.columns();
    REQUIRE(columns.column_count() >= 3);

    // A folder with a column: that column is focused, its cursor still on the way down; the
    // folder we came from shows again from the top, the columns after it close.
    f.browser.go_to(f.root / "a", "b");
    CHECK(columns.focused_folder()->path() == f.root / "a");
    CHECK(cursor_name(*columns.focused_folder()) == "b");
    REQUIRE(columns.column_count() == 3);
    CHECK(columns.column(2).folder().path() == f.root / "a" / "b");
    REQUIRE(f.settled());
    CHECK_FALSE(columns.column(2).folder().cursor().has_value());
    CHECK(columns.column_count() == 3);

    // A folder above the first column: columns from it, the cursor on where we came from.
    f.browser.open(f.root / "a" / "b");
    REQUIRE(f.settled());
    f.browser.go_to(f.root, "a");
    REQUIRE(f.settled());
    CHECK(columns.column(0).folder().path() == f.root);
    CHECK(cursor_name(columns.column(0).folder()) == "a");
}
