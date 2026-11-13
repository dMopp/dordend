#include "rummage/views/state.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::fs::SortKey;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::format_state;
using rummage::views::load_state;
using rummage::views::parse_state;
using rummage::views::State;
using rummage::views::StateKeeper;
using rummage::views::StateResult;
using namespace std::chrono_literals;

namespace {

StateResult parse(std::string_view text) { return parse_state(text, "state.toml"); }

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

// Every value away from its default.
State changed() {
    State s;
    s.mode = State::Mode::details;
    s.show_hidden = true;
    s.sidebar_visible = false;
    s.sidebar_width = 260.5F;
    s.sidebar_size = 4;
    s.info_visible = false;
    s.info_width = 410.0F;
    s.info_expanded = true;
    s.sort_key = SortKey::modified;
    s.sort_descending = true;
    s.details_widths = {80.0F, 170.0F, 240.0F};
    s.list_size = 3;
    s.grid_icon = 192;
    s.window_width = 1400;
    s.window_height = 900;
    s.maximized = true;
    return s;
}

}  // namespace

TEST_CASE("state: written and read back the same") {
    const StateResult result = parse(format_state(changed()));
    CHECK(result.problems.empty());
    CHECK(result.state == changed());

    const StateResult defaults = parse(format_state(State{}));
    CHECK(defaults.problems.empty());
    CHECK(defaults.state == State{});
}

TEST_CASE("state: empty or missing is the defaults, quietly") {
    CHECK(parse("").state == State{});
    CHECK(parse("").problems.empty());
    const TempDir dir;
    const StateResult missing = load_state(dir.path / "none" / "state.toml");
    CHECK(missing.state == State{});
    CHECK(missing.problems.empty());
}

TEST_CASE("state: bad values keep their default, each reported where it is") {
    const StateResult result = parse(R"([view]
mode = "tree"
show_hidden = "yes"

[sidebar]
width = 5

[window]
width = 100000
height = 700
)");
    CHECK(result.state.mode == State::Mode::columns);
    CHECK_FALSE(result.state.show_hidden);
    CHECK(result.state.sidebar_width == State{}.sidebar_width);
    CHECK(result.state.window_width == State{}.window_width);
    CHECK(result.state.window_height == 700);  // the good one is taken
    REQUIRE(result.problems.size() == 4);
    CHECK(result.problems[0].starts_with("state.toml:2:"));
    CHECK(result.problems[0].find("view.mode") != std::string::npos);
    CHECK(result.problems[1].starts_with("state.toml:3:"));
    CHECK(result.problems[2].starts_with("state.toml:6:"));
    CHECK(result.problems[3].starts_with("state.toml:9:"));
}

TEST_CASE("state: a broken file is the defaults; unknown keys are fine") {
    const StateResult broken = parse("[view\nmode = \"grid\"\n");
    CHECK(broken.state == State{});
    REQUIRE(broken.problems.size() == 1);
    CHECK(broken.problems[0].starts_with("state.toml:1:"));

    const StateResult newer = parse("[view]\nmode = \"grid\"\nzoom = 3\n[tabs]\nopen = 2\n");
    CHECK(newer.problems.empty());
    CHECK(newer.state.mode == State::Mode::grid);
}

TEST_CASE("state: the file lives under XDG_STATE_HOME") {
    const char* home = std::getenv("XDG_STATE_HOME");  // NOLINT(concurrency-mt-unsafe)
    REQUIRE(home != nullptr);  // tests/main.cpp points it into the test root
    CHECK(rummage::views::default_state_path() ==
          std::filesystem::path{home} / "rummage" / "state.toml");
}

TEST_CASE("state: the browser gives back what it was given") {
    const TempDir dir;
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    CHECK(browser.state() == State{});  // the browser's defaults are the file's

    State wanted = changed();
    browser.apply(wanted);
    browser.open(dir.path);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    CHECK(browser.mode() == Browser::Mode::details);
    CHECK(browser.shows_hidden());
    CHECK_FALSE(browser.sidebar_visible());
    CHECK(browser.details().folder()->order().key == SortKey::modified);
    // The window's part is main's: the browser leaves it at its defaults.
    wanted.window_width = State{}.window_width;
    wanted.window_height = State{}.window_height;
    wanted.maximized = false;
    CHECK(browser.state() == wanted);

    // Narrower than a column's minimum: the view keeps its minimum.
    State narrow = wanted;
    narrow.details_widths = {10.0F, 10.0F, 10.0F};
    browser.apply(narrow);
    for (const float width : browser.state().details_widths) {
        CHECK(width >= 64.0F);
    }
    // After open(), a mode applied is as if chosen.
    State grid = wanted;
    grid.mode = State::Mode::grid;
    browser.apply(grid);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    CHECK(browser.mode() == Browser::Mode::grid);
}

TEST_CASE("StateKeeper: one write after the last change, flush writes what is left") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "rummage" / "state.toml";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    StateKeeper keeper{loop, pool, file, State{}, 20ms};

    keeper.note(State{});  // nothing changed: nothing to write
    State first;
    first.mode = State::Mode::grid;
    State second = first;
    second.show_hidden = true;
    keeper.note(first);
    keeper.note(second);  // within the delay: only this one is written
    REQUIRE(run_until(loop, [&] { return keeper.saved() == second; }));
    REQUIRE(run_until(loop, [&] { return load_state(file).state == second; }));

    State third = second;
    third.sidebar_width = 300.0F;
    keeper.note(third);
    keeper.flush();  // at exit: written now, not after the delay
    CHECK(load_state(file).state == third);
    CHECK(load_state(file).problems.empty());
}

TEST_CASE("state: tabs written and read back, odd names too; one not UTF-8 left out") {
    State s = changed();
    s.tabs = {{.folder = "/home/me/Pictures",
               .mode = State::Mode::columns,
               .root = "/home/me",
               .cursor = "P1 \"2\".ORF",
               .list_size = 3,
               .grid_icon = 192},
              {.folder = "/home/me/a \"quoted\" \\ name\twith tab/città",
               .mode = State::Mode::grid,
               .root = {},
               .cursor = {},
               .list_size = {},
               .grid_icon = {}},
              {.folder = "/mnt/x",
               .mode = State::Mode::details,
               .root = {},
               .cursor = {},
               .list_size = {},
               .grid_icon = {}}};
    s.shown_tab = 1;
    const std::string text = format_state(s);
    const StateResult back = parse(text);
    CHECK(back.problems.empty());
    CHECK(back.state == s);

    // TOML holds only UTF-8: a tab on a name that is not is not written.
    State odd;
    odd.tabs = {{.folder = "/home/me/ok",
                 .mode = State::Mode::grid,
                 .root = {},
                 .cursor = {},
                 .list_size = {},
                 .grid_icon = {}},
                {.folder = std::string{"/home/me/\xff\xfe"},
                 .mode = State::Mode::grid,
                 .root = {},
                 .cursor = {},
                 .list_size = {},
                 .grid_icon = {}}};
    odd.shown_tab = 0;
    const StateResult kept = parse(format_state(odd));
    CHECK(kept.problems.empty());
    REQUIRE(kept.state.tabs.size() == 1);
    CHECK(kept.state.tabs[0].folder == "/home/me/ok");
}

TEST_CASE("state: a tab that cannot be used is left out, and said where") {
    const StateResult result = parse(R"([[tab]]
folder = "relative/path"

[[tab]]
folder = "/ok"
mode = "tree"

[[tab]]
folder = "/fine"
mode = "details"
root = "nope"

[[tab]]
folder = "/kept"
mode = "grid"
shown = true
)");
    REQUIRE(result.state.tabs.size() == 1);
    CHECK(result.state.tabs[0].folder == "/kept");
    CHECK(result.state.tabs[0].mode == State::Mode::grid);
    CHECK(result.state.shown_tab == 0);
    REQUIRE(result.problems.size() == 3);
    CHECK(result.problems[0].starts_with("state.toml:1:"));
    CHECK(result.problems[1].starts_with("state.toml:6:"));
    CHECK(result.problems[2].starts_with("state.toml:11:"));

    const StateResult wrong = parse("tab = 3\n");
    CHECK(wrong.state.tabs.empty());
    CHECK(wrong.problems.size() == 1);
}

TEST_CASE("state: the browser's tabs, kept and brought back as they were") {
    const TempDir dir;
    std::filesystem::create_directories(dir.path / "a" / "deep" / "er");
    std::filesystem::create_directories(dir.path / "b");
    std::filesystem::create_directories(dir.path / "gone");
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    const auto make = [&] {
        return std::make_unique<Browser>(
            loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {},
            [](rummage::ui::CursorShape /*shape*/) {});
    };
    State saved;
    {
        const std::unique_ptr<Browser> browser = make();
        browser->open(dir.path / "a");
        REQUIRE(run_until(loop, [&] { return !browser->loading(); }));
        CHECK(browser->state().tabs.empty());  // one tab: nothing to restore
        browser->reveal(dir.path / "a", dir.path / "a" / "deep" / "er");  // a > deep > er
        REQUIRE(run_until(loop, [&] {
            return !browser->loading() && !browser->columns().revealing() &&
                   browser->columns().column_count() >= 2;
        }));
        const std::filesystem::path focused = browser->columns().focused_folder()->path();
        CHECK(focused.string().starts_with((dir.path / "a").string()));
        browser->open_tab(dir.path / "gone", false);
        browser->open_tab(dir.path / "b", true);
        browser->set_mode(Browser::Mode::grid);
        REQUIRE(run_until(loop, [&] { return !browser->loading(); }));
        saved = browser->state();
    }
    REQUIRE(saved.tabs.size() == 3);
    CHECK(saved.tabs[0].mode == State::Mode::columns);
    CHECK(saved.tabs[0].root == dir.path / "a");
    CHECK(saved.tabs[1].folder == dir.path / "b");
    CHECK(saved.tabs[1].mode == State::Mode::grid);
    CHECK(saved.tabs[2].folder == dir.path / "gone");
    CHECK(saved.shown_tab == 1);
    // Through the file, as at the next start.
    const StateResult read = parse(format_state(saved));
    REQUIRE(read.problems.empty());
    std::filesystem::remove(dir.path / "gone");

    const std::unique_ptr<Browser> browser = make();
    browser->apply(read.state);
    REQUIRE(browser->restore_tabs(read.state.tabs, read.state.shown_tab));
    REQUIRE(run_until(loop, [&] { return !browser->loading(); }));
    CHECK(browser->tab_count() == 2);  // "gone" is gone
    CHECK(browser->active_tab() == 1);
    CHECK(browser->tab_titles().size() == 2);
    CHECK(browser->tab_titles()[0] == saved.tabs[0].folder.filename().string());
    CHECK(browser->tab_titles()[1] == "b");
    CHECK(browser->mode() == Browser::Mode::grid);
    browser->select_tab(0);
    CHECK(browser->mode() == Browser::Mode::columns);
    REQUIRE(run_until(loop, [&] { return browser->columns().column_count() >= 2; }));
    CHECK(browser->columns().column(0).folder().path() == dir.path / "a");

    // Nothing left to restore: false, and the start opens its folder.
    const std::unique_ptr<Browser> empty = make();
    CHECK_FALSE(empty->restore_tabs({{.folder = dir.path / "gone",
                                      .mode = State::Mode::grid,
                                      .root = {},
                                      .cursor = {},
                                      .list_size = {},
                                      .grid_icon = {}}},
                                    0));
}

TEST_CASE("state: the lists' and the grid's size, and what is not one") {
    for (const int size : {384, 512}) {
        State large;
        large.grid_icon = size;
        const auto restored = parse(format_state(large));
        CHECK(restored.problems.empty());
        CHECK(restored.state.grid_icon == size);
        CHECK(Browser::grid_index(size) != rummage::views::kUsualGridSize);
    }
    const StateResult good = parse("[view]\nlist_size = \"largest\"\ngrid_icon = 48\n");
    CHECK(good.problems.empty());
    CHECK(good.state.list_size == 4);
    CHECK(good.state.grid_icon == 48);
    const StateResult bad = parse("[view]\nlist_size = \"huge\"\ngrid_icon = 100\n");
    CHECK(bad.state.list_size == State{}.list_size);
    CHECK(bad.state.grid_icon == State{}.grid_icon);
    REQUIRE(bad.problems.size() == 2);
    CHECK(bad.problems[0].find("view.list_size") != std::string::npos);
    CHECK(bad.problems[1].find("view.grid_icon") != std::string::npos);
}

TEST_CASE("state: a tab written before tabs had a zoom takes the window's") {
    const TempDir dir;
    std::filesystem::create_directories(dir.path / "a");
    std::filesystem::create_directories(dir.path / "b");
    const StateResult read =
        parse(std::format(R"([view]
list_size = "larger"
grid_icon = 128

[[tab]]
folder = "{}"
mode = "details"

[[tab]]
folder = "{}"
mode = "details"
list_size = "small"
shown = true
)",
                          (dir.path / "a").string(), (dir.path / "b").string()));
    REQUIRE(read.problems.empty());
    CHECK_FALSE(read.state.tabs[0].list_size);
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.apply(read.state);
    REQUIRE(browser.restore_tabs(read.state.tabs, read.state.shown_tab));
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    CHECK(browser.details().list_size() == 0);  // "small", its own
    browser.select_tab(0);
    CHECK(browser.details().list_size() == 3);  // "larger", the window's
    CHECK(browser.grid().icon_size() == 128.0F);
}

TEST_CASE("state: sidebar zoom defaults and validation are independent of the view") {
    CHECK(parse("[view]\nlist_size = \"largest\"\n").state.sidebar_size == 1);
    const auto good = parse("[sidebar]\nlist_size = \"larger\"\n");
    CHECK(good.problems.empty());
    CHECK(good.state.sidebar_size == 3);
    CHECK(good.state.list_size == 1);
    const auto bad = parse("[sidebar]\nlist_size = \"huge\"\n");
    CHECK(bad.state.sidebar_size == 1);
    REQUIRE(bad.problems.size() == 1);
    CHECK(bad.problems[0].find("sidebar.list_size") != std::string::npos);
}
