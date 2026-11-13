#include "rummage/views/context_menu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::ContextMenu;
using rummage::views::Folder;
using rummage::views::MenuItem;
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

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

MenuItem item(std::string id, bool enabled = true, std::function<void()> action = nullptr) {
    MenuItem out;
    out.label = id;
    out.id = std::move(id);
    out.enabled = enabled;
    out.action = std::move(action);
    return out;
}

// The ids of the menu's items, and which can be chosen ("-paste": shown, not usable).
Names ids(const std::vector<MenuItem>& items) {
    Names out;
    for (const MenuItem& entry : items) {
        out.push_back(entry.enabled ? entry.id : "-" + entry.id);
    }
    return out;
}

// photos/{a.txt, b.txt, P1.ORF, P1.ORF.xmp, sub/}, shown in the details.
struct Fixture {
    TempDir dir;
    fs::path photos = dir.path / "photos";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    // What "Open With" was asked and did.
    std::vector<Browser::AppChoice> apps{
        {.id = "viewer.desktop", .name = "Viewer", .is_default = true},
        {.id = "editor.desktop", .name = "Editor"}};
    std::string asked_type;
    std::string opened_with;
    Names opened;
    std::vector<fs::path> terminals;  // where F4 and "Open in Terminal" opened one

    Fixture() {
        fs::create_directories(photos / "sub");
        for (const char* name : {"a.txt", "b.txt", "P1.ORF", "P1.ORF.xmp"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(dir.path / "Trash");
        browser.set_templates_dir(dir.path / "templates");
        browser.set_open_with_handlers(
            [this](const fs::path& /*directory*/, const rummage::fs::Entry& entry,
                   const std::function<void(std::vector<Browser::AppChoice>)>& done) {
                asked_type = entry.name;
                done(apps);
            },
            [this](const fs::path& /*directory*/, const std::vector<rummage::fs::Entry>& files,
                   std::string app_id) {
                opened_with = std::move(app_id);
                opened.clear();
                for (const rummage::fs::Entry& file : files) {
                    opened.push_back(file.name);
                }
            });
        browser.set_terminal_handler(
            [this](const fs::path& directory) { terminals.push_back(directory); });
        browser.open(photos);
        browser.set_mode(Browser::Mode::details);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    std::shared_ptr<Folder> folder() { return browser.details().folder(); }

    ContextMenu& menu() { return browser.context_menu(); }

    void select(const Names& names) {
        folder()->set_cursor(std::nullopt);
        for (const std::string& name : names) {
            const auto& entries = folder()->entries();
            const auto it = std::ranges::find(entries, name, &rummage::fs::Entry::name);
            REQUIRE(it != entries.end());
            folder()->set_cursor(static_cast<std::size_t>(it - entries.begin()),
                                 rummage::views::SelectMode::toggle);
        }
    }

    // Down until the item called id is highlighted, then Enter.
    void choose(const std::string& id) {
        for (std::size_t i = 0; i < 20; ++i) {
            const std::optional<std::size_t> at = menu().highlighted();
            if (at && menu().items()[*at].id == id) {
                CHECK(press(key(XKB_KEY_Return)));
                return;
            }
            press(key(XKB_KEY_Down));
        }
        FAIL("no item " << id);
    }
};

}  // namespace

TEST_CASE("context menu: the keyboard moves over what can be chosen, in and out of a submenu") {
    ContextMenu menu{[] {}};
    int ran = 0;
    MenuItem with = item("with");
    with.submenu = true;
    with.children = {item("first", true, [&ran] { ran = 1; }), item("off", false),
                     item("last", true, [&ran] { ran = 3; })};
    MenuItem later = item("later");
    later.submenu = true;
    later.empty_text = "Looking…";
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; }), item("paste", false), with, later},
              true);
    REQUIRE(menu.is_open());
    CHECK(menu.highlighted() == 0);  // from the keyboard: the first, at once
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.highlighted() == 2);  // over Paste, which cannot be chosen
    CHECK(menu.key(key(XKB_KEY_Up)));
    CHECK(menu.highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Up)));
    CHECK(menu.highlighted() == 3);  // wraps
    CHECK(menu.key(key(XKB_KEY_Home)));
    CHECK(menu.highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_q)));  // every key is the menu's: nothing happens
    CHECK(menu.is_open());

    // Right opens the submenu, its first item highlighted; Down skips the disabled one.
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.submenu_open());
    CHECK(menu.sub_highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.sub_highlighted() == 2);
    CHECK(menu.key(key(XKB_KEY_Left)));  // back out
    CHECK_FALSE(menu.submenu_open());
    CHECK(menu.highlighted() == 2);
    CHECK(menu.key(key(XKB_KEY_Return)));  // Enter on a submenu opens it
    CHECK(menu.submenu_open());
    CHECK(menu.key(key(XKB_KEY_Escape)));  // Escape: out of the submenu first
    CHECK(menu.is_open());
    CHECK_FALSE(menu.submenu_open());

    // A submenu filled later: the keyboard waiting in it lands on its first item.
    CHECK(menu.key(key(XKB_KEY_Down)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.submenu_open());
    CHECK_FALSE(menu.sub_highlighted().has_value());
    menu.set_submenu("later", {item("found", true, [&ran] { ran = 4; })});
    CHECK(menu.sub_highlighted() == 0);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 4);
    CHECK_FALSE(menu.is_open());  // closed before the action ran

    // Escape closes; Enter runs an item.
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, true);
    CHECK(menu.key(key(XKB_KEY_Escape)));
    CHECK_FALSE(menu.is_open());
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, true);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 10);

    // A click before it was ever drawn lands outside: it closes, nothing runs.
    ran = 0;
    menu.open(10, 10, {item("open", true, [&ran] { ran = 10; })}, false);
    CHECK_FALSE(menu.highlighted().has_value());  // by the pointer: nothing highlighted yet
    rummage::ui::PointerEvent click;
    click.kind = rummage::ui::PointerEvent::Kind::press;
    click.button = rummage::ui::PointerButton::left;
    CHECK(menu.pointer(click));
    CHECK_FALSE(menu.is_open());
    CHECK(ran == 0);
}

TEST_CASE("context menu: the items for one entry, several, empty space; paste once taken") {
    Fixture f;
    // One file: everything, Rename too; nothing taken yet, so no Paste.
    f.select({"a.txt"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.menu().is_open());
    CHECK(ids(f.menu().items()) == Names{"open", "open_with", "copy", "cut", "-paste", "rename",
                                         "trash", "erase", "properties", "action:copy_location",
                                         "sort", "panes"});
    CHECK(f.menu().items()[2].keys == "Ctrl+C");  // from the shortcuts table
    CHECK(f.menu().items()[6].keys == "Delete");
    CHECK(f.press(key(XKB_KEY_Escape)));
    CHECK_FALSE(f.menu().is_open());

    // Several: no Rename. A folder: Open With and a terminal too.
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    CHECK(ids(f.menu().items()) == Names{"open", "open_with", "copy", "cut", "-paste", "-rename",
                                         "trash", "erase", "-properties", "action:copy_location",
                                         "sort", "panes"});
    f.menu().close();
    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_F10, {.shift = true})));
    CHECK(ids(f.menu().items()) == Names{"open", "open_with", "open_tab", "open_terminal",
                                         "add_bookmark", "copy", "cut", "-paste", "rename", "trash",
                                         "erase", "properties", "action:copy_location", "sort",
                                         "panes"});

    // Copy from the menu; then Paste can be chosen, here and on empty space.
    f.choose("copy");
    CHECK_FALSE(f.menu().is_open());
    CHECK(f.browser.status_bar().message() == "Ctrl+V copies “sub” where you want them");
    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(ids(f.menu().items()) == Names{"new_folder", "new_file", "paste", "add_bookmark",
                                         "open_terminal", "action:copy_location", "sort",
                                         "properties", "panes"});
    REQUIRE(f.settle());  // the templates looked for: none
    CHECK(ids(f.menu().items()[1].children) == Names{"empty_file"});
}

TEST_CASE("context menu: Sort By sorts every view, and the folders they open later") {
    Fixture f;
    // Sizes 1, 30, 200 and 5000 bytes; "sub" stays first, folders before files.
    std::ofstream{f.photos / "a.txt"} << std::string(5000, 'a');
    std::ofstream{f.photos / "b.txt"} << std::string(1, 'b');
    std::ofstream{f.photos / "P1.ORF"} << std::string(200, 'p');
    std::ofstream{f.photos / "P1.ORF.xmp"} << std::string(30, 'x');
    fs::create_directories(f.photos / "sub" / "deeper");
    std::ofstream{f.photos / "sub" / "big"} << std::string(900, 'g');
    std::ofstream{f.photos / "sub" / "small"} << "s";
    f.browser.set_mode(Browser::Mode::grid);
    REQUIRE(run_until(f.loop, [&f] {
        const auto& entries = f.browser.grid().folder()->entries();
        const auto a = std::ranges::find(entries, "a.txt", &rummage::fs::Entry::name);
        return a != entries.end() && a->size == 5000;
    }));
    const auto names = [](const Folder& folder) {
        Names out;
        for (const rummage::fs::Entry& entry : folder.entries()) {
            out.push_back(entry.name);
        }
        return out;
    };
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "b.txt", "P1.ORF", "P1.ORF.xmp"});

    // The menu: a submenu with a dot on the order in use, on empty space and on an entry.
    f.select({});
    f.browser.open_menu(100, 100, false);
    const MenuItem& sort = *std::ranges::find(f.menu().items(), "sort", &MenuItem::id);
    REQUIRE(sort.id == "sort");
    CHECK(sort.submenu);
    CHECK(ids(sort.children) == Names{"sort:name", "sort:size", "sort:modified", "sort:type",
                                      "sort:ascending", "sort:descending"});
    const auto checked = [](const MenuItem& submenu) {
        Names out;
        for (const MenuItem& item : submenu.children) {
            CHECK(item.radio);
            if (item.checked) {
                out.push_back(item.id);
            }
        }
        return out;
    };
    CHECK(checked(sort) == Names{"sort:name", "sort:ascending"});

    // Size, then Descending: the grid's folder follows, largest first.
    f.choose("sort");
    CHECK(f.press(key(XKB_KEY_Right)));  // into the submenu, on Name
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Return)));  // Size
    CHECK_FALSE(f.menu().is_open());
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "b.txt", "P1.ORF.xmp", "P1.ORF", "a.txt"});
    f.select({"a.txt"});
    f.browser.open_menu(100, 100, false);
    const auto sorted = std::ranges::find(f.menu().items(), "sort", &MenuItem::id);
    REQUIRE(sorted != f.menu().items().end());
    CHECK(checked(*sorted) == Names{"sort:size", "sort:ascending"});
    sorted->children.back().action();  // Descending
    f.menu().close();
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "P1.ORF", "P1.ORF.xmp", "b.txt"});
    CHECK(f.browser.state().sort_key == rummage::fs::SortKey::size);
    CHECK(f.browser.state().sort_descending);

    // A folder opened afterwards, in the columns, is sorted so too; the details show it.
    f.browser.open(f.photos / "sub");
    f.browser.set_mode(Browser::Mode::columns);
    REQUIRE(f.settle());
    REQUIRE(run_until(
        f.loop, [&f] { return f.browser.columns().focused_folder()->entries().size() == 3; }));
    CHECK(names(*f.browser.columns().focused_folder()) == Names{"deeper", "big", "small"});
    CHECK(f.browser.details().order().key == rummage::fs::SortKey::size);

    // A click on a title of the details sorts the other views too.
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().sort_by(rummage::views::DetailsView::Field::name);  // name, ascending
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.open(f.photos);
    REQUIRE(f.settle());
    CHECK(names(*f.browser.grid().folder()) ==
          Names{"sub", "a.txt", "b.txt", "P1.ORF", "P1.ORF.xmp"});
}

TEST_CASE("context menu: Open With lists the applications and opens the selection with one") {
    Fixture f;
    f.select({"sub", "b.txt", "a.txt"});  // the cursor on a.txt
    REQUIRE(f.folder()->cursor_entry() != nullptr);
    const std::string cursor = f.folder()->cursor_entry()->name;
    REQUIRE(cursor == "a.txt");
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.asked_type == cursor);  // the applications of the cursor's file
    const MenuItem& with = f.menu().items()[1];
    REQUIRE(with.id == "open_with");
    REQUIRE(with.children.size() == 2);
    CHECK(with.children[0].label == "Viewer (default)");
    CHECK(with.children[1].label == "Editor");
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(f.opened_with == "editor.desktop");
    std::ranges::sort(f.opened);
    CHECK(f.opened == Names{"a.txt", "b.txt"});  // the files; folders are not opened

    // No applications: a dimmed line says so.
    f.apps.clear();
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.menu().items()[1].children.size() == 1);
    CHECK(f.menu().items()[1].children[0].label == "No applications");
    CHECK_FALSE(f.menu().items()[1].children[0].enabled);
}

TEST_CASE("context menu: F4 and Open in Terminal: the cursor's folder, else the one shown") {
    Fixture f;
    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_F4)));
    f.select({"a.txt"});
    CHECK(f.press(key(XKB_KEY_F4)));
    CHECK(f.terminals == std::vector<fs::path>{f.photos / "sub", f.photos});

    f.select({"sub"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.menu().items()[3].keys == "F4");
    f.choose("open_terminal");
    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("open_terminal");
    CHECK(f.terminals ==
          std::vector<fs::path>{f.photos / "sub", f.photos, f.photos / "sub", f.photos});

    // A folder opens with a chosen application; the files selected with it are left out.
    f.select({"a.txt", "sub"});  // the cursor on sub
    REQUIRE(f.folder()->cursor_entry() != nullptr);
    REQUIRE(f.folder()->cursor_entry()->name == "sub");
    CHECK(f.press(key(XKB_KEY_Menu)));
    CHECK(f.asked_type == "sub");
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(f.opened_with == "viewer.desktop");
    CHECK(f.opened == Names{"sub"});
}

TEST_CASE("context menu: Move to Trash and Rename from the menu do what their keys do") {
    Fixture f;
    f.select({"P1.ORF"});
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("trash");
    REQUIRE(f.settle());
    CHECK(fs::exists(f.dir.path / "Trash" / "files" / "P1.ORF.xmp"));
    CHECK_FALSE(fs::exists(f.photos / "P1.ORF"));

    REQUIRE(run_until(f.loop, [&] { return f.folder()->cursor_entry() != nullptr; }));
    CHECK(f.press(key(XKB_KEY_Menu)));
    f.choose("rename");
    CHECK(f.browser.name_editor().active());
}

TEST_CASE("context menu: New File, empty or from a template, never over an existing file") {
    Fixture f;
    const fs::path templates = f.dir.path / "templates";
    fs::create_directories(templates / "folder");
    std::ofstream{templates / "Letter.txt"} << "Dear,";
    chmod((templates / "Letter.txt").c_str(), 0640);
    std::ofstream{templates / ".hidden"} << "x";
    std::ofstream{templates / "Notes.md~"} << "x";
    CHECK(rummage::fs::templates_in(templates) == Names{"Letter.txt"});

    f.select({});
    CHECK(f.press(key(XKB_KEY_Menu)));
    REQUIRE(f.settle());
    CHECK(ids(f.menu().items()[1].children) == Names{"empty_file", "template:Letter.txt"});
    CHECK(f.press(key(XKB_KEY_Down)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));  // Empty File
    REQUIRE(f.settle());
    CHECK(fs::is_regular_file(f.photos / "New File"));
    CHECK(fs::file_size(f.photos / "New File") == 0);
    REQUIRE(run_until(f.loop, [&] { return f.browser.name_editor().active(); }));
    f.browser.name_editor().cancel();

    for (int i = 0; i < 2; ++i) {
        f.select({});
        CHECK(f.press(key(XKB_KEY_Menu)));
        REQUIRE(f.settle());
        CHECK(f.press(key(XKB_KEY_Down)));
        CHECK(f.press(key(XKB_KEY_Right)));
        CHECK(f.press(key(XKB_KEY_Down)));
        CHECK(f.press(key(XKB_KEY_Return)));  // Letter.txt
        REQUIRE(f.settle());
        f.browser.name_editor().cancel();
    }
    CHECK(read_file(f.photos / "Letter.txt") == "Dear,");
    CHECK(read_file(f.photos / "Letter 2.txt") == "Dear,");
    CHECK((fs::status(f.photos / "Letter 2.txt").permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read));
}

TEST_CASE("new file: numbered names, a template's contents, nothing left on failure") {
    const TempDir dir;
    std::ofstream{dir.path / "New File"} << "old";
    const rummage::fs::MadeFile empty = rummage::fs::make_file(dir.path, "New File");
    CHECK(empty.name == "New File 2");
    CHECK(read_file(dir.path / "New File") == "old");
    CHECK(rummage::fs::make_file(dir.path, "a.tar.gz").name == "a.tar.gz");
    CHECK(rummage::fs::make_file(dir.path, "a.tar.gz").name == "a.tar 2.gz");
    const rummage::fs::MadeFile missing =
        rummage::fs::make_file(dir.path, "x.txt", dir.path / "no template");
    CHECK(missing.error == std::errc::no_such_file_or_directory);
    CHECK_FALSE(fs::exists(dir.path / "x.txt"));
}

TEST_CASE("new file: the templates folder, as the user dirs say") {
    const TempDir dir;
    rummage::fs::PlaceSources sources{.home = dir.path / "home",
                                      .config_home = dir.path / "config"};
    fs::create_directories(sources.home / "Templates");
    fs::create_directories(sources.home / "Modelli");
    fs::create_directories(sources.config_home);
    CHECK(rummage::fs::templates_dir(sources) == sources.home / "Templates");  // no user-dirs
    std::ofstream{sources.config_home / "user-dirs.dirs"}
        << "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n";
    CHECK(rummage::fs::templates_dir(sources).empty());  // the file says nothing about it
    std::ofstream{sources.config_home / "user-dirs.dirs"}
        << "XDG_TEMPLATES_DIR=\"$HOME/Modelli\"\n";
    CHECK(rummage::fs::templates_dir(sources) == sources.home / "Modelli");
    std::ofstream{sources.config_home / "user-dirs.dirs"} << "XDG_TEMPLATES_DIR=\"$HOME/Nope\"\n";
    CHECK(rummage::fs::templates_dir(sources).empty());  // not there
}

TEST_CASE("actions: copy location shares menu and palette and captures its target") {
    Fixture f;
    std::vector<rummage::views::SystemClipboard::Format> offered;
    rummage::views::SystemClipboard clipboard;
    clipboard.offer = [&](auto formats) { offered = std::move(formats); };
    f.browser.set_system_clipboard(std::move(clipboard));
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    f.choose("action:copy_location");
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().second ==
          (f.photos / "a.txt").string() + '\n' + (f.photos / "b.txt").string());
    CHECK(offered.front().first == "text/plain;charset=utf-8");
    f.select({"sub"});
    auto items = f.browser.palette_items();
    const auto item =
        std::ranges::find(items, "action:copy_location", &rummage::views::PaletteItem::id);
    REQUIRE(item != items.end());
    f.select({"a.txt"});
    item->action();
    CHECK(offered.front().second == (f.photos / "sub").string());
    f.select({});
    f.browser.open_menu(100, 100, false);
    f.choose("action:copy_location");
    CHECK(offered.front().second == f.photos.string());
}

TEST_CASE("actions: custom handoffs share applicability and capture selection across navigation") {
    Fixture f;
    using rummage::core::Action;
    std::vector<Action> actions{{.id = "inspect",
                                 .label = "Inspect",
                                 .argv = {"inspector", "{files}"},
                                 .mime = {"text/plain"},
                                 .selection = Action::Selection::many}};
    SUBCASE("Actions submenu") {}
    SUBCASE("parent menu") { actions.front().placement = Action::Placement::menu; }
    const bool parent = actions.front().placement == Action::Placement::menu;
    f.browser.set_actions(actions);
    std::vector<std::string> launched;
    fs::path directory;
    bool available = true;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
            directory = context.directory;
        },
        [&](const auto&, const auto&) { return available; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    const auto group =
        std::ranges::find(f.menu().items(), parent ? "action:inspect" : "actions", &MenuItem::id);
    REQUIRE(group != f.menu().items().end());
    if (parent) {
        CHECK_FALSE(group->submenu);
        CHECK(std::ranges::find(f.menu().items(), "actions", &MenuItem::id) ==
              f.menu().items().end());
    } else {
        REQUIRE(group->children.size() == 1);
        CHECK(group->children.front().id == "action:inspect");
    }
    const auto run = parent ? group->action : group->children.front().action;
    f.menu().close();
    f.browser.open(f.photos / "sub");
    REQUIRE(f.settle());
    run();
    CHECK(directory == f.photos);
    CHECK(launched == std::vector<std::string>{"inspector", (f.photos / "a.txt").string(),
                                               (f.photos / "b.txt").string()});
    f.browser.open(f.photos);
    REQUIRE(f.settle());
    f.select({"a.txt", "sub"});
    const auto mixed = f.browser.palette_items();
    CHECK(std::ranges::find(mixed, "action:inspect", &rummage::views::PaletteItem::id) ==
          mixed.end());
    f.select({"a.txt"});
    const auto supported = f.browser.palette_items();
    REQUIRE(std::ranges::find(supported, "action:inspect", &rummage::views::PaletteItem::id) !=
            supported.end());
    available = false;
    f.browser.open_menu(100, 100, false);
    const auto missing =
        std::ranges::find(f.menu().items(), parent ? "action:inspect" : "actions", &MenuItem::id);
    REQUIRE(missing != f.menu().items().end());
    const bool enabled = parent ? missing->enabled : missing->children.front().enabled;
    CHECK_FALSE(enabled);
    const auto unavailable = f.browser.palette_items();
    CHECK(std::ranges::find(unavailable, "action:inspect", &rummage::views::PaletteItem::id) ==
          unavailable.end());
}

TEST_CASE("actions: parent menu actions precede the submenu in configuration order") {
    Fixture f;
    using rummage::core::Action;
    const std::vector<Action> actions{{.id = "goblin",
                                       .label = "Goblin",
                                       .argv = {"goblin"},
                                       .mime = {},
                                       .selection = Action::Selection::many},
                                      {.id = "compress",
                                       .label = "Compress…",
                                       .argv = {"archiver", "{files}"},
                                       .mime = {},
                                       .selection = Action::Selection::many,
                                       .placement = Action::Placement::menu},
                                      {.id = "inspect",
                                       .label = "Inspect",
                                       .argv = {"inspector", "{files}"},
                                       .mime = {},
                                       .selection = Action::Selection::many,
                                       .placement = Action::Placement::menu}};
    f.browser.set_actions(actions);
    std::vector<std::string> launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
        },
        [](const auto&, const auto&) { return true; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(100, 100, false);
    const auto& items = f.menu().items();
    const auto group = std::ranges::find(items, "actions", &MenuItem::id);
    REQUIRE(group != items.end());
    REQUIRE(group - items.begin() >= 2);
    CHECK((group - 2)->id == "action:compress");
    CHECK((group - 1)->id == "action:inspect");
    CHECK(ids(group->children) == Names{"action:goblin"});
    f.choose("action:compress");
    CHECK(launched ==
          Names{"archiver", (f.photos / "a.txt").string(), (f.photos / "b.txt").string()});

    f.select({});
    f.browser.open_menu(100, 100, true);
    const auto background = ids(f.menu().items());
    CHECK(std::ranges::find(background, "action:compress") == background.end());
    CHECK(std::ranges::find(background, "actions") == background.end());
}

TEST_CASE("actions: archive presets appear in menu and palette and hand off literal paths") {
    Fixture f;
    const auto archive =
        f.dir.write("two words.zip", std::string{"PK\005\006"} + std::string(18, '\0'));
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.select({"two words.zip"});
    std::vector<std::string> launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = {action.id, context.targets.front().path.string()};
        },
        [](const auto&, const auto&) { return true; });
    f.browser.open_menu(100, 100, false);
    const auto here = std::ranges::find(f.menu().items(), "action:extract_here", &MenuItem::id);
    REQUIRE(here != f.menu().items().end());
    REQUIRE(here->enabled);
    f.choose("action:extract_here");
    CHECK(launched == std::vector<std::string>{"extract_here", archive.string()});
    const auto items = f.browser.palette_items();
    const auto to = std::ranges::find(items, "action:extract_to", &rummage::views::PaletteItem::id);
    REQUIRE(to != items.end());
    to->action();
    CHECK(launched == std::vector<std::string>{"extract_to", archive.string()});
    f.browser.set_action_handoff({}, {});
    const auto picker_items = f.browser.palette_items();
    CHECK(std::ranges::find(picker_items, "action:extract_to", &rummage::views::PaletteItem::id) ==
          picker_items.end());
}

TEST_CASE("context menu: keyboard enters arbitrary nesting and backs out one level") {
    ContextMenu menu{[] {}};
    int ran = 0;
    auto leaf = item("leaf", true, [&] {
        ++ran;
        CHECK_FALSE(menu.is_open());
    });
    auto nested = leaf;
    for (int depth = 0; depth < 8; ++depth) {
        auto group = item("group" + std::to_string(depth));
        group.submenu = true;
        group.children = {item("disabled", false), nested};
        nested = std::move(group);
    }
    menu.open(10, 10, {nested}, true);
    for (std::size_t depth = 1; depth <= 8; ++depth) {
        CHECK(menu.key(key(XKB_KEY_Right)));
        CHECK(menu.open_levels() == depth + 1);
        CHECK(menu.highlighted(depth) == 1);
    }
    CHECK(menu.key(key(XKB_KEY_Escape)));
    CHECK(menu.open_levels() == 8);
    CHECK(menu.key(key(XKB_KEY_Left)));
    CHECK(menu.open_levels() == 7);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 1);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("context menu: asynchronously filled nested groups preserve keyboard navigation") {
    ContextMenu menu{[] {}};
    int ran = 0;
    auto dynamic = item("open_with");
    dynamic.submenu = true;
    dynamic.empty_text = "Loading…";
    auto outer = item("outer");
    outer.submenu = true;
    outer.children = {dynamic};
    menu.open(10, 10, {outer}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.open_levels() == 3);
    CHECK_FALSE(menu.highlighted(2));
    menu.set_submenu("open_with", {item("viewer", true, [&] { ++ran; })});
    CHECK(menu.highlighted(2) == 0);
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(ran == 1);

    // Updating a parent while one of its descendants is open drops stale descendant indices.
    dynamic.children = {outer};
    menu.open(10, 10, {dynamic}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    menu.set_submenu("open_with", {item("replacement", false)});
    CHECK(menu.open_levels() == 2);
    CHECK_FALSE(menu.highlighted(1));
    CHECK(menu.key(key(XKB_KEY_Return)));
    CHECK(menu.is_open());

    auto templates = item("new_file");
    templates.submenu = true;
    menu.open(10, 10, {item("empty_file"), templates}, true);
    auto letter = item("template:Letter");
    letter.separator = true;
    menu.set_submenu("new_file", {item("empty_file"), letter});
    REQUIRE(menu.items()[1].children.size() == 1);
    CHECK(menu.items()[1].children[0].id == "template:Letter");
    CHECK_FALSE(menu.items()[1].children[0].separator);
}

TEST_CASE("context menu: layout moves built-ins and actions without duplicates") {
    const auto parsed = rummage::core::parse_config(R"(
[[actions]]
id = "script"
label = "Script"
argv = ["tool", "{files}"]
selection = "many"
[[actions]]
id = "external"
label = "External tool"
argv = ["external", "{files}"]
selection = "many"
[context_menu]
items = [
  { separator = true },
  { label = "My Actions", items = [
    { label = "Scripts", items = [{ id = "action:script", label = "Run script" }] },
    { label = "External", items = [{ id = "action:external" }, { id = "open_with" }] },
  ] },
  { id = "copy", label = "Copy these", separator = true },
  { label = "Not applicable", items = [{id = "restore"}] },
  { remaining = true },
  { separator = true },
]
background = [{ label = "Create", items = [{id = "new_file"}, {id = "new_folder"}] }]
sidebar = []
)",
                                                    "menu.toml");
    REQUIRE(parsed.problems.empty());
    Fixture f;
    f.browser.set_actions(parsed.config.actions);
    f.browser.set_context_menu(parsed.config.context_menu);
    Names launched;
    f.browser.set_action_handoff(
        [&](const auto& action, const auto& context) {
            launched = rummage::core::action_arguments(action, context);
        },
        [](const auto&, const auto&) { return true; });
    f.select({"a.txt", "b.txt"});
    f.browser.open_menu(10, 10, true);
    const auto& items = f.menu().items();
    REQUIRE(items.size() > 2);
    CHECK(items[0].label == "My Actions");
    CHECK_FALSE(items[0].separator);
    REQUIRE(items[0].children.size() == 2);
    const auto& scripts = items[0].children[0];
    CHECK(scripts.label == "Scripts");
    REQUIRE(scripts.children.size() == 1);
    CHECK(scripts.children.front().label == "Run script");
    const auto& external = items[0].children[1];
    CHECK(external.label == "External");
    REQUIRE(external.children.size() == 2);
    CHECK(external.children[1].id == "open_with");
    CHECK(external.children[1].children.size() == 2);  // async loader found its nested group
    CHECK(items[1].id == "copy");
    CHECK(items[1].keys == "Ctrl+C");
    CHECK(items[1].separator);
    const auto root_ids = ids(items);
    CHECK(std::ranges::count(root_ids, "copy") == 1);
    CHECK(std::ranges::find(root_ids, "actions") == root_ids.end());
    CHECK(std::ranges::find(root_ids, "open_with") == root_ids.end());
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Right)));
    CHECK(f.press(key(XKB_KEY_Return)));
    CHECK(launched == Names{"tool", (f.photos / "a.txt").string(), (f.photos / "b.txt").string()});
    // Layout hiding/relabeling doesn't remove registry actions from the palette.
    const auto palette = f.browser.palette_items();
    CHECK(std::ranges::find(palette, "action:script", &rummage::views::PaletteItem::id) !=
          palette.end());

    f.select({});
    f.browser.open_menu(10, 10, true);
    REQUIRE(f.menu().items().size() == 1);
    CHECK(f.menu().items()[0].label == "Create");
    REQUIRE(f.menu().items()[0].children.size() == 2);
    CHECK(f.menu().items()[0].children[0].id == "new_file");
    auto hidden = parsed.config.context_menu;
    hidden.background = std::vector<rummage::core::MenuNode>{};
    f.browser.set_context_menu(hidden);
    f.browser.open_menu(10, 10, true);
    CHECK_FALSE(f.menu().is_open());
    CHECK(f.settle());  // hiding a menu must not leave unstarted loaders counted as pending
}

TEST_CASE("context menu: remaining items may be nested and empty groups are omitted") {
    using rummage::core::MenuNode;
    auto group = item("actions");
    group.submenu = true;
    group.children = {item("action:a"), item("action:b")};
    const std::vector<MenuItem> available{item("copy"), group};
    const std::vector<MenuNode> layout{
        {.id = "action:a"},
        {.label = "Everything else", .items = {{.remaining = true}}},
        {.label = "Hidden", .items = {{.id = "unavailable"}}},
    };
    const auto items = rummage::views::arrange_menu(available, layout);
    REQUIRE(items.size() == 2);
    CHECK(items[0].id == "action:a");
    REQUIRE(items[1].children.size() == 2);
    CHECK(items[1].children[0].id == "copy");
    CHECK(ids(items[1].children[1].children) == Names{"action:b"});
}

TEST_CASE("context menu: pointer navigates nested panels and switches sibling groups") {
    rummage::ui::Offscreen gl{800, 400};
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    ContextMenu menu{[] {}};
    int ran = 0;
    auto scripts = item("scripts");
    scripts.label = "Scripts";
    scripts.submenu = true;
    scripts.children = {item("run", true, [&] { ran = 1; })};
    scripts.children.front().label = "Run script";
    auto external = scripts;
    external.id = "external";
    external.label = "External";
    external.children.front().action = [&] { ran = 2; };
    auto root = item("my_actions");
    root.label = "My Actions";
    root.submenu = true;
    root.children = {scripts, external};
    const auto draw = [&] {
        renderer.begin_frame(800, 400, 1.0F, rummage::ui::Color::rgb(0x202020));
        menu.draw(renderer, {0, 0, 800, 400}, rummage::theme::Palette{});
        renderer.end_frame();
    };
    const auto pointer = [&](float x, float y, bool click = false) {
        rummage::ui::PointerEvent event;
        event.kind = click ? rummage::ui::PointerEvent::Kind::press
                           : rummage::ui::PointerEvent::Kind::motion;
        event.button = rummage::ui::PointerButton::left;
        event.x = x;
        event.y = y;
        CHECK(menu.pointer(event));
        draw();
    };
    menu.open(10, 10, {root}, false);
    draw();
    pointer(30, 30);
    CHECK(menu.open_levels() == 2);
    pointer(200, 30);
    CHECK(menu.open_levels() == 3);
    pointer(380, 30);
    CHECK(menu.highlighted(2) == 0);
    pointer(200, 58);  // switch to the sibling, discarding Scripts' open descendants
    CHECK(menu.open_levels() == 3);
    CHECK(menu.highlighted(1) == 1);
    pointer(380, 58, true);
    CHECK(ran == 2);
    CHECK_FALSE(menu.is_open());

    // At the bottom/right edge, panels flip left and overlapping panels hit the deepest one.
    menu.open(790, 390, {root}, true);
    CHECK(menu.key(key(XKB_KEY_Right)));
    CHECK(menu.key(key(XKB_KEY_Right)));
    draw();
    pointer(620, 340, true);
    CHECK(ran == 1);
    CHECK_FALSE(menu.is_open());
}

TEST_CASE("properties: context menu and shortcut open a modal for files and the current folder") {
    Fixture f;
    f.select({"a.txt"});
    f.browser.open_menu(100, 100, true);
    f.choose("properties");
    auto& properties = f.browser.properties_dialog();
    REQUIRE(properties.is_open());
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos / "a.txt");
    CHECK_FALSE(properties.can_edit_icon());
    CHECK(f.browser.has_overlay());
    f.press(key(XKB_KEY_Delete));
    CHECK(fs::exists(f.photos / "a.txt"));
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(properties.is_open());
    f.select({});
    f.press(key(XKB_KEY_Return, {.alt = true}));
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos);
    CHECK(properties.can_edit_icon());
    properties.close();
    f.select({"a.txt", "b.txt"});
    f.press(key(XKB_KEY_Return, {.alt = true}));
    CHECK_FALSE(properties.is_open());
}

TEST_CASE(
    "properties: staging, cancel, apply and reset preserve metadata and refresh the selected "
    "folder") {
    Fixture f;
    f.select({"sub"});
    auto& properties = f.browser.properties_dialog();
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.choose_icon("folder-music");
    CHECK(properties.dirty());
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    properties.close();
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.pick_icon();
    REQUIRE(properties.picking());
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(properties.picking());
    CHECK(properties.is_open());
    properties.choose_icon("folder-music");
    // An external program changed another setting while Properties was open.
    std::ofstream{f.photos / "sub/.directory"} << "[Settings]\nView=3\n";
    properties.apply();
    REQUIRE(run_until(f.loop, [&] {
        const auto* entry = f.folder()->cursor_entry();
        return !properties.is_open() && entry && entry->folder_icon == "folder-music";
    }));
    CHECK(f.folder()->cursor_entry()->name == "sub");
    CHECK(read_file(f.photos / "sub/.directory").find("View=3") != std::string::npos);
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.choose_icon("");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] {
        const auto* entry = f.folder()->cursor_entry();
        return !properties.is_open() && entry && entry->folder_icon.empty();
    }));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=") == std::string::npos);
    CHECK(read_file(f.photos / "sub/.directory").find("View=3") != std::string::npos);
}

TEST_CASE("properties: failed saves stay open and do not recreate a removed folder") {
    Fixture f;
    f.select({"sub"});
    f.browser.show_properties();
    REQUIRE(f.settle());
    auto& properties = f.browser.properties_dialog();
    properties.choose_icon("/no/such/icon.png");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    CHECK(properties.is_open());
    CHECK_FALSE(properties.error().empty());
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    properties.choose_icon("folder-music");
    fs::remove(f.photos / "sub");
    properties.apply();
    REQUIRE(run_until(f.loop, [&] { return !properties.saving(); }));
    CHECK(properties.is_open());
    CHECK_FALSE(properties.error().empty());
    CHECK_FALSE(fs::exists(f.photos / "sub"));
}

TEST_CASE("properties: obsolete metadata load cannot replace a newly opened item's properties") {
    Fixture f;
    auto& properties = f.browser.properties_dialog();
    properties.open(f.photos / "sub/", [] {});
    REQUIRE(f.settle());
    CHECK(properties.can_edit_icon());
    properties.open(f.photos / "sub", [] {});
    properties.open(f.photos / "a.txt", [] {});
    REQUIRE(f.settle());
    CHECK(properties.path() == f.photos / "a.txt");
    CHECK_FALSE(properties.can_edit_icon());
    properties.open(f.photos / "sub", [] {});
    properties.close();
    REQUIRE(f.settle());
    CHECK_FALSE(properties.is_open());
}

TEST_CASE("properties: keyboard search selects a themed icon and custom artwork stays staged") {
    std::unique_ptr<rummage::ui::Offscreen> gl;
    try {
        gl = std::make_unique<rummage::ui::Offscreen>(800, 640);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return;
    }
    Fixture f;
    const char* data_home = std::getenv("XDG_DATA_HOME");
    REQUIRE(data_home != nullptr);
    const auto theme = fs::path{data_home} / "icons/rummage-properties-test";
    fs::create_directories(theme / "48");
    std::ofstream{theme / "index.theme"} << "[Icon Theme]\nDirectories=48\n[48]\nSize=48\n";
    const std::string svg =
        "<svg width='32' height='32'><rect width='32' height='32' fill='red'/></svg>";
    std::ofstream{theme / "48/folder-red.svg"} << svg;
    std::ofstream{theme / "48/folder-blue.svg"} << svg;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    icons.set_theme("rummage-properties-test");
    f.select({"sub"});
    f.browser.show_properties();
    REQUIRE(f.settle());
    auto& properties = f.browser.properties_dialog();
    const auto draw = [&] {
        renderer.begin_frame(800, 640, 1, rummage::ui::Color::rgb(0));
        properties.draw(renderer, {0, 0, 800, 640}, rummage::theme::Palette{}, icons);
        renderer.end_frame();
    };
    draw();
    f.press(key(XKB_KEY_Return));
    REQUIRE(properties.picking());
    draw();
    REQUIRE(f.settle());
    rummage::ui::KeyEvent query;
    query.text = "blue";
    f.press(query);
    draw();
    f.press(key(XKB_KEY_Tab));
    f.press(key(XKB_KEY_Return));
    CHECK_FALSE(properties.picking());
    CHECK(properties.icon() == "folder-blue");
    CHECK_FALSE(fs::exists(f.photos / "sub/.directory"));
    f.press(key(XKB_KEY_Return));  // Apply is focused after choosing.
    REQUIRE(run_until(f.loop, [&] { return !properties.is_open(); }));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=folder-blue") != std::string::npos);
    f.browser.show_properties();
    REQUIRE(f.settle());
    properties.pick_icon();
    draw();
    f.press(key(XKB_KEY_Tab));
    f.press(key(XKB_KEY_Tab));  // custom path
    f.press(key(XKB_KEY_a, {.ctrl = true}));
    properties.paste((theme / "48/folder-red.svg").string());
    f.press(key(XKB_KEY_Return));
    CHECK(properties.icon() == (theme / "48/folder-red.svg").string());
    f.press(key(XKB_KEY_Escape));
    CHECK(read_file(f.photos / "sub/.directory").find("Icon=folder-blue") != std::string::npos);
}

TEST_CASE("actions: missing archive tools remain actionable to explain installation") {
    Fixture f;
    f.dir.write("archive.zip", std::string{"PK\005\006"} + std::string(18, '\0'));
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.select({"archive.zip"});
    unsigned launched = 0;
    f.browser.set_action_handoff([&](const auto&, const auto&) { ++launched; },
                                 [](const auto&, const auto&) { return false; });
    f.browser.open_menu(100, 100, false);
    const auto item = std::ranges::find(f.menu().items(), "action:extract_here", &MenuItem::id);
    REQUIRE(item != f.menu().items().end());
    CHECK(item->enabled);
    CHECK(item->label.find("setup required") != std::string::npos);
    f.choose("action:extract_here");
    CHECK(launched == 1);
    const auto items = f.browser.palette_items();
    const auto to = std::ranges::find(items, "action:extract_to", &rummage::views::PaletteItem::id);
    REQUIRE(to != items.end());
    to->action();
    CHECK(launched == 2);
}

TEST_CASE("actions: errors remain queued and copyable when a later archive succeeds") {
    Fixture f;
    f.browser.show_action_result("First archive failed\nInstall unrar", true);
    f.browser.show_action_result("Second archive failed\nDisk full", true);
    f.browser.show_action_result("Extracted third archive", false);
    auto& errors = f.browser.action_errors();
    CHECK(errors.pending() == 2);
    CHECK(errors.detail() == "First archive failed\nInstall unrar");
    CHECK(f.browser.has_overlay());
    errors.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(errors.pending() == 1);
    CHECK(errors.detail() == "Second archive failed\nDisk full");
    errors.key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK_FALSE(errors.is_open());
}
