#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::ui::TextField;
using rummage::views::Browser;
using rummage::views::Folder;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace ops = rummage::fs;

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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

KeyEvent typed(std::string text) { return key(XKB_KEY_NoSymbol, {}, std::move(text)); }

std::string selected(const TextField& field) {
    const auto [first, last] = field.selection();
    return field.text().substr(first, last - first);
}

std::vector<std::string> listing(const fs::path& directory) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

}  // namespace

TEST_CASE("operations: names that cannot be") {
    CHECK(ops::name_problem("photo.ORF") == std::nullopt);
    CHECK(ops::name_problem(" spaced ") == std::nullopt);
    CHECK(ops::name_problem("").has_value());
    CHECK(ops::name_problem(".").has_value());
    CHECK(ops::name_problem("..").has_value());
    CHECK(ops::name_problem("a/b") == std::optional<std::string>{"A name cannot contain /"});
    CHECK(ops::name_problem(std::string(256, 'x')).has_value());
    CHECK(ops::name_problem(std::string(255, 'x')) == std::nullopt);
}

TEST_CASE("operations: a new folder takes the first free name") {
    TempDir dir;
    CHECK(ops::make_folder(dir.path, "New Folder").name == "New Folder");
    CHECK(ops::make_folder(dir.path, "New Folder").name == "New Folder 2");
    static_cast<void>(dir.write("New Folder 3", "a file takes the name too"));
    CHECK(ops::make_folder(dir.path, "New Folder").name == "New Folder 4");
    CHECK(fs::is_directory(dir.path / "New Folder 4"));
    const ops::MadeFolder missing = ops::make_folder(dir.path / "missing", "New Folder");
    CHECK(missing.name.empty());
    CHECK(missing.error == std::errc::no_such_file_or_directory);
}

TEST_CASE("operations: darktable sidecars follow their photo") {
    using Renames = std::vector<std::pair<std::string, std::string>>;
    const std::vector<std::string> names{"P1.ORF",          "P1.ORF.xmp",    "P1_01.ORF.xmp",
                                         "P1_12.ORF.xmp",   "P1_1.ORF.xmp",  "P1_ab.ORF.xmp",
                                         "P1.JPG.xmp",      "P10.ORF.xmp",   "P1_2024.ORF",
                                         "P1_2024.ORF.xmp", "P1.ORF.xmp.bak"};
    CHECK(ops::sidecar_renames(names, "P1.ORF", "Lake.ORF") ==
          Renames{{"P1.ORF.xmp", "Lake.ORF.xmp"},
                  {"P1_01.ORF.xmp", "Lake_01.ORF.xmp"},
                  {"P1_12.ORF.xmp", "Lake_12.ORF.xmp"}});
    // A new extension goes into the duplicates' names too.
    CHECK(ops::sidecar_renames(names, "P1.ORF", "Lake.orf") ==
          Renames{{"P1.ORF.xmp", "Lake.orf.xmp"},
                  {"P1_01.ORF.xmp", "Lake_01.orf.xmp"},
                  {"P1_12.ORF.xmp", "Lake_12.orf.xmp"}});
    CHECK(ops::sidecar_renames(names, "P1.ORF", "P1.ORF").empty());
    CHECK(ops::sidecar_renames(names, "P1.ORF.xmp", "x.xmp").empty());  // a sidecar alone
}

TEST_CASE("operations: rename never replaces, and takes the sidecars along") {
    TempDir dir;
    static_cast<void>(dir.write("P1.ORF", "raw"));
    static_cast<void>(dir.write("P1.ORF.xmp", "edits"));
    static_cast<void>(dir.write("P1_01.ORF.xmp", "duplicate"));
    static_cast<void>(dir.write("other.txt", "other"));

    ops::Renamed renamed = ops::rename_entry(dir.path, "P1.ORF", "Lake.ORF");
    CHECK_FALSE(renamed.error);
    CHECK(renamed.sidecars == 2);
    CHECK(listing(dir.path) ==
          std::vector<std::string>{"Lake.ORF", "Lake.ORF.xmp", "Lake_01.ORF.xmp", "other.txt"});

    // A taken name: nothing moves.
    renamed = ops::rename_entry(dir.path, "Lake.ORF", "other.txt");
    CHECK(renamed.error == std::errc::file_exists);
    CHECK(renamed.problem == "other.txt already exists");
    // A taken sidecar name: nothing moves either, the photo included.
    static_cast<void>(dir.write("Sea.ORF.xmp", "someone else's"));
    renamed = ops::rename_entry(dir.path, "Lake.ORF", "Sea.ORF");
    CHECK(renamed.error == std::errc::file_exists);
    CHECK(renamed.problem == "Sea.ORF.xmp already exists");
    CHECK(fs::exists(dir.path / "Lake.ORF"));
    std::ifstream other{dir.path / "other.txt"};
    std::string text;
    std::getline(other, text);
    CHECK(text == "other");

    // A folder is renamed alone.
    fs::create_directory(dir.path / "trip");
    static_cast<void>(dir.write("trip.xmp", "not a sidecar of a folder"));
    CHECK_FALSE(ops::rename_entry(dir.path, "trip", "journey").error);
    CHECK(fs::exists(dir.path / "trip.xmp"));

    renamed = ops::rename_entry(dir.path, "missing", "x");
    CHECK(renamed.error == std::errc::no_such_file_or_directory);
}

TEST_CASE("operations: a folder without write permission says so") {
    if (geteuid() == 0) {
        return;  // root writes anywhere
    }
    TempDir dir;
    static_cast<void>(dir.write("a.txt", "a"));
    fs::permissions(dir.path, fs::perms::owner_read | fs::perms::owner_exec);
    const ops::Renamed renamed = ops::rename_entry(dir.path, "a.txt", "b.txt");
    const ops::MadeFolder made = ops::make_folder(dir.path, "New Folder");
    fs::permissions(dir.path, fs::perms::owner_all);
    CHECK(renamed.error == std::errc::permission_denied);
    CHECK(renamed.problem == "Cannot rename a.txt: Permission denied");
    CHECK(made.error == std::errc::permission_denied);
}

TEST_CASE("text field: typing, selecting, deleting, by characters and by words") {
    TextField field{"photo.ORF"};
    CHECK(field.caret() == 9);
    field.select(0, 5);
    CHECK(selected(field) == "photo");
    CHECK(field.key(typed("lake")));  // replaces the selection
    CHECK(field.text() == "lake.ORF");
    CHECK(field.caret() == 4);

    CHECK(field.key(key(XKB_KEY_End)));
    CHECK(field.key(key(XKB_KEY_Left, {.shift = true})));
    CHECK(selected(field) == "F");
    CHECK(field.key(key(XKB_KEY_Left)));  // collapses to the selection's start
    CHECK(field.caret() == 7);
    CHECK(field.key(key(XKB_KEY_BackSpace)));
    CHECK(field.text() == "lake.OF");
    CHECK(field.key(key(XKB_KEY_Home)));
    CHECK(field.key(key(XKB_KEY_Delete)));
    CHECK(field.text() == "ake.OF");

    // Words end at spaces, dots, dashes, underscores.
    field.set_text("my trip_2026.jpg");
    CHECK(field.key(key(XKB_KEY_Left, {.ctrl = true})));
    CHECK(field.caret() == 13);  // before "jpg"
    CHECK(field.key(key(XKB_KEY_Left, {.ctrl = true})));
    CHECK(field.caret() == 8);  // before "2026"
    CHECK(field.key(key(XKB_KEY_BackSpace, {.ctrl = true})));
    CHECK(field.text() == "my 2026.jpg");
    CHECK(field.key(key(XKB_KEY_Right, {.ctrl = true, .shift = true})));
    CHECK(selected(field) == "2026");
    CHECK(field.key(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(selected(field) == "my 2026.jpg");

    // UTF-8: one step is one character, never half of one.
    field.set_text("caffè");
    CHECK(field.key(key(XKB_KEY_BackSpace)));
    CHECK(field.text() == "caff");
    field.set_text("è");
    CHECK(field.key(key(XKB_KEY_Left)));
    CHECK(field.caret() == 0);
    field.select(1, 2);  // inside the character: moved back to its start
    CHECK(field.selection() == std::pair<std::size_t, std::size_t>{0, 2});

    // Not its keys; no control characters typed.
    CHECK_FALSE(field.key(key(XKB_KEY_Return)));
    CHECK_FALSE(field.key(key(XKB_KEY_Escape, {}, "\x1b")));
    CHECK_FALSE(field.key(key(XKB_KEY_c, {.ctrl = true}, "c")));
}

namespace {

// root/{alpha/, P1.ORF, P1.ORF.xmp, P1_01.ORF.xmp, notes.txt}
struct BrowserFixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::vector<std::pair<std::string, bool>> messages;
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};

    BrowserFixture() {
        fs::create_directories(root / "alpha");
        for (const char* const name : {"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "notes.txt"}) {
            std::ofstream{root / name} << name;
        }
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> folder() {
        switch (browser.mode()) {
            case Browser::Mode::grid:
                return browser.grid().folder();
            case Browser::Mode::details:
                return browser.details().folder();
            case Browser::Mode::columns:
                break;
        }
        return browser.columns().focused_folder();
    }

    std::string cursor() {
        const rummage::fs::Entry* entry = folder()->cursor_entry();
        return entry == nullptr ? std::string{} : entry->name;
    }

    // The editor closed after a rename, and the cursor landed on name.
    bool renamed_to(const std::string& name) {
        return run_until(loop, [&] { return !browser.name_editor().active() && cursor() == name; });
    }
};

}  // namespace

TEST_CASE("text field: Ctrl+Z undoes edits, Ctrl+Shift+Z redoes them") {
    TextField field{"P1.ORF"};
    field.select(0, 2);
    CHECK_FALSE(field.undo());  // nothing done yet
    // Typed over the selection, one character after another: one edit.
    for (const char* c : {"L", "a", "k", "e"}) {
        CHECK(field.key(typed(c)));
    }
    CHECK(field.text() == "Lake.ORF");
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.text() == "P1.ORF");
    CHECK(selected(field) == "P1");  // the selection as it was
    CHECK(field.key(key(XKB_KEY_Z, {.ctrl = true, .shift = true})));
    CHECK(field.text() == "Lake.ORF");
    CHECK(field.caret() == 4);

    // Removing characters one after another: one edit; moving the caret starts another.
    CHECK(field.key(key(XKB_KEY_BackSpace)));
    CHECK(field.key(key(XKB_KEY_BackSpace)));
    CHECK(field.text() == "La.ORF");
    CHECK(field.key(key(XKB_KEY_End)));
    CHECK(field.key(typed("x")));
    CHECK(field.text() == "La.ORFx");
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.text() == "La.ORF");
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.text() == "Lake.ORF");

    // A paste is one edit; a new edit forgets what could be redone.
    field.select(0, 4);
    field.insert("Lago di Como");
    CHECK(field.text() == "Lago di Como.ORF");
    CHECK(field.undo());
    CHECK(field.text() == "Lake.ORF");
    CHECK(field.key(typed("y")));
    CHECK_FALSE(field.redo());
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));  // nothing left is still a key it uses
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.key(key(XKB_KEY_z, {.ctrl = true})));
    CHECK(field.text() == "P1.ORF");

    // A new text: nothing to undo.
    field.set_text("other");
    CHECK_FALSE(field.undo());
}

TEST_CASE("browser: F2 renames in place, the extension left out, sidecars along") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        BrowserFixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.folder()->select_name("P1.ORF");
        REQUIRE(f.settle());

        CHECK(f.press(key(XKB_KEY_F2)));
        rummage::views::NameEditor& editor = f.browser.name_editor();
        REQUIRE(editor.active());
        CHECK(selected(editor.field()) == "P1");
        // Every key is the field's meanwhile: Space, Ctrl+2 and arrows do not reach the view.
        CHECK(f.press(key(XKB_KEY_space, {}, " ")));
        CHECK(f.press(key(XKB_KEY_2, {.ctrl = true})));
        CHECK(f.browser.mode() == mode);
        CHECK_FALSE(f.browser.quick_look().is_open());
        CHECK(editor.field().text() == " .ORF");
        CHECK(f.press(key(XKB_KEY_Home)));
        CHECK(f.press(key(XKB_KEY_Delete)));
        CHECK(f.press(typed("Lake")));
        CHECK(f.press(key(XKB_KEY_Return)));
        REQUIRE(f.renamed_to("Lake.ORF"));
        CHECK(f.browser.status_bar().message() == "Renamed Lake.ORF with 2 darktable sidecars");
        CHECK(listing(f.root) == std::vector<std::string>{"Lake.ORF", "Lake.ORF.xmp",
                                                          "Lake_01.ORF.xmp", "alpha", "notes.txt"});
    }
}

TEST_CASE("browser: a refused name keeps the field; Escape leaves everything as it was") {
    BrowserFixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(f.settle());
    f.folder()->select_name("notes.txt");
    REQUIRE(f.settle());
    rummage::views::NameEditor& editor = f.browser.name_editor();

    CHECK(f.press(key(XKB_KEY_F2)));
    CHECK(f.press(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(f.press(typed("a/b")));
    CHECK(f.press(key(XKB_KEY_Return)));  // refused at once
    CHECK(editor.active());
    CHECK_FALSE(editor.busy());
    CHECK(f.browser.status_bar().message() == "A name cannot contain /");

    CHECK(f.press(key(XKB_KEY_a, {.ctrl = true})));
    CHECK(f.press(typed("P1.ORF")));
    CHECK(f.press(key(XKB_KEY_KP_Enter)));  // taken: the worker says so, the field stays
    REQUIRE(run_until(f.loop, [&] { return !editor.busy(); }));
    CHECK(editor.active());
    CHECK(f.browser.status_bar().message() == "P1.ORF already exists");

    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(editor.active());
    CHECK(f.cursor() == "notes.txt");
    CHECK(fs::exists(f.root / "notes.txt"));

    // Enter on an unchanged name just closes.
    CHECK(f.press(key(XKB_KEY_F2)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK_FALSE(editor.active());
}

TEST_CASE("browser: Ctrl+Shift+N makes a folder and edits its name") {
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::grid, Browser::Mode::details}) {
        BrowserFixture f;
        f.browser.open(f.root);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        f.folder()->select_name("notes.txt");
        REQUIRE(f.settle());
        rummage::views::NameEditor& editor = f.browser.name_editor();

        CHECK(f.press(key(XKB_KEY_N, {.ctrl = true, .shift = true})));
        REQUIRE(run_until(f.loop, [&] { return editor.active(); }));
        CHECK(fs::is_directory(f.root / "New Folder"));
        CHECK(selected(editor.field()) == "New Folder");  // all of it: a folder has no extension
        CHECK(f.press(typed("Trip.2026")));
        CHECK(f.press(key(XKB_KEY_Return)));
        REQUIRE(f.renamed_to("Trip.2026"));
        CHECK(fs::is_directory(f.root / "Trip.2026"));

        // Again: the editor's Escape keeps the name it was given.
        CHECK(f.press(key(XKB_KEY_n, {.ctrl = true, .shift = true})));
        REQUIRE(run_until(f.loop, [&] { return editor.active(); }));
        CHECK(f.press(key(XKB_KEY_Escape)));
        CHECK(fs::is_directory(f.root / "New Folder"));
        REQUIRE(run_until(f.loop, [&] { return f.cursor() == "New Folder"; }));
    }
}

TEST_CASE("browser: F2 with nothing under the cursor, and in the sidebar, does nothing") {
    BrowserFixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_F2)));  // the shortcut, with no entry to rename
    CHECK_FALSE(f.browser.name_editor().active());
    f.folder()->select_name("notes.txt");
    REQUIRE(f.settle());
    f.browser.sidebar().set_focused(true);
    CHECK_FALSE(f.press(key(XKB_KEY_F2)));
    CHECK_FALSE(f.browser.name_editor().active());
}

TEST_CASE("archive password: Unicode editing, paste, confirmation and cancellation") {
    rummage::views::ConfirmDialog dialog{[] {}};
    std::optional<std::string> answer;
    unsigned answered = 0;
    const auto receive = [&](auto value) {
        answer = std::move(value);
        ++answered;
    };
    dialog.open_password("secret.7z", false, receive);
    REQUIRE(dialog.is_open());
    dialog.key(typed("päss 🔒"));
    dialog.key(key(XKB_KEY_BackSpace));
    dialog.paste("word");
    dialog.paste("invalid\nline");
    dialog.key(key(XKB_KEY_Return));
    CHECK_FALSE(dialog.is_open());
    CHECK(answer == "päss word");
    CHECK(answered == 1);
    dialog.open_password("secret.7z", true, receive);
    CHECK(dialog.detail().starts_with("Password rejected"));
    dialog.key(typed("discard"));
    dialog.key(key(XKB_KEY_Escape));
    CHECK_FALSE(answer);
    CHECK(answered == 2);
    dialog.open_password("secret.7z", false, receive);
    dialog.key(key(XKB_KEY_Return));
    CHECK(answer == "");  // a new prompt cannot reuse the previous password
}
