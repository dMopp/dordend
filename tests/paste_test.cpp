#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/shortcuts.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::Folder;
using rummage::views::Shortcut;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace scope = rummage::views::scope;

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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/{P2.ORF}}, shown in the details.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(root / "photos");
        fs::create_directories(root / "album");
        for (const char* name : {"photos/P1.ORF", "photos/P1.ORF.xmp", "photos/P2.ORF"}) {
            std::ofstream{root / name} << name;
        }
        std::ofstream{root / "album" / "P2.ORF"} << "album's own";
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> folder() { return browser.details().folder(); }

    // folder shown, these selected.
    void show(const fs::path& folder_path, const Names& selected) {
        browser.open(folder_path);
        browser.set_mode(Browser::Mode::details);  // after open: a mode needs a folder
        REQUIRE(settle());
        if (selected.empty()) {
            return;
        }
        folder()->select_name(selected.front());
        for (std::size_t i = 1; i < selected.size(); ++i) {
            const auto& entries = folder()->entries();
            const auto it = std::ranges::find(entries, selected[i], &rummage::fs::Entry::name);
            REQUIRE(it != entries.end());
            folder()->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                                 rummage::views::SelectMode::toggle);
        }
    }
};

}  // namespace

TEST_CASE("paste: Ctrl+C then Ctrl+V copies, sidecars along, and selects what arrived") {
    Fixture f;
    f.show(f.root / "photos", {"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Ctrl+V copies “P1.ORF” where you want them");
    CHECK(f.browser.cut_marks().names.empty());

    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.root / "photos") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(f.browser.status_bar().message().starts_with("Copied 2 files ("));
    CHECK_FALSE(f.browser.status_bar().transfer().has_value());
    REQUIRE(run_until(f.loop, [&] { return f.folder()->cursor_entry() != nullptr; }));
    CHECK(f.folder()->cursor_entry()->name == "P1.ORF");

    // Pasting again: the copy stays in the clipboard; in its own folder it keeps both.
    f.show(f.root / "photos", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "photos") ==
          Names{"P1 2.ORF", "P1 2.ORF.xmp", "P1.ORF", "P1.ORF.xmp", "P2.ORF"});
}

TEST_CASE("paste: Ctrl+X fades, Escape keeps them, Ctrl+V moves once") {
    Fixture f;
    f.show(f.root / "photos", {"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.browser.cut_marks().names.contains("P1.ORF"));
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK(f.browser.cut_marks().names.empty());
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Nothing to paste: Ctrl+C or Ctrl+X first");

    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    f.show(f.root / "album", {});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.cut_marks().names.empty());  // on its way: nothing left to move
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.root / "photos") == Names{"P2.ORF"});
    CHECK(f.browser.status_bar().message() == "Moved 2 files");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    CHECK(f.browser.status_bar().message() == "Nothing to paste: Ctrl+C or Ctrl+X first");
}

TEST_CASE("paste: a taken name asks; skip, replace, both for the rest, stop") {
    Fixture f;
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    const auto paste_into_album = [&](const Names& names) {
        f.show(f.root / "photos", names);
        CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
        f.show(f.root / "album", {});
        CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
        REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
        CHECK(dialog.conflict().name == names.front());  // the first name taken
    };

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_space)));  // the card takes every key; nothing else happens
    CHECK_FALSE(f.browser.quick_look().is_open());
    CHECK(f.press(key(XKB_KEY_s)));
    REQUIRE(f.settle());
    CHECK(read_file(f.root / "album" / "P2.ORF") == "album's own");

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_r)));
    REQUIRE(f.settle());
    CHECK(read_file(f.root / "album" / "P2.ORF") == "photos/P2.ORF");

    // Both names taken now: "for the rest" answers the second without asking.
    std::ofstream{f.root / "album" / "P1.ORF"} << "album's P1";
    paste_into_album({"P1.ORF", "P2.ORF"});
    CHECK(f.press(key(XKB_KEY_a)));
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK_FALSE(dialog.is_open());
    CHECK(listing(f.root / "album") ==
          Names{"P1 2.ORF", "P1 2.ORF.xmp", "P1.ORF", "P2 2.ORF", "P2.ORF"});

    paste_into_album({"P2.ORF"});
    CHECK(f.press(key(XKB_KEY_Escape)));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Stopped: 0 files copied so far");
}

TEST_CASE("paste: every key listed for the question works on it") {
    Fixture f;
    rummage::views::ConflictDialog& dialog = f.browser.conflict_dialog();
    for (const Shortcut& shortcut : rummage::views::shortcuts()) {
        if ((shortcut.scope & scope::kConflict) == 0) {
            continue;
        }
        for (const xkb_keysym_t keysym : shortcut.keysyms) {
            if (keysym == XKB_KEY_NoSymbol) {
                continue;
            }
            f.show(f.root / "photos", {"P2.ORF"});
            CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
            f.show(f.root / "album", {});
            CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
            REQUIRE(run_until(f.loop, [&] { return dialog.is_open(); }));
            CHECK(f.press(key(keysym, shortcut.modifiers)));
            if (dialog.is_open()) {  // "for the rest" only ticks the box
                CHECK(f.press(key(XKB_KEY_s)));
            }
            REQUIRE(f.settle());
        }
    }
}
