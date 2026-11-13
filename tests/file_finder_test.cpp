#include "rummage/views/file_finder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/walk.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/command_palette.hpp"
#include "rummage/views/fuzzy.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::PathBatch;
using rummage::fs::walk;
using rummage::fs::WalkOptions;
using rummage::fs::WalkResult;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::FileFinder;
using rummage::views::FoundFile;
using rummage::views::FuzzyQuery;
using rummage::views::match_paths;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream{path} << "x";
}

// a.txt, photos/{shot.orf, shot.orf.xmp, deep/deeper/far.orf}, notes/orf-list.txt, .hidden/
// secret.orf, .dotfile, link -> photos.
void make_tree(const fs::path& root) {
    touch(root / "a.txt");
    touch(root / "photos" / "shot.orf");
    touch(root / "photos" / "shot.orf.xmp");
    touch(root / "photos" / "deep" / "deeper" / "far.orf");
    touch(root / "notes" / "orf-list.txt");
    touch(root / ".hidden" / "secret.orf");
    touch(root / ".dotfile");
    fs::create_directory_symlink(root / "photos", root / "link");
}

struct Walked {
    std::vector<std::string> paths;
    std::vector<bool> directories;
    WalkResult result;
};

Walked walk_all(const fs::path& root, WalkOptions options, const CancelToken& token = {}) {
    Walked out;
    out.result = walk(root, options, token, [&out](const PathBatch& batch) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            out.paths.emplace_back(batch.path(i));
            out.directories.push_back(batch.directory(i));
        }
    });
    return out;
}

std::size_t index_of(const std::vector<std::string>& paths, std::string_view path) {
    return static_cast<std::size_t>(std::ranges::find(paths, path) - paths.begin());
}

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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

constexpr Modifiers kCtrl{.ctrl = true};

}  // namespace

TEST_CASE("walk: breadth first, relative paths, no dotfiles, links listed not followed") {
    const TempDir dir;
    make_tree(dir.path);
    const Walked walked = walk_all(dir.path, {.batch = 2});
    CHECK(walked.result.entries == walked.paths.size());
    CHECK_FALSE(walked.result.truncated);
    std::vector<std::string> sorted = walked.paths;
    std::ranges::sort(sorted);
    CHECK(sorted == std::vector<std::string>{"a.txt", "link", "notes", "notes/orf-list.txt",
                                             "photos", "photos/deep", "photos/deep/deeper",
                                             "photos/deep/deeper/far.orf", "photos/shot.orf",
                                             "photos/shot.orf.xmp"});
    // The nearest first: every entry of a folder before those of its subfolders.
    CHECK(index_of(walked.paths, "a.txt") < index_of(walked.paths, "photos/shot.orf"));
    CHECK(index_of(walked.paths, "photos/shot.orf") < index_of(walked.paths, "photos/deep/deeper"));
    CHECK(walked.directories[index_of(walked.paths, "photos")]);
    CHECK_FALSE(walked.directories[index_of(walked.paths, "link")]);  // a link, not entered
    CHECK_FALSE(walked.directories[index_of(walked.paths, "a.txt")]);

    const Walked hidden = walk_all(dir.path, {.hidden = true});
    CHECK(std::ranges::find(hidden.paths, ".hidden/secret.orf") != hidden.paths.end());
    CHECK(std::ranges::find(hidden.paths, ".dotfile") != hidden.paths.end());
}

TEST_CASE("walk: stops at the limits, when cancelled, and on a missing folder") {
    const TempDir dir;
    make_tree(dir.path);
    const Walked few = walk_all(dir.path, {.max_entries = 3});
    CHECK(few.paths.size() == 3);
    CHECK(few.result.truncated);
    const Walked shallow = walk_all(dir.path, {.max_depth = 1});
    CHECK(std::ranges::find(shallow.paths, "photos/shot.orf") == shallow.paths.end());
    CHECK(std::ranges::find(shallow.paths, "photos") != shallow.paths.end());
    CancelToken token;
    token.cancel();
    const Walked cancelled = walk_all(dir.path, {}, token);
    CHECK(cancelled.result.cancelled);
    CHECK(cancelled.paths.empty());
    const Walked missing = walk_all(dir.path / "nothing", {});
    CHECK(missing.paths.empty());
    CHECK(missing.result.unreadable == 1);
}

TEST_CASE("fuzzy paths: a match in the name ranks above one across folders") {
    FuzzyQuery query{"orf"};
    const auto score = [&query](std::string_view path) {
        const std::optional<double> found = query.path_score(path);
        REQUIRE(found);
        return found.value_or(0.0);
    };
    CHECK(score("photos/shot.orf") > score("orf-notes/list.txt"));
    CHECK(score("x/y/z/far.orf") > score("orfanage/a.txt"));
    CHECK_FALSE(query.path_score("photos/a.txt"));
    // Highlighted in the name when it matches there.
    CHECK(query.path_positions("orf/shot.orf") == std::vector<std::size_t>{9, 10, 11});
    CHECK(query.path_positions("orf/notes.txt") == std::vector<std::size_t>{0, 1, 2});
    // Spaces mean nothing: "monte rosa" finds "MonteRosa".
    CHECK(FuzzyQuery{"monte rosa"}.score("MonteRosa"));
}

TEST_CASE("match_paths: the best few, ordered, the shorter first among equals") {
    auto batch = std::make_shared<PathBatch>();
    for (const char* path : {"orf-notes/list.txt", "photos/deep/deeper/far.orf", "photos/shot.orf",
                             "a.txt", "shot.orf"}) {
        batch->add(path, false);
    }
    const std::vector<std::shared_ptr<const PathBatch>> batches{batch};
    const std::vector<FoundFile> found = match_paths(batches, "orf", 10, {});
    REQUIRE(found.size() == 4);
    CHECK(found.back().path == "orf-notes/list.txt");  // only across the folders
    const std::vector<FoundFile> two = match_paths(batches, "orf", 2, {});
    REQUIRE(two.size() == 2);
    CHECK(two[0].path == found[0].path);
    CHECK(two[1].path == found[1].path);
    CancelToken token;
    token.cancel();
    CHECK(match_paths(batches, "orf", 10, token).empty());
}

TEST_CASE("file finder: lists once, a query typed at once is answered, the nearest when empty") {
    const TempDir dir;
    make_tree(dir.path);
    EventLoop loop;
    WorkerPool pool{loop, 2};
    int changes = 0;
    FileFinder finder{pool, [&changes] { ++changes; }, {.batch = 2}};
    finder.start(dir.path, false);
    finder.set_query("far");  // before anything is listed
    REQUIRE(run_until(loop, [&] { return !finder.scanning() && !finder.matching(); }));
    CHECK(changes > 0);
    REQUIRE_FALSE(finder.results().empty());
    CHECK(finder.results().front().path == "photos/deep/deeper/far.orf");
    CHECK(finder.entries() == 10);
    CHECK(finder.status() == "10 items");

    finder.set_query("shot");
    REQUIRE(run_until(loop, [&] { return !finder.matching(); }));
    REQUIRE(finder.results().size() == 2);
    CHECK(finder.results().front().path == "photos/shot.orf");  // the shorter first

    finder.set_query(" ");  // nothing to match: the nearest
    CHECK(finder.results().size() == 10);
    CHECK(finder.results().front().path.find('/') == std::string::npos);
    CHECK(finder.bytes() > 0);
}

namespace {

// Two folders: a/{one.txt, photos/{shot.orf, deep/far.orf}}, in the grid at a.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    EventLoop loop;
    WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](CursorShape /*shape*/) {}};

    Fixture() {
        touch(a / "one.txt");
        touch(a / "photos" / "shot.orf");
        touch(a / "photos" / "deep" / "far.orf");
        browser.set_home_trash(dir.path / "Trash");
        browser.open(a);
        REQUIRE(settle());
        browser.set_mode(Browser::Mode::grid);
        REQUIRE(settle());
        REQUIRE(browser.mode() == Browser::Mode::grid);
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    // Ctrl+F, query typed, the list filled.
    void find(const std::string& query) {
        REQUIRE(press(key(XKB_KEY_f, kCtrl)));
        REQUIRE(browser.command_palette().external());
        REQUIRE(browser.file_finder() != nullptr);
        browser.command_palette().set_query(query);
        REQUIRE(run_until(loop, [this] {
            const FileFinder* finder = browser.file_finder();
            return finder != nullptr && !finder->scanning() && !finder->matching();
        }));
    }

    // The settle_found() that draw() would run, until the file is shown.
    bool shown(const std::string& name) {
        return run_until(loop, [this, &name] {
            browser.settle_found(Browser::Clock::now());
            const std::shared_ptr<rummage::views::Folder> folder = browser.grid().folder();
            return folder && folder->cursor_entry() != nullptr &&
                   folder->cursor_entry()->name == name;
        });
    }
};

}  // namespace

TEST_CASE("find files: Ctrl+F lists the folder, Enter shows the file in the view") {
    Fixture f;
    f.find("far");
    const auto& palette = f.browser.command_palette();
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(palette.items()[palette.shown().front()].label == "photos/deep/far.orf");
    CHECK(palette.items().front().path);
    CHECK(palette.status() == "5 items");
    f.press(key(XKB_KEY_Return));
    CHECK_FALSE(palette.is_open());
    CHECK(f.browser.file_finder() == nullptr);  // let go once closed
    REQUIRE(f.shown("far.orf"));
    CHECK(f.browser.grid().folder()->path() == f.a / "photos" / "deep");
    CHECK(f.browser.mode() == Browser::Mode::grid);
}

TEST_CASE("find files: Space shows the file in Quick Look; folders say so") {
    Fixture f;
    f.find("shot");
    f.press(key(XKB_KEY_space, {}, " "));
    CHECK_FALSE(f.browser.command_palette().is_open());
    REQUIRE(f.shown("shot.orf"));
    REQUIRE(run_until(f.loop, [&f] {
        f.browser.settle_found(Browser::Clock::now());
        return f.browser.quick_look().is_open();
    }));
    f.browser.quick_look().close();

    f.find("deep");
    const auto& palette = f.browser.command_palette();
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(palette.items()[palette.shown().front()].keys == "Folder");
}

TEST_CASE("find files: Ctrl+F again closes, Ctrl+K switches, the palette offers it") {
    Fixture f;
    REQUIRE(f.press(key(XKB_KEY_f, kCtrl)));
    REQUIRE(f.browser.command_palette().external());
    f.press(key(XKB_KEY_k, kCtrl));
    CHECK(f.browser.command_palette().is_open());
    CHECK_FALSE(f.browser.command_palette().external());
    CHECK(f.browser.file_finder() == nullptr);
    f.press(key(XKB_KEY_f, kCtrl));
    CHECK(f.browser.command_palette().external());
    f.press(key(XKB_KEY_f, kCtrl));
    CHECK_FALSE(f.browser.command_palette().is_open());
    // From the command palette, "Find Files…".
    f.browser.open_palette();
    f.browser.command_palette().set_query("find files");
    f.press(key(XKB_KEY_Return));
    CHECK(f.browser.command_palette().external());
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.command_palette().is_open());
}

TEST_CASE("find files: in the columns, the way down from the first column stays") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    f.find("far");
    f.press(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&f] {
        const std::shared_ptr<rummage::views::Folder> folder = f.browser.columns().focused_folder();
        return folder && folder->path() == f.a / "photos" / "deep" &&
               folder->cursor_entry() != nullptr && folder->cursor_entry()->name == "far.orf";
    }));
    CHECK(f.browser.columns().column(0).folder().path() == f.a);
    CHECK(f.browser.columns().column_count() == 3);  // a, photos, deep
}
