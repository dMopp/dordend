#include "rummage/views/command_palette.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
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
#include "rummage/fs/places.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/fuzzy.hpp"
#include "rummage/views/shortcuts.hpp"
#include "rummage/views/state.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::views::Browser;
using rummage::views::Command;
using rummage::views::CommandPalette;
using rummage::views::fuzzy_positions;
using rummage::views::fuzzy_score;
using rummage::views::PaletteItem;
using rummage::views::State;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    event.text = std::move(text);
    return event;
}

KeyEvent typed(char c) { return key(static_cast<xkb_keysym_t>(c), {}, std::string(1, c)); }

constexpr Modifiers kCtrl{.ctrl = true};

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

// The best match first: the labels of what the palette shows.
std::vector<std::string> labels(const CommandPalette& palette) {
    std::vector<std::string> out;
    for (const std::size_t index : palette.shown()) {
        out.push_back(palette.items()[index].label);
    }
    return out;
}

std::vector<PaletteItem> plain_items(const std::vector<std::string>& names) {
    std::vector<PaletteItem> items;
    items.reserve(names.size());
    for (const std::string& name : names) {
        items.push_back({.id = name,
                         .label = name,
                         .keys = {},
                         .remember = true,
                         .action = {},
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    }
    return items;
}

}  // namespace

TEST_CASE("fuzzy: in order anywhere, any case, best placement first") {
    CHECK(fuzzy_score("", "anything") == 0.0);
    CHECK(fuzzy_score("ntab", "New Tab"));
    CHECK(fuzzy_score("NEW", "New Tab"));  // case does not matter
    CHECK_FALSE(fuzzy_score("xyz", "New Tab"));
    CHECK_FALSE(fuzzy_score("New Tab and more", "New Tab"));  // longer than the text
    // Word starts and runs win.
    const auto score = [](std::string_view query, std::string_view text) {
        const std::optional<double> found = fuzzy_score(query, text);
        REQUIRE(found);
        return found.value_or(0.0);
    };
    CHECK(score("nt", "New Tab") > score("nt", "Open in Terminal"));
    CHECK(score("tab", "New Tab") > score("tab", "Show the Details Table"));
    CHECK(fuzzy_positions("nt", "New Tab") == std::vector<std::size_t>{0, 4});
    CHECK(fuzzy_positions("xyz", "New Tab").empty());
    CHECK(fuzzy_positions("", "New Tab").empty());
    // UTF-8 bytes are compared as they are.
    CHECK(fuzzy_score("città", "Go to Città"));
    CHECK(fuzzy_positions("tà", "Città").size() == 3);
}

TEST_CASE("palette: filters as it is typed, the best first, recent first when empty") {
    CommandPalette palette{[] {}};
    palette.open(plain_items({"Open in Terminal", "New Folder", "New Tab", "Next Tab",
                              "Show Hidden Files", "Sort by Name"}),
                 {"Sort by Name", "gone", "New Tab"}, "Type a command");
    REQUIRE(palette.is_open());
    // Nothing typed: the recently used (those still offered), then the rest in order.
    CHECK(labels(palette) == std::vector<std::string>{"Sort by Name", "New Tab", "Open in Terminal",
                                                      "New Folder", "Next Tab",
                                                      "Show Hidden Files"});
    CHECK(palette.highlighted() == 0);

    palette.key(typed('n'));
    palette.key(typed('f'));
    CHECK(palette.query() == "nf");
    REQUIRE_FALSE(palette.shown().empty());
    CHECK(labels(palette).front() == "New Folder");

    palette.set_query("ntab");
    const std::vector<std::string> tabs = labels(palette);
    CHECK(tabs.front() == "New Tab");
    CHECK(std::ranges::find(tabs, "Next Tab") != tabs.end());

    palette.set_query("zzz");
    CHECK(palette.shown().empty());
    CHECK_FALSE(palette.highlighted());
    palette.key(key(XKB_KEY_Return));  // nothing to run
    CHECK(palette.is_open());

    palette.key(key(XKB_KEY_BackSpace));
    palette.key(key(XKB_KEY_BackSpace));
    palette.key(key(XKB_KEY_BackSpace));
    CHECK(palette.query().empty());
    CHECK(palette.shown().size() == 6);
}

TEST_CASE("palette: arrows move within the list, Enter runs and closes, Escape closes") {
    CommandPalette palette{[] {}};
    std::vector<std::string> ran;
    std::vector<std::string> chosen;
    palette.set_chosen_handler([&](const std::string& id) { chosen.push_back(id); });
    std::vector<PaletteItem> items = plain_items({"One", "Two", "Three"});
    for (PaletteItem& item : items) {
        item.action = [&ran, label = item.label] { ran.push_back(label); };
    }
    items[2].remember = false;
    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_Up));  // already at the top
    CHECK(palette.highlighted() == 0);
    palette.key(key(XKB_KEY_Down));
    palette.key(key(XKB_KEY_Down));
    palette.key(key(XKB_KEY_Down));  // stays on the last
    CHECK(palette.highlighted() == 2);
    palette.key(key(XKB_KEY_Page_Up));
    CHECK(palette.highlighted() == 0);
    palette.key(key(XKB_KEY_Page_Down));
    CHECK(palette.highlighted() == 2);
    palette.key(key(XKB_KEY_Up));
    palette.key(key(XKB_KEY_Return));
    CHECK_FALSE(palette.is_open());
    CHECK(ran == std::vector<std::string>{"Two"});
    CHECK(chosen == std::vector<std::string>{"Two"});

    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_End));  // the field's: the caret
    palette.key(key(XKB_KEY_Page_Down));
    palette.choose_highlighted();
    CHECK(ran == std::vector<std::string>{"Two", "Three"});
    CHECK(chosen == std::vector<std::string>{"Two"});  // Three is not remembered

    palette.open(items, {}, "");
    palette.key(key(XKB_KEY_Escape));
    CHECK_FALSE(palette.is_open());
    CHECK(ran.size() == 2);

    // A paste: its first line, without control characters.
    palette.open(items, {}, "");
    palette.paste("Th\tree\nsecond line");
    CHECK(palette.query() == "Three");
    CHECK(labels(palette).front() == "Three");
}

TEST_CASE("palette: filtering 500 items takes well under a millisecond a key") {
    std::vector<std::string> names;
    names.reserve(500);
    for (int i = 0; i < 500; ++i) {
        names.push_back(std::format("Go to Some Folder Number {} in the Photos", i));
    }
    CommandPalette palette{[] {}};
    palette.open(plain_items(names), {}, "");
    const std::string query = "photos";
    const auto start = std::chrono::steady_clock::now();
    for (const char c : query) {
        palette.key(typed(c));
    }
    const auto each = (std::chrono::steady_clock::now() - start) / query.size();
    const double ms = std::chrono::duration<double, std::milli>(each).count();
    MESSAGE(std::format("filtering 500 items: {:.3f} ms a key", ms));
    CHECK(palette.shown().size() == 500);
    CHECK(ms < 10.0);  // the target is 1 ms in a release build; sanitizers are slower
}

namespace {

// a/{one.txt, sub/}, b/, in the columns at a, with two places.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    fs::path b = dir.path / "b";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(a / "sub");
        fs::create_directories(b);
        std::ofstream{a / "one.txt"} << "1";
        browser.set_home_trash(dir.path / "Trash");
        browser.set_places({{.section = rummage::fs::Place::Section::places,
                             .label = "Bee",
                             .path = b,
                             .icon = "folder",
                             .disk = {}}});
        browser.open(a);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) { return browser.key(event, Browser::Clock::now()); }

    [[nodiscard]] std::set<std::string> ids() {
        std::set<std::string> out;
        for (const PaletteItem& item : browser.palette_items()) {
            out.insert(item.id);
        }
        return out;
    }

    // Opens the palette, types query, runs the best match.
    void run(std::string_view query) {
        REQUIRE(press(key(XKB_KEY_k, kCtrl)));
        REQUIRE(browser.command_palette().is_open());
        browser.command_palette().set_query(std::string{query});
        REQUIRE(browser.command_palette().highlighted());
        press(key(XKB_KEY_Return));
        CHECK_FALSE(browser.command_palette().is_open());
        REQUIRE(settle());
    }
};

}  // namespace

TEST_CASE("palette in the browser: Ctrl+K opens and closes it, and it takes every key") {
    Fixture f;
    CHECK(f.press(key(XKB_KEY_k, kCtrl)));
    REQUIRE(f.browser.command_palette().is_open());
    CHECK(f.press(key(XKB_KEY_2, kCtrl)));  // not the grid: the palette's
    CHECK(f.browser.mode() == Browser::Mode::columns);
    CHECK(f.press(typed('g')));
    CHECK(f.browser.command_palette().query() == "g");
    CHECK(f.press(key(XKB_KEY_K, kCtrl)));  // Ctrl+K again (Caps Lock on)
    CHECK_FALSE(f.browser.command_palette().is_open());
    // The context menu gives way to it.
    f.browser.open_menu(100, 100, false);
    REQUIRE(f.browser.context_menu().is_open());
    f.browser.open_palette();
    CHECK_FALSE(f.browser.context_menu().is_open());
    CHECK(f.browser.command_palette().is_open());
}

TEST_CASE("palette in the browser: every command is offered where it can be used") {
    Fixture f;
    std::set<std::string> seen = f.ids();
    // A file under the cursor: what acts on it.
    f.press(key(XKB_KEY_Down));  // sub → one.txt (columns: a's entries)
    seen.merge(f.ids());
    // Two tabs, the grid, history behind.
    f.browser.new_tab();
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    seen.merge(f.ids());  // back
    f.browser.back();
    REQUIRE(f.settle());
    seen.merge(f.ids());
    // Something to paste, undo and redo.
    f.browser.open(f.a);
    REQUIRE(f.settle());
    f.press(key(XKB_KEY_Down));
    f.press(key(XKB_KEY_c, kCtrl));
    seen.merge(f.ids());

    // Pane commands require two different folders and a source selection.
    f.browser.set_mode(Browser::Mode::details);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    f.browser.details().folder()->select_name("sub");
    seen.merge(f.ids());
    f.browser.details().folder()->select_name("one.txt");
    seen.merge(f.ids());

    // Every command of the shortcuts but those the palette has no use for.
    const std::set<Command> not_in_palette{Command::none,       Command::context_menu,
                                           Command::select_tab, Command::command_palette,
                                           Command::undo,       Command::redo};
    const std::vector<std::pair<Command, const char*>> commands{
        {Command::columns_view, "columns_view"},
        {Command::grid_view, "grid_view"},
        {Command::details_view, "details_view"},
        {Command::toggle_info, "toggle_info"},
        {Command::toggle_sidebar, "toggle_sidebar"},
        {Command::focus_sidebar, "focus_sidebar"},
        {Command::quick_look, "quick_look"},
        {Command::show_shortcuts, "show_shortcuts"},
        {Command::properties, "properties"},
        {Command::rename, "rename"},
        {Command::new_folder, "new_folder"},
        {Command::copy, "copy"},
        {Command::cut, "cut"},
        {Command::paste, "paste"},
        {Command::trash, "trash"},
        {Command::erase, "erase"},
        {Command::back, "back"},
        {Command::forward, "forward"},
        {Command::toggle_hidden, "toggle_hidden"},
        {Command::add_bookmark, "add_bookmark"},
        {Command::new_tab, "new_tab"},
        {Command::close_tab, "close_tab"},
        {Command::next_tab, "next_tab"},
        {Command::previous_tab, "previous_tab"},
        {Command::find_files, "find_files"},
        {Command::filter_names, "filter_names"},
        {Command::go_to, "go_to"},
        {Command::toggle_split, "toggle_split"},
        {Command::switch_pane, "switch_pane"},
        {Command::swap_panes, "swap_panes"},
        {Command::orient_panes, "orient_panes"},
        {Command::open_other_pane, "open_other_pane"},
        {Command::same_other_pane, "same_other_pane"},
        {Command::copy_other_pane, "copy_other_pane"},
        {Command::move_other_pane, "move_other_pane"},
    };
    for (const rummage::views::Shortcut& shortcut : rummage::views::shortcuts()) {
        if (not_in_palette.contains(shortcut.command)) {
            continue;
        }
        const auto it =
            std::ranges::find(commands, shortcut.command, &std::pair<Command, const char*>::first);
        if (it == commands.end()) {
            // open_terminal: only with a terminal handler (none in the tests).
            CHECK(shortcut.command == Command::open_terminal);
            continue;
        }
        const std::string id = std::string{"command:"} + it->second;
        CAPTURE(id);
        CHECK(seen.contains(id));
    }
    for (const char* id :
         {"sort:size", "sort:descending", "view:zoom_in", "view:select_all", "view:parent",
          "go:", "command:empty_trash", "command:new_file", "command:open"}) {
        if (std::string_view{id} == "go:") {
            CHECK(seen.contains("go:" + f.b.string()));
        } else {
            CAPTURE(id);
            CHECK(seen.contains(id));
        }
    }
    // The other tab, by number; no Quit without a handler.
    CHECK(seen.contains("tab:0"));  // from the second tab, the first
    CHECK_FALSE(seen.contains("command:quit"));
    f.browser.set_quit_handler([] {});
    CHECK(f.ids().contains("command:quit"));
}

TEST_CASE("palette in the browser: an item does what its key does") {
    Fixture f;
    f.run("show grid");
    CHECK(f.browser.mode() == Browser::Mode::grid);
    f.run("hidden");
    CHECK(f.browser.shows_hidden());
    f.run("hidden");  // now "Hide Hidden Files"
    CHECK_FALSE(f.browser.shows_hidden());
    f.run("new tab");
    CHECK(f.browser.tab_count() == 2);
    CHECK(f.browser.active_tab() == 1);
    f.run("tab 1");
    CHECK(f.browser.active_tab() == 0);
    f.run("sort size");
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::size);
    f.run("go to bee");
    REQUIRE(f.browser.columns().column_count() > 0);
    CHECK(f.browser.state().palette_recent.front() == "go:" + f.b.string());

    // The keys' path: a view switch from the palette and from Ctrl+3 leave the same state.
    Fixture g;
    f.run("show details");
    g.press(key(XKB_KEY_3, kCtrl));
    CHECK(f.browser.mode() == g.browser.mode());
}

TEST_CASE("palette in the browser: the recently used come back first, and in state.toml") {
    Fixture f;
    f.run("show grid");
    f.run("new tab");
    f.run("hidden");
    const std::vector<std::string> recent = f.browser.palette_recent();
    REQUIRE(recent.size() == 3);
    CHECK(recent[0] == "command:toggle_hidden");
    CHECK(recent[1] == "command:new_tab");
    CHECK(recent[2] == "command:grid_view");

    f.browser.open_palette();
    // "Show Grid" is not offered in the grid; the other two come first, the latest first (the
    // hidden files' item under its other label).
    const auto& palette = f.browser.command_palette();
    REQUIRE(palette.shown().size() > 2);
    CHECK(palette.items()[palette.shown()[0]].id == "command:toggle_hidden");
    CHECK(palette.items()[palette.shown()[0]].label == "Hide Hidden Files");
    CHECK(palette.items()[palette.shown()[1]].id == "command:new_tab");
    f.browser.command_palette().close();

    // Through state.toml and back.
    const State state = f.browser.state();
    const rummage::views::StateResult read =
        rummage::views::parse_state(rummage::views::format_state(state), "state.toml");
    CHECK(read.problems.empty());
    CHECK(read.state.palette_recent == recent);
    Fixture g;
    g.browser.apply(read.state);
    CHECK(g.browser.palette_recent() == recent);
}

TEST_CASE("state: palette.recent keeps strings only, ten at most") {
    std::string list;
    for (int i = 0; i < 12; ++i) {
        list += std::format("\"id{}\", ", i);
    }
    const rummage::views::StateResult read =
        rummage::views::parse_state("[palette]\nrecent = [" + list + "3, \"\"]\n", "s.toml");
    CHECK(read.state.palette_recent.size() == State::kMaxRecent);
    CHECK(read.state.palette_recent.front() == "id0");
    const rummage::views::StateResult wrong =
        rummage::views::parse_state("[palette]\nrecent = \"id\"\n", "s.toml");
    CHECK(wrong.state.palette_recent.empty());
    CHECK(wrong.problems.size() == 1);
}
