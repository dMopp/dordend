#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_model.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::fs::Conflict;
using rummage::fs::Resolution;
using rummage::fs::TransferKind;
using rummage::fs::TransferRequest;
using rummage::fs::TransferResult;
using rummage::fs::TrashSources;
using rummage::test::TempDir;
using rummage::views::Browser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

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

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
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

// A home with a trash, and a "disk" (a top directory) with its own; the trash folder reads these
// while the fixture lives.
struct Trashes {
    TempDir dir;
    fs::path home = dir.path / "home";
    fs::path photos = home / "photos";
    fs::path disk = dir.path / "disk";
    TrashSources before = rummage::fs::trash_sources();
    TrashSources sources{
        .home = home / ".local/share/Trash", .tops = std::vector<fs::path>{disk}, .uid = geteuid()};

    Trashes() {
        fs::create_directories(photos);
        fs::create_directories(disk / "shots");
        rummage::fs::set_trash_sources(sources);
    }

    ~Trashes() { rummage::fs::set_trash_sources(before); }

    Trashes(const Trashes&) = delete;
    Trashes& operator=(const Trashes&) = delete;
    Trashes(Trashes&&) = delete;
    Trashes& operator=(Trashes&&) = delete;

    static void make(const fs::path& file, std::string_view text) {
        fs::create_directories(file.parent_path());
        std::ofstream{file} << text;
    }

    // Trashes directory's names as Delete does; the disk's go to its own trash.
    void trash(const fs::path& directory, const Names& names) const {
        TransferRequest request{
            .kind = TransferKind::trash,
            .source = directory,
            .names = names,
            .destination = sources.home,
            .force_copy = false,
            .trash_top = directory.string().starts_with(disk.string()) ? disk : fs::path{}};
        const TransferResult result = rummage::fs::run_trash(request, CancelToken{}, {});
        REQUIRE(result.errors.empty());
        REQUIRE(result.refused.empty());
    }

    [[nodiscard]] fs::path disk_trash() const { return disk / std::format(".Trash-{}", geteuid()); }
};

TransferResult restore(const Names& paths, Resolution answer = Resolution::skip,
                       std::vector<Conflict>* asked = nullptr) {
    rummage::fs::TransferCallbacks callbacks;
    callbacks.resolve = [answer, asked](const Conflict& conflict) {
        if (asked != nullptr) {
            asked->push_back(conflict);
        }
        return answer;
    };
    return rummage::fs::run_restore({.kind = TransferKind::restore,
                                     .source = rummage::fs::trash_path(),
                                     .names = paths,
                                     .destination = {},
                                     .force_copy = false},
                                    CancelToken{}, callbacks);
}

}  // namespace

TEST_CASE("trash folder: .trashinfo files as the specification has them") {
    using rummage::fs::parse_trash_info;
    const auto info = parse_trash_info(
        "[Trash Info]\nPath=/home/me/a%20b%C3%A8.txt\nDeletionDate=2026-10-29T21:25:31\n"
        "Path=/ignored\n",
        "/top");
    REQUIRE(info.has_value());
    CHECK(info.value_or(rummage::fs::TrashInfo{}).original == "/home/me/a bè.txt");
    CHECK(info.value_or(rummage::fs::TrashInfo{}).deleted == "2026-10-29T21:25:31");
    // Relative to the top directory; other groups and comments ignored.
    const auto relative = parse_trash_info(
        "# a comment\n[Trash Info]\r\nPath=shots/P1.ORF\r\n[Other]\nPath=/no\n", "/media/disk");
    CHECK(relative.value_or(rummage::fs::TrashInfo{}).original == "/media/disk/shots/P1.ORF");
    CHECK_FALSE(parse_trash_info("[Desktop Entry]\nPath=/x\n", "/").has_value());
    CHECK_FALSE(parse_trash_info("[Trash Info]\nDeletionDate=2026-10-29T21:25:31\n", "/"));
}

TEST_CASE("trash folder: every trash merged, by original name, the newer ones numbered") {
    Trashes t;
    Trashes::make(t.photos / "a.txt", "first");
    t.trash(t.photos, {"a.txt"});
    Trashes::make(t.disk / "shots" / "a.txt", "second, on the disk");
    t.trash(t.disk / "shots", {"a.txt"});
    Trashes::make(t.photos / "P1.ORF", "raw");
    Trashes::make(t.photos / "P1.ORF.xmp", "xmp");
    t.trash(t.photos, {"P1.ORF"});
    // Not items: a file without its .trashinfo, a .trashinfo without its file.
    Trashes::make(t.sources.home / "files" / "orphan", "x");
    Trashes::make(t.sources.home / "info" / "ghost.trashinfo", "[Trash Info]\nPath=/ghost\n");

    const std::vector<rummage::fs::TrashDir> trashes = rummage::fs::existing_trashes(t.sources);
    REQUIRE(trashes.size() == 2);
    CHECK(trashes[0].path == t.sources.home);
    CHECK(trashes[1].path == t.disk_trash());

    std::vector<rummage::fs::TrashedItem> items = rummage::fs::list_trash(t.sources);
    std::ranges::sort(items, {}, &rummage::fs::TrashedItem::name);
    Names names;
    for (const auto& item : items) {
        names.push_back(item.name);
    }
    REQUIRE(names.size() == 4);
    // Trashed in the same second: either a.txt may be the older; both are listed, one numbered.
    CHECK(names == Names{"P1.ORF", "P1.ORF.xmp", "a.2.txt", "a.txt"});
    const auto plain =
        std::ranges::find(items, std::string{"a.txt"}, &rummage::fs::TrashedItem::name);
    CHECK((plain->info.original == t.photos / "a.txt" ||
           plain->info.original == t.disk / "shots" / "a.txt"));
}

TEST_CASE("trash folder: restoring puts items back, folders made again, sidecars along") {
    Trashes t;
    Trashes::make(t.photos / "P1.ORF", "raw");
    Trashes::make(t.photos / "P1.ORF.xmp", "xmp");
    Trashes::make(t.photos / "P1_01.ORF.xmp", "copy");
    Trashes::make(t.photos / "Trip" / "x.txt", "x");
    t.trash(t.photos, {"P1.ORF", "Trip"});
    fs::remove_all(t.photos);  // the folder they came from is gone

    const fs::path files = t.sources.home / "files";
    const TransferResult result = restore({(files / "P1.ORF").string(), (files / "Trip").string()});
    CHECK(result.errors.empty());
    CHECK(result.placed == Names{"P1.ORF", "Trip"});
    CHECK(result.files_done == 4);  // with its two sidecars
    CHECK(listing(t.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip"});
    CHECK(read_file(t.photos / "Trip" / "x.txt") == "x");
    CHECK(listing(files).empty());
    CHECK(listing(t.sources.home / "info").empty());

    // Not there any more.
    const TransferResult gone = restore({(files / "P1.ORF").string()});
    CHECK(gone.errors == Names{"P1.ORF: not in the trash any more"});
}

TEST_CASE("trash folder: a taken name asks; keep both renames the sidecars too") {
    Trashes t;
    Trashes::make(t.photos / "P1.ORF", "old");
    Trashes::make(t.photos / "P1.ORF.xmp", "old xmp");
    t.trash(t.photos, {"P1.ORF"});
    Trashes::make(t.photos / "P1.ORF", "new");
    const fs::path files = t.sources.home / "files";

    std::vector<Conflict> asked;
    CHECK(restore({(files / "P1.ORF").string()}, Resolution::skip, &asked).placed.empty());
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].name == "P1.ORF");
    CHECK(asked[0].destination == t.photos);
    CHECK_FALSE(asked[0].merge);
    CHECK(fs::exists(files / "P1.ORF"));  // skipped: still in the trash

    const TransferResult both = restore({(files / "P1.ORF").string()}, Resolution::keep_both);
    CHECK(both.placed == Names{"P1 2.ORF"});
    CHECK(read_file(t.photos / "P1 2.ORF") == "old");
    CHECK(read_file(t.photos / "P1 2.ORF.xmp") == "old xmp");
    CHECK(read_file(t.photos / "P1.ORF") == "new");

    // Replace: a file over a file; never a folder.
    t.trash(t.photos, {"P1 2.ORF"});
    Trashes::make(t.photos / "P1 2.ORF", "in the way");
    CHECK(restore({(files / "P1 2.ORF").string()}, Resolution::replace).errors.empty());
    CHECK(read_file(t.photos / "P1 2.ORF") == "old");
    Trashes::make(t.photos / "Trip" / "a", "a");
    t.trash(t.photos, {"Trip"});
    fs::create_directories(t.photos / "Trip");
    CHECK(restore({(files / "Trip").string()}, Resolution::replace).errors ==
          Names{"Trip: a folder is not replaced; keep both or skip"});
    CHECK(fs::exists(files / "Trip"));
}

TEST_CASE("trash folder: the model lists every trash and follows changes") {
    Trashes t;
    Trashes::make(t.photos / "a.txt", "a");
    Trashes::make(t.disk / "shots" / "b.txt", "b");
    t.trash(t.photos, {"a.txt"});
    t.trash(t.disk / "shots", {"b.txt"});

    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    rummage::fs::DirectoryModel model{loop, pool, watcher, rummage::fs::MimeDatabase::load()};
    model.open(rummage::fs::trash_path());
    CHECK(model.is_trash());
    REQUIRE(run_until(loop, [&] { return !model.loading(); }));
    const auto names = [&] {
        Names out;
        for (const rummage::fs::Entry& entry : model.listing().entries()) {
            out.push_back(entry.name);
        }
        std::ranges::sort(out);
        return out;
    };
    CHECK(names() == Names{"a.txt", "b.txt"});
    REQUIRE(model.trashed("b.txt") != nullptr);
    CHECK(model.trashed("b.txt")->path == t.disk_trash() / "files" / "b.txt");
    CHECK(model.trashed("b.txt")->info.original == t.disk / "shots" / "b.txt");

    // Something trashed elsewhere comes in; something restored goes.
    Trashes::make(t.photos / "c.txt", "c");
    t.trash(t.photos, {"c.txt"});
    REQUIRE(run_until(loop, [&] { return names().size() == 3; }));
    CHECK(restore({(t.disk_trash() / "files" / "b.txt").string()}).errors.empty());
    REQUIRE(run_until(loop, [&] { return names() == Names{"a.txt", "c.txt"}; }));
}

TEST_CASE("trash folder: in the browser, Restore from the menu; nothing else is done there") {
    Trashes t;
    Trashes::make(t.photos / "a.txt", "a");
    Trashes::make(t.photos / "b.txt", "b");
    Trashes::make(t.photos / "P1.ORF", "raw");
    Trashes::make(t.photos / "P1.ORF.xmp", "xmp");
    t.trash(t.photos, {"a.txt", "b.txt", "P1.ORF"});

    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_places({{.label = "Home", .path = t.home, .icon = "home"},
                        {.label = "Trash", .path = rummage::fs::trash_path(), .icon = "trash"}});
    browser.open(rummage::fs::trash_path());
    browser.set_mode(Browser::Mode::details);
    const auto settle = [&] { return run_until(loop, [&] { return !browser.loading(); }); };
    REQUIRE(settle());
    const auto trash_icon = [&] { return browser.sidebar().places().at(1).icon; };
    REQUIRE(run_until(loop, [&] { return trash_icon() == "trash-filled"; }));
    const std::shared_ptr<rummage::views::Folder> folder = browser.details().folder();
    REQUIRE(folder->is_trash());
    REQUIRE(folder->entries().size() == 4);
    const auto key = [](xkb_keysym_t keysym, rummage::ui::Modifiers modifiers = {}) {
        rummage::ui::KeyEvent event;
        event.keysym = keysym;
        event.modifiers = modifiers;
        return event;
    };

    // Nothing is made, renamed, pasted or trashed again; files are not opened from here. Delete
    // asks to delete for good; Escape keeps it.
    folder->select_name("a.txt");
    CHECK(browser.key(key(XKB_KEY_Delete), Browser::Clock::now()));
    REQUIRE(browser.confirm_dialog().is_open());
    CHECK(browser.confirm_dialog().title() == "Delete “a.txt” permanently?");
    REQUIRE(settle());
    CHECK(browser.key(key(XKB_KEY_Escape), Browser::Clock::now()));
    CHECK_FALSE(browser.confirm_dialog().is_open());
    REQUIRE(settle());
    CHECK(folder->entries().size() == 4);
    CHECK(browser.key(key(XKB_KEY_F2), Browser::Clock::now()));
    CHECK_FALSE(browser.name_editor().active());
    CHECK(browser.key(key(XKB_KEY_Return), Browser::Clock::now()));
    CHECK(browser.status_bar().message() == "Restore it first to open it");
    folder->set_cursor(std::nullopt);
    CHECK(browser.key(key(XKB_KEY_Menu), Browser::Clock::now()));
    REQUIRE(browser.context_menu().is_open());  // on empty space: Empty Trash and pane controls
    REQUIRE(browser.context_menu().items().size() == 2);
    CHECK(browser.context_menu().items()[0].id == "empty_trash");
    CHECK(browser.key(key(XKB_KEY_Escape), Browser::Clock::now()));

    // The menu on an item: Restore. The photo comes back with its sidecar; the cursor goes on.
    folder->select_name("P1.ORF");
    CHECK(browser.key(key(XKB_KEY_Menu), Browser::Clock::now()));
    REQUIRE(browser.context_menu().is_open());
    REQUIRE(browser.context_menu().items().size() == 4);
    CHECK(browser.context_menu().items()[0].id == "restore");
    CHECK(browser.context_menu().items()[1].id == "erase");
    CHECK(browser.key(key(XKB_KEY_Return), Browser::Clock::now()));
    REQUIRE(settle());
    CHECK(listing(t.photos) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(browser.status_bar().message() == "Restored “P1.ORF” with 1 darktable sidecar");
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 2; }));

    // The last ones: the trash is empty, and its icon says so.
    browser.context_menu().close();
    folder->select_all();
    browser.restore();
    REQUIRE(settle());
    CHECK(listing(t.photos) == Names{"P1.ORF", "P1.ORF.xmp", "a.txt", "b.txt"});
    REQUIRE(run_until(loop, [&] { return trash_icon() == "trash"; }));
    // The view's folder reads the trash on its own: it may follow a moment later.
    CHECK(run_until(loop, [&] { return folder->entries().empty(); }));
}

TEST_CASE("trash folder: the disks' trashes are found from the mount table") {
    CHECK(
        rummage::fs::mount_points("36 25 8:0 / / rw - ext4 /dev/a rw\n"
                                  "38 27 8:1 / /run/media/me/My\\040Disk rw - ext4 /dev/b rw\n") ==
        std::vector<fs::path>{"/", "/run/media/me/My Disk"});
    // The real table: /proc reports a size of 0, so a sized read would find nothing.
    const std::vector<fs::path> points =
        rummage::fs::mount_points(rummage::fs::read_proc_file("/proc/self/mountinfo"));
    CHECK(std::ranges::find(points, fs::path{"/"}) != points.end());
}
