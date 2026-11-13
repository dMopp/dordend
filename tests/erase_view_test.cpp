#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/transfer.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/shortcuts.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::fs::TransferKind;
using rummage::fs::TrashSources;
using rummage::test::TempDir;
using rummage::views::Browser;
using rummage::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace scope = rummage::views::scope;

namespace {

using Names = std::vector<std::string>;

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

void make(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream{file} << text;
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

rummage::ui::KeyEvent key(xkb_keysym_t keysym, rummage::ui::Modifiers modifiers = {}) {
    rummage::ui::KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

// A home with photos and a trash, and a "disk" with its own; the trash folder reads these while
// the fixture lives.
struct Fixture {
    TempDir dir;
    fs::path home = dir.path / "home";
    fs::path photos = home / "photos";
    fs::path disk = dir.path / "disk";
    TrashSources before = rummage::fs::trash_sources();
    TrashSources sources{
        .home = home / ".local/share/Trash", .tops = std::vector<fs::path>{disk}, .uid = geteuid()};
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::unique_ptr<Browser> browser;

    Fixture() {
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P2.ORF", "a.txt"}) {
            make(photos / name, name);
        }
        fs::create_directories(disk / "shots");
        rummage::fs::set_trash_sources(sources);
        browser = std::make_unique<Browser>(
            loop, pool, watcher, rummage::fs::MimeDatabase::load(), [] {},
            [](rummage::ui::CursorShape /*shape*/) {});
        browser->set_home_trash(sources.home);
        browser->set_places(
            {{.label = "Home", .path = home, .icon = "home"},
             {.label = "Trash", .path = rummage::fs::trash_path(), .icon = "trash"}});
    }

    ~Fixture() {
        browser.reset();
        rummage::fs::set_trash_sources(before);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&&) = delete;
    Fixture& operator=(Fixture&&) = delete;

    bool settle() {
        return run_until(loop, [&] { return !browser->loading(); });
    }

    bool press(const rummage::ui::KeyEvent& event) const {
        return browser->key(event, Browser::Clock::now());
    }

    // directory in the details, the cursor on cursor_on.
    std::shared_ptr<Folder> show(const fs::path& directory, const std::string& cursor_on) {
        browser->confirm_dialog().close();
        browser->context_menu().close();
        browser->open(directory);
        browser->set_mode(Browser::Mode::details);
        REQUIRE(settle());
        std::shared_ptr<Folder> folder = browser->details().folder();
        if (!cursor_on.empty()) {
            folder->select_name(cursor_on);
            REQUIRE(run_until(loop, [&] { return folder->cursor_entry() != nullptr; }));
        }
        return folder;
    }
};

}  // namespace

TEST_CASE("erase: Shift+Delete asks; Escape keeps everything, Enter deletes with the sidecar") {
    Fixture f;
    const std::shared_ptr<Folder> folder = f.show(f.photos, "P1.ORF");
    rummage::views::ConfirmDialog& card = f.browser->confirm_dialog();

    CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
    REQUIRE(card.is_open());
    CHECK(card.title() == "Delete “P1.ORF” permanently?");
    CHECK(card.detail() == "Adding up the size…");
    REQUIRE(f.settle());
    CHECK(card.detail().starts_with("With its darktable sidecar · "));
    CHECK(f.press(key(XKB_KEY_space)));  // the card takes every key; nothing else happens
    CHECK_FALSE(f.browser->quick_look().is_open());
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(card.is_open());
    REQUIRE(f.settle());
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "a.txt"});

    // Enter at once, before the size is known: it goes once it is.
    CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK_FALSE(card.is_open());
    REQUIRE(f.settle());
    CHECK(listing(f.photos) == Names{"P2.ORF", "a.txt"});
    CHECK(f.browser->status_bar().message().starts_with("Deleted “P1.ORF” permanently"));
    CHECK(listing(f.sources.home / "files").empty());  // nothing went to the trash
    REQUIRE(run_until(f.loop, [&] { return folder->entries().size() == 2; }));
    const rummage::fs::Entry* const cursor = folder->cursor_entry();
    CHECK((cursor != nullptr && cursor->name == "P2.ORF"));
}

TEST_CASE("erase: every key listed for the card works on it") {
    Fixture f;
    for (const rummage::views::Shortcut& shortcut : rummage::views::shortcuts()) {
        if ((shortcut.scope & scope::kConfirm) == 0) {
            continue;
        }
        for (const xkb_keysym_t keysym : shortcut.keysyms) {
            if (keysym == XKB_KEY_NoSymbol) {
                continue;
            }
            make(f.photos / "a.txt", "a");
            f.show(f.photos, "a.txt");
            CHECK(f.press(key(XKB_KEY_Delete, {.shift = true})));
            REQUIRE(f.browser->confirm_dialog().is_open());
            CHECK(f.press(key(keysym, shortcut.modifiers)));
            CHECK_FALSE(f.browser->confirm_dialog().is_open());
            REQUIRE(f.settle());
            CHECK(fs::exists(f.photos / "a.txt") == (keysym == XKB_KEY_Escape));
        }
    }
}

TEST_CASE("erase: in the trash folder, Delete and Empty Trash delete for good") {
    Fixture f;
    make(f.disk / "shots" / "P9.ORF", "raw");
    const auto trash = [&](const fs::path& directory, const Names& names, const fs::path& top) {
        const rummage::fs::TransferResult result =
            rummage::fs::run_trash({.kind = TransferKind::trash,
                                    .source = directory,
                                    .names = names,
                                    .destination = f.sources.home,
                                    .force_copy = false,
                                    .trash_top = top},
                                   CancelToken{}, {});
        REQUIRE(result.errors.empty());
    };
    trash(f.photos, {"P1.ORF", "P2.ORF", "a.txt"}, {});
    trash(f.disk / "shots", {"P9.ORF"}, f.disk);
    const fs::path disk_trash = f.disk / std::format(".Trash-{}", geteuid());

    // Delete on a photo: it goes with its sidecar, the rest stays.
    const std::shared_ptr<Folder> folder = f.show(rummage::fs::trash_path(), "P1.ORF");
    REQUIRE(folder->entries().size() == 5);
    CHECK(f.press(key(XKB_KEY_Delete)));
    REQUIRE(f.browser->confirm_dialog().is_open());
    REQUIRE(f.settle());
    CHECK(f.browser->confirm_dialog().detail().starts_with("With its darktable sidecar"));
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK(listing(f.sources.home / "files") == Names{"P2.ORF", "a.txt"});
    CHECK(listing(f.sources.home / "info") == Names{"P2.ORF.trashinfo", "a.txt.trashinfo"});
    REQUIRE(run_until(f.loop, [&] { return folder->entries().size() == 3; }));

    // Empty Trash from the menu on empty space: both trashes, after the card.
    folder->set_cursor(std::nullopt);
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.browser->context_menu().is_open());
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.browser->confirm_dialog().is_open());
    CHECK(f.browser->confirm_dialog().title() == "Empty the trash?");
    REQUIRE(f.settle());
    CHECK(f.browser->confirm_dialog().detail().starts_with("3 items · "));
    CHECK(f.press(key(XKB_KEY_Return)));
    REQUIRE(f.settle());
    CHECK(f.browser->status_bar().message().starts_with("Emptied the trash: 3 items deleted"));
    for (const fs::path& trash_dir : {f.sources.home, disk_trash}) {
        CHECK(listing(trash_dir / "files").empty());
        CHECK(listing(trash_dir / "info").empty());
    }
    CHECK(run_until(f.loop, [&] { return folder->entries().empty(); }));
    CHECK(run_until(f.loop, [&] { return f.browser->sidebar().places().at(1).icon == "trash"; }));

    // Nothing left: the card does not stay.
    f.browser->empty_trash();
    REQUIRE(f.settle());
    CHECK_FALSE(f.browser->confirm_dialog().is_open());
    CHECK(f.browser->status_bar().message() == "The trash is empty already");
}
