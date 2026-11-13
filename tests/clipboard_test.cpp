#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_clipboard.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::ClipboardFiles;
using rummage::views::SystemClipboard;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace views = rummage::views;

namespace {

using Names = std::vector<std::string>;
using Paths = std::vector<fs::path>;

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

// The system clipboard as a program would see it: what rummage offered, or another program's.
struct FakeClipboard {
    EventLoop* loop{nullptr};
    std::vector<SystemClipboard::Format> formats;
    bool ours{false};
    int cleared{0};
    std::uint64_t generation{0};  // other contents: one more

    SystemClipboard port() {
        return {.offer =
                    [this](std::vector<SystemClipboard::Format> offered) {
                        formats = std::move(offered);
                        ours = true;
                        ++generation;
                    },
                .clear =
                    [this] {
                        formats.clear();
                        ours = false;
                        ++cleared;
                        ++generation;
                    },
                .owned = [this] { return ours; },
                .generation = [this] { return generation; },
                .types =
                    [this] {
                        std::vector<std::string> types;
                        types.reserve(formats.size());
                        for (const auto& [type, data] : formats) {
                            types.push_back(type);
                        }
                        return types;
                    },
                .read =
                    [this](const std::string& type, SystemClipboard::Done done) {
                        std::optional<std::string> data;
                        for (const auto& [offered, bytes] : formats) {
                            if (offered == type) {
                                data = bytes;
                            }
                        }
                        // Later, as a pipe would answer.
                        loop->post([done = std::move(done), data] { done(data); });
                    }};
    }

    // Another program copies these.
    void copy_from_elsewhere(std::vector<SystemClipboard::Format> offered) {
        formats = std::move(offered);
        ours = false;
        ++generation;
    }

    [[nodiscard]] std::string data(std::string_view type) const {
        for (const auto& [offered, bytes] : formats) {
            if (offered == type) {
                return bytes;
            }
        }
        return {};
    }
};

// root/{photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, album/, other/{note.txt}}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    FakeClipboard clipboard{
        .loop = &loop, .formats = {}, .ours = false, .cleared = 0, .generation = 0};

    Fixture() {
        fs::create_directories(root / "photos");
        fs::create_directories(root / "album");
        fs::create_directories(root / "other");
        for (const char* name :
             {"photos/P1.ORF", "photos/P1.ORF.xmp", "photos/P2.ORF", "other/note.txt"}) {
            std::ofstream{root / name} << name;
        }
        browser.set_system_clipboard(clipboard.port());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    void show(const fs::path& folder, const std::string& cursor_on) {
        browser.open(folder);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
        if (!cursor_on.empty()) {
            browser.details().folder()->select_name(cursor_on);
            REQUIRE(run_until(
                loop, [this] { return browser.details().folder()->cursor_entry() != nullptr; }));
        }
    }
};

}  // namespace

TEST_CASE("clipboard: file URIs as GLib writes them, lists as other programs write them") {
    const Paths paths{"/home/me/a b%#?.ORF", "/home/me/caffè.jpg"};
    CHECK(views::uri_list(paths) ==
          "file:///home/me/a%20b%25%23%3F.ORF\r\nfile:///home/me/caff%C3%A8.jpg\r\n");
    CHECK(views::gnome_copied_files(true, paths) ==
          "cut\nfile:///home/me/a%20b%25%23%3F.ORF\nfile:///home/me/caff%C3%A8.jpg");
    CHECK(views::gnome_copied_files(false, paths).starts_with("copy\n"));
    CHECK(views::path_lines(paths) == "/home/me/a b%#?.ORF\n/home/me/caffè.jpg");

    // Back again, whatever the writer.
    const ClipboardFiles read = views::parse_uri_list(views::uri_list(paths));
    CHECK(read.paths == paths);
    CHECK_FALSE(read.cut);
    const std::optional<ClipboardFiles> gnome =
        views::parse_gnome_copied_files(views::gnome_copied_files(true, paths));
    REQUIRE(gnome.has_value());
    CHECK(gnome.value_or(ClipboardFiles{}).cut);
    CHECK(gnome.value_or(ClipboardFiles{}).paths == paths);
    CHECK_FALSE(views::parse_gnome_copied_files("file:///a\n").has_value());
}

TEST_CASE("clipboard: only local files are taken from a URI list") {
    const ClipboardFiles files = views::parse_uri_list(
        "# a comment\r\n"
        "file:///a/b.txt\r\n"
        "\r\n"
        "file://localhost/c/d/\n"
        "FILE:///e\n"
        "file://otherhost/f\n"
        "https://example.com/g.jpg\n"
        "file:///h%00i\n");
    CHECK(files.paths == Paths{"/a/b.txt", "/c/d", "/e"});
    CHECK(files.refused == 3);
}

TEST_CASE("clipboard: Ctrl+C and Ctrl+X offer the files to other programs") {
    Fixture f;
    f.show(f.root / "photos", "P1.ORF");
    CHECK(f.press(key(XKB_KEY_c, {.ctrl = true})));
    const fs::path p1 = f.root / "photos" / "P1.ORF";
    CHECK(f.clipboard.ours);
    CHECK(f.clipboard.data(views::kGnomeFiles) == "copy\n" + rummage::thumbs::file_uri(p1));
    CHECK(f.clipboard.data(views::kUriList) == rummage::thumbs::file_uri(p1) + "\r\n");
    CHECK(f.clipboard.data(views::kText) == p1.string());
    CHECK(f.clipboard.data(views::kKdeCut).empty());

    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK(f.clipboard.data(views::kGnomeFiles).starts_with("cut\n"));
    CHECK(f.clipboard.data(views::kKdeCut) == "1");

    // rummage's own: Ctrl+V moves them once, then the clipboard is emptied.
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.clipboard.cleared == 1);

    // Another program copies: what rummage took is forgotten, the cut marks go.
    f.show(f.root / "photos", "P2.ORF");
    CHECK(f.press(key(XKB_KEY_x, {.ctrl = true})));
    CHECK_FALSE(f.browser.cut_marks().names.empty());
    f.clipboard.copy_from_elsewhere({{"text/plain", "hello"}});
    f.browser.clipboard_lost();
    CHECK(f.browser.cut_marks().names.empty());
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "The clipboard holds no files");
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp"});
}

TEST_CASE("clipboard: Ctrl+V pastes another program's files: copied, or moved when cut") {
    Fixture f;
    const fs::path p2 = f.root / "photos" / "P2.ORF";
    const fs::path note = f.root / "other" / "note.txt";

    // A URI list from two folders: copied, each from its own.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kUriList}, views::uri_list(Paths{p2, note})}});
    f.show(f.root / "album", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P2.ORF", "note.txt"});
    CHECK(fs::exists(p2));
    CHECK(f.clipboard.cleared == 0);

    // GNOME's "cut": moved, sidecar along, and the clipboard emptied.
    const fs::path p1 = f.root / "photos" / "P1.ORF";
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{p1})},
         {std::string{views::kUriList}, views::uri_list(Paths{p1})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF", "note.txt"});
    CHECK(listing(f.root / "photos") == Names{"P2.ORF"});
    CHECK(f.clipboard.cleared == 1);

    // KDE's "cut": a URI list and a type of its own.
    f.clipboard.copy_from_elsewhere({{std::string{views::kUriList}, views::uri_list(Paths{note})},
                                     {std::string{views::kKdeCut}, "1"}});
    f.show(f.root / "photos", "");
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(listing(f.root / "photos") == Names{"P2.ORF", "note.txt"});
    CHECK(listing(f.root / "other").empty());

    // Not files here.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kUriList}, "https://example.com/a.jpg\r\n"}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.browser.status_bar().message() == "Only files on this computer can be pasted");
}

TEST_CASE("clipboard: another program's cut is emptied only once something moved") {
    Fixture f;
    const fs::path note = f.root / "other" / "note.txt";
    f.show(f.root / "album", "");

    // Nothing could move (the file is gone): the cut stays on the clipboard.
    const fs::path gone = f.root / "other" / "gone.txt";
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{gone})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    CHECK(f.clipboard.cleared == 0);
    CHECK_FALSE(f.clipboard.formats.empty());

    // Something else copied while the move ran: that is not emptied.
    f.clipboard.copy_from_elsewhere(
        {{std::string{views::kGnomeFiles}, views::gnome_copied_files(true, Paths{note})}});
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    f.clipboard.copy_from_elsewhere({{"text/plain", "newer"}});
    REQUIRE(f.settle());
    CHECK(listing(f.root / "album") == Names{"note.txt"});
    CHECK(f.clipboard.cleared == 0);
    CHECK(f.clipboard.data("text/plain") == "newer");
}

TEST_CASE("clipboard: Ctrl+V in a name's field pastes the text's first line") {
    Fixture f;
    f.show(f.root / "photos", "P2.ORF");
    f.clipboard.copy_from_elsewhere({{"UTF8_STRING", "Lake\tComo\nsecond line"}});
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    CHECK(f.press(key(XKB_KEY_v, {.ctrl = true})));
    REQUIRE(f.settle());
    // F2 selects the name without its extension: the pasted text takes its place.
    CHECK(f.browser.name_editor().field().text() == "LakeComo.ORF");
    // Ctrl+Z gives the name back as it was.
    CHECK(f.press(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(f.browser.name_editor().field().text() == "P2.ORF");
    CHECK(f.press(key(XKB_KEY_Escape)));
}
