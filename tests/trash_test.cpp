#include "rummage/fs/trash.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
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
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::fs::TransferKind;
using rummage::fs::TransferRequest;
using rummage::fs::TransferResult;
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

// The Path= line of a .trashinfo.
std::string info_path(const fs::path& file) {
    const std::string text = read_file(file);
    const std::size_t start = text.find("\nPath=");
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find('\n', start + 1);
    return text.substr(start + 6, end - start - 6);
}

TransferResult trash(const fs::path& source, const Names& names, const fs::path& home,
                     const fs::path& top = {}) {
    TransferRequest request{.kind = TransferKind::trash,
                            .source = source,
                            .names = names,
                            .destination = home,
                            .force_copy = false,
                            .trash_top = top};
    return rummage::fs::run_trash(request, CancelToken{}, {});
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

}  // namespace

TEST_CASE("trash: names and paths are written as GLib writes them") {
    using rummage::fs::escape_path;
    using rummage::fs::trash_name;
    CHECK(escape_path("/home/a b/è%~_-.txt", true) == "/home/a%20b/%C3%A8%25~_-.txt");
    CHECK(escape_path("a/b\nc", false) == "a%2Fb%0Ac");
    CHECK(trash_name("P1.ORF", 1) == "P1.ORF");
    CHECK(trash_name("P1.ORF", 2) == "P1.2.ORF");
    CHECK(trash_name("P1.ORF.xmp", 3) == "P1.3.ORF.xmp");
    CHECK(trash_name("Trip", 2) == "Trip.2");

    const std::string info = rummage::fs::trash_info("/home/me/a b.txt", 0);
    CHECK(info.starts_with("[Trash Info]\nPath=/home/me/a%20b.txt\nDeletionDate="));
    const std::string date = info.substr(info.find("DeletionDate=") + 13);
    CHECK(date.size() == 20);  // 1970-01-01T01:00:00 and the newline, in local time
    CHECK(date[10] == 'T');
    CHECK(date.back() == '\n');
}

TEST_CASE("trash: into the home trash, sidecars along, folders in directorysizes") {
    const TempDir dir;
    const fs::path home = dir.path / "data" / "Trash";  // made when needed
    const fs::path photos = dir.path / "my photos";
    fs::create_directories(photos / "Trip");
    {
        std::ofstream big{photos / "Trip" / "big.raw"};
        big << std::string(100'000, 'x');
    }
    fs::create_hard_link(photos / "Trip" / "big.raw", photos / "Trip" / "same.raw");
    for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "a b.txt", "Trip/x.txt"}) {
        std::ofstream{photos / name} << name;
    }

    const TransferResult result = trash(photos, {"P1.ORF", "a b.txt", "Trip"}, home);
    CHECK(result.refused.empty());
    CHECK(result.errors.empty());
    CHECK(result.placed == Names{"P1.ORF", "a b.txt", "Trip"});
    CHECK(result.files_done == 5);
    CHECK(listing(photos).empty());
    CHECK(listing(home / "files") ==
          Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip", "a b.txt"});
    CHECK(read_file(home / "files" / "Trip" / "x.txt") == "Trip/x.txt");
    CHECK(info_path(home / "info" / "a b.txt.trashinfo") ==
          rummage::fs::escape_path((photos / "a b.txt").string(), true));
    CHECK(info_path(home / "info" / "P1.ORF.xmp.trashinfo") ==
          rummage::fs::escape_path((photos / "P1.ORF.xmp").string(), true));
    struct stat home_info{};
    REQUIRE(stat(home.c_str(), &home_info) == 0);
    CHECK((home_info.st_mode & 0777) == 0700);

    const std::string sizes = read_file(home / "directorysizes");
    CHECK(sizes.ends_with(" Trip\n"));
    CHECK(std::ranges::count(sizes, '\n') == 1);
    // The size is du -B1's, a hard link counted once.
    const std::string command = std::format("du -B1 -s '{}'", (home / "files" / "Trip").string());
    if (FILE* const du = popen(command.c_str(), "r")) {
        std::array<char, 64> line{};
        const bool read = std::fgets(line.data(), line.size(), du) != nullptr;
        pclose(du);
        REQUIRE(read);
        const std::string blocks{line.data(), std::strcspn(line.data(), "\t")};
        CHECK(sizes.starts_with(blocks + " "));
    }

    // The same names again: GLib's numbers, nothing replaced; the folder joins directorysizes.
    fs::create_directories(photos / "Trip");
    std::ofstream{photos / "P1.ORF"} << "second";
    std::ofstream{photos / "P1.ORF.xmp"} << "second";
    const TransferResult again = trash(photos, {"P1.ORF", "Trip"}, home);
    CHECK(again.errors.empty());
    CHECK(read_file(home / "files" / "P1.2.ORF") == "second");
    CHECK(read_file(home / "files" / "P1.2.ORF.xmp") == "second");
    CHECK(read_file(home / "files" / "P1.ORF") == "P1.ORF");
    CHECK(fs::exists(home / "info" / "P1.2.ORF.trashinfo"));
    CHECK(fs::is_directory(home / "files" / "Trip.2"));
    CHECK(std::ranges::count(read_file(home / "directorysizes"), '\n') == 2);

    // A file in files/ without its info is left be: the next number is taken.
    std::ofstream{home / "files" / "lost.txt"} << "orphan";
    std::ofstream{photos / "lost.txt"} << "mine";
    CHECK(trash(photos, {"lost.txt"}, home).errors.empty());
    CHECK(read_file(home / "files" / "lost.txt") == "orphan");
    CHECK(read_file(home / "files" / "lost.2.txt") == "mine");
    CHECK_FALSE(fs::exists(home / "info" / "lost.txt.trashinfo"));
}

TEST_CASE("trash: what cannot go stays, and leaves no info behind") {
    const TempDir dir;
    const fs::path home = dir.path / "Trash";
    const fs::path locked = dir.path / "locked";
    fs::create_directories(locked);
    std::ofstream{locked / "keep.txt"} << "keep";
    if (geteuid() == 0) {
        return;  // root ignores permissions
    }
    fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
    const TransferResult result = trash(locked, {"keep.txt", "missing.txt"}, home);
    fs::permissions(locked, fs::perms::owner_all);
    CHECK(result.placed.empty());
    REQUIRE(result.errors.size() == 2);
    CHECK(result.errors[0] == "missing.txt: No such file or directory");
    CHECK(result.errors[1] == "keep.txt: Permission denied");
    CHECK(read_file(locked / "keep.txt") == "keep");
    CHECK(listing(home / "info").empty());

    // The trash and what is in it are not trashed again.
    std::ofstream{dir.path / "x.txt"} << "x";
    CHECK(trash(dir.path, {"x.txt"}, home).errors.empty());
    CHECK(trash(home / "files", {"x.txt"}, home).refused == "These are in the trash already");
    const TransferResult holder = trash(dir.path, {"Trash"}, home);
    CHECK(holder.errors == Names{"Trash: it holds the trash"});
    CHECK(fs::exists(home / "files" / "x.txt"));
}

TEST_CASE("trash: on another disk, the top directory's trash with relative paths") {
    const TempDir dir;
    const uid_t uid = geteuid();
    const fs::path top = dir.path / "disk";
    fs::create_directories(top / "photos");
    std::ofstream{top / "photos" / "P1.ORF"} << "P1";

    // No .Trash: .Trash-uid, made private.
    const TransferResult result = trash(top / "photos", {"P1.ORF"}, dir.path / "home", top);
    CHECK(result.refused.empty());
    const fs::path mine = top / std::format(".Trash-{}", uid);
    CHECK(read_file(mine / "files" / "P1.ORF") == "P1");
    CHECK(info_path(mine / "info" / "P1.ORF.trashinfo") == "photos/P1.ORF");
    CHECK_FALSE(fs::exists(dir.path / "home"));  // the home trash is not touched

    std::error_code error;
    // .Trash without the sticky bit, or a link: not used.
    fs::create_directories(top / ".Trash");
    CHECK(rummage::fs::top_trash(top, uid, error).value_or(rummage::fs::TrashDir{}).path == mine);
    fs::remove(top / ".Trash");
    fs::create_directories(dir.path / "elsewhere");
    chmod((dir.path / "elsewhere").c_str(), 01777);
    fs::create_directory_symlink(dir.path / "elsewhere", top / ".Trash");
    CHECK(rummage::fs::top_trash(top, uid, error).value_or(rummage::fs::TrashDir{}).path == mine);
    fs::remove(top / ".Trash");

    // A real .Trash with the sticky bit: its uid folder.
    fs::create_directories(top / ".Trash");
    chmod((top / ".Trash").c_str(), 01777);
    const std::optional<rummage::fs::TrashDir> shared = rummage::fs::top_trash(top, uid, error);
    REQUIRE(shared.has_value());
    CHECK(shared.value_or(rummage::fs::TrashDir{}).path == top / ".Trash" / std::to_string(uid));
    CHECK(fs::is_directory(top / ".Trash" / std::to_string(uid) / "files"));
    CHECK(shared.value_or(rummage::fs::TrashDir{}).relative_to == top);
}

TEST_CASE("trash: the top directory is the mount point") {
    const TempDir dir;
    const fs::path top = rummage::fs::mount_top(dir.path);
    struct stat here{};
    struct stat at_top{};
    REQUIRE(stat(dir.path.c_str(), &here) == 0);
    REQUIRE(stat(top.c_str(), &at_top) == 0);
    CHECK(here.st_dev == at_top.st_dev);
    const auto [end, rest] = std::ranges::mismatch(top, fs::canonical(dir.path));
    CHECK(end == top.end());  // an ancestor
    struct stat above{};
    if (top != top.root_path()) {
        REQUIRE(stat(top.parent_path().c_str(), &above) == 0);
        CHECK(above.st_dev != at_top.st_dev);
    }
}

TEST_CASE("trash: Delete moves the selection to the trash, the cursor goes on") {
    const TempDir dir;
    const fs::path photos = dir.path / "photos";
    fs::create_directories(photos);
    for (const char* name : {"P1.ORF", "P2.ORF", "P2.ORF.xmp", "P3.ORF"}) {
        std::ofstream{photos / name} << name;
    }
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    browser.set_home_trash(dir.path / "Trash");
    browser.open(photos);
    browser.set_mode(Browser::Mode::details);
    REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    const std::shared_ptr<rummage::views::Folder> folder = browser.details().folder();
    folder->select_name("P2.ORF");

    rummage::ui::KeyEvent del;
    del.keysym = XKB_KEY_Delete;
    CHECK(browser.key(del, Browser::Clock::now()));
    REQUIRE(run_until(loop, [&] { return browser.transfers_pending() == 0; }));
    CHECK(listing(photos) == Names{"P1.ORF", "P3.ORF"});
    CHECK(listing(dir.path / "Trash" / "files") == Names{"P2.ORF", "P2.ORF.xmp"});
    CHECK(browser.status_bar().message() == "“P2.ORF” with 1 darktable sidecar moved to the trash");
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 2; }));
    REQUIRE(folder->cursor_entry() != nullptr);
    CHECK(folder->cursor_entry()->name == "P3.ORF");

    // The last one: the cursor goes back to the one before.
    CHECK(browser.key(del, Browser::Clock::now()));
    REQUIRE(run_until(loop, [&] { return browser.transfers_pending() == 0; }));
    REQUIRE(run_until(loop, [&] { return folder->entries().size() == 1; }));
    REQUIRE(folder->cursor_entry() != nullptr);
    CHECK(folder->cursor_entry()->name == "P1.ORF");
    CHECK(browser.status_bar().message() == "“P3.ORF” moved to the trash");
}
