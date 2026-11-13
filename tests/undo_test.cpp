#include "rummage/views/undo.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::FileOperation;
using rummage::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

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

Names listing(const fs::path& directory) {
    Names names;
    std::error_code ignored;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

const Modifiers kCtrl{.ctrl = true};
const Modifiers kCtrlShift{.ctrl = true, .shift = true};

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/}, a trash of its own.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path photos = root / "photos";
    fs::path album = root / "album";
    fs::path trash = dir.path / "Trash";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(photos);
        fs::create_directories(album);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P2.ORF"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(trash);
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    // Presses event and waits for what it started.
    void run(const KeyEvent& event) {
        CHECK(press(event));
        REQUIRE(settle());
    }

    std::shared_ptr<Folder> show(const fs::path& folder, const std::string& cursor_on) {
        browser.open(folder);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
        std::shared_ptr<Folder> shown = browser.details().folder();
        if (!cursor_on.empty()) {
            shown->select_name(cursor_on);
            REQUIRE(run_until(loop, [&] { return shown->cursor_entry() != nullptr; }));
        }
        return shown;
    }

    [[nodiscard]] std::string message() { return browser.status_bar().message(); }
};

}  // namespace

TEST_CASE("undo: a rename, sidecars along; redo; then nothing left to undo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    CHECK(f.press(key(XKB_KEY_NoSymbol, {}, "Lake")));
    f.run(key(XKB_KEY_Return));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.name_editor().active(); }));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “Lake.ORF” is “P1.ORF” again");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "Nothing to undo");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"Lake.ORF", "Lake.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Redone: “P1.ORF” is “Lake.ORF” again");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(f.message() == "Nothing to redo");
    f.run(key(XKB_KEY_z, kCtrl));  // the redo is undone like any rename
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("undo: a move goes back, and comes again on redo") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P1.ORF” back in photos");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.message() == "Redone: “P1.ORF” back in album");
}

TEST_CASE("undo: copies go to the trash, never deleted; redo brings them back") {
    Fixture f;
    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});  // the originals stay
    CHECK(f.message() == "Undone: “P1.ORF” moved to the trash");

    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(listing(f.trash / "files").empty());
    CHECK(f.message() == "Redone: “P1.ORF” restored");
}

TEST_CASE("undo: Delete is undone by restoring; a permanent delete cannot be") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    f.run(key(XKB_KEY_Delete));
    REQUIRE(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.message() == "Undone: “P2.ORF” restored");
    f.run(key(XKB_KEY_Z, kCtrlShift));
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.show(f.photos, "P1.ORF");
    CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
    f.run(key(XKB_KEY_Return));
    REQUIRE(listing(f.photos).empty());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "A permanent delete cannot be undone");
}

TEST_CASE("undo: a new folder goes to the trash while it is empty") {
    Fixture f;
    f.show(f.album, "");
    CHECK(f.press(key(XKB_KEY_n, kCtrlShift)));
    REQUIRE(run_until(f.loop, [&] { return f.browser.name_editor().active(); }));
    CHECK(f.press(key(XKB_KEY_Escape)));  // keeps "New Folder"
    REQUIRE(f.settle());
    REQUIRE(listing(f.album) == Names{"New Folder"});

    // Something put in it: not taken away.
    std::ofstream{f.album / "New Folder" / "note.txt"} << "mine";
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() == "“New Folder” has changed since it was made: not undone");
    CHECK(listing(f.album) == Names{"New Folder"});

    fs::remove(f.album / "New Folder" / "note.txt");
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(listing(f.album).empty());
    CHECK(listing(f.trash / "files") == Names{"New Folder"});
}

TEST_CASE("undo: what changed meanwhile is said, and undo goes on working") {
    Fixture f;
    f.show(f.photos, "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, kCtrl)));
    f.show(f.album, "");
    f.run(key(XKB_KEY_v, kCtrl));
    REQUIRE(listing(f.album) == Names{"P2.ORF"});
    fs::remove(f.album / "P2.ORF");  // another program took it away

    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message().find("P2.ORF") != std::string::npos);
    CHECK(f.message().find("could not be") != std::string::npos);
    // Nothing came back: nothing to redo, and Ctrl+Z still answers.
    CHECK_FALSE(f.browser.undone_operation().has_value());
    f.run(key(XKB_KEY_z, kCtrl));
    CHECK(f.message() != "Still undoing the last one");
}

TEST_CASE("undo: what the messages say") {
    using Kind = FileOperation::Kind;
    CHECK(rummage::views::describe({.kind = Kind::trash,
                                    .items = {{.from = "/a/x.txt", .to = "/T/files/x.2.txt"},
                                              {.from = "/a/y.txt", .to = "/T/files/y.txt"}}}) ==
          "2 items moved to the trash");
    CHECK(rummage::views::describe(
              {.kind = Kind::move, .items = {{.from = "/a/x.txt", .to = "/b/x.txt"}}}) ==
          "“x.txt” back in b");
}
