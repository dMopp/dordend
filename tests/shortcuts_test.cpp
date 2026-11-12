#include "nordend/views/shortcuts.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/core/worker_pool.hpp"
#include "nordend/fs/directory_watcher.hpp"
#include "nordend/fs/mime.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/shortcuts_overlay.hpp"
#include "nordend/views/status_bar.hpp"
#include "temp_dir.hpp"

using nordend::core::EventLoop;
using nordend::test::TempDir;
using nordend::ui::KeyEvent;
using nordend::ui::Modifiers;
using nordend::views::Browser;
using nordend::views::Command;
using nordend::views::Folder;
using nordend::views::Shortcut;
using nordend::views::shortcuts;
namespace scope = nordend::views::scope;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

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

bool cursor_name_is(const Folder& folder, std::string_view name) {
    const nordend::fs::Entry* entry = folder.cursor_entry();
    return entry != nullptr && entry->name == name;
}

KeyEvent chord(xkb_keysym_t keysym, Modifiers modifiers) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

std::string describe(const KeyEvent& event) {
    std::string out;
    out += event.modifiers.ctrl ? "Ctrl+" : "";
    out += event.modifiers.shift ? "Shift+" : "";
    out += event.modifiers.alt ? "Alt+" : "";
    std::array<char, 64> name{};
    xkb_keysym_get_name(event.keysym, name.data(), name.size());
    return out + name.data();
}

// root/{sub/{inner.txt}, a.txt, b.txt}
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    EventLoop loop;
    nordend::core::WorkerPool pool{loop, 2};
    nordend::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, nordend::fs::MimeDatabase::load(),
                    [] {},   [](nordend::ui::CursorShape /*shape*/) {}};

    Fixture() {
        make_files();
        browser.set_home_trash(dir.path / "Trash");  // Delete trashes them here, never in the home
        browser.set_places({{.label = "root", .path = root, .icon = "folder"},
                            {.label = "sub", .path = root / "sub", .icon = "folder"}});
    }

    // The files again, after Delete took some.
    void make_files() const {
        fs::create_directories(root / "sub");
        for (const char* name : {"sub/inner.txt", "a.txt", "b.txt"}) {
            if (!fs::exists(root / name)) {
                std::ofstream{root / name} << name;
            }
        }
    }

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

    // The root in mode, loaded, the cursor on cursor_on, nothing over the view.
    void reset(Browser::Mode mode, const std::string& cursor_on = "sub") {
        // A new folder, say; an undone Delete may ask about a name made again meanwhile.
        REQUIRE(run_until(loop, [&] {
            if (browser.conflict_dialog().is_open()) {
                browser.cancel_transfer();
            }
            return !browser.loading();
        }));
        browser.blur_filter();
        browser.set_name_filter({});
        browser.name_editor().cancel();
        REQUIRE_FALSE(browser.name_editor().active());
        browser.quick_look().close();
        browser.shortcuts_overlay().close();
        browser.context_menu().close();
        browser.command_palette().close();
        browser.confirm_dialog().close();
        browser.properties_dialog().close();
        browser.location_dialog().close();
        browser.sidebar().set_focused(false);
        make_files();
        browser.open(root);
        browser.set_mode(mode);
        REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
        folder()->select_name(cursor_on);
        REQUIRE(run_until(loop, [&] { return folder()->cursor_entry() != nullptr; }));
        REQUIRE(run_until(loop, [&] { return !browser.loading(); }));
    }
};

constexpr std::array<Browser::Mode, 3> kModes{Browser::Mode::columns, Browser::Mode::grid,
                                              Browser::Mode::details};

std::uint16_t scope_of(Browser::Mode mode) {
    switch (mode) {
        case Browser::Mode::grid:
            return scope::kGrid;
        case Browser::Mode::details:
            return scope::kDetails;
        case Browser::Mode::columns:
            break;
    }
    return scope::kColumns;
}

// Every chord a shortcut stands for.
std::vector<KeyEvent> chords_of(const Shortcut& shortcut) {
    std::vector<KeyEvent> out;
    for (const xkb_keysym_t keysym : shortcut.keysyms) {
        if (keysym != XKB_KEY_NoSymbol) {
            out.push_back(chord(keysym, shortcut.modifiers));
        }
    }
    return out;
}

}  // namespace

TEST_CASE("shortcuts: the groups are the overlay's") {
    const std::vector<std::string_view> groups = nordend::views::overlay_groups();
    const std::set<std::string_view> expected{groups.begin(), groups.end()};
    for (const Shortcut& shortcut : shortcuts()) {
        CHECK_MESSAGE(expected.contains(shortcut.group), shortcut.group);
        CHECK(shortcut.scope != 0);
        CHECK(!chords_of(shortcut).empty());
    }
}

TEST_CASE("shortcuts: every one listed for a view works there") {
    Fixture f;
    for (const Browser::Mode mode : kModes) {
        for (const Shortcut& shortcut : shortcuts()) {
            if ((shortcut.scope & scope_of(mode)) == 0) {
                continue;
            }
            for (const KeyEvent& event : chords_of(shortcut)) {
                f.reset(mode);
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()),
                              describe(event) << " in mode " << static_cast<int>(mode));
            }
        }
    }
}

TEST_CASE("shortcuts: every one listed for the sidebar and Quick Look works there") {
    Fixture f;
    for (const Shortcut& shortcut : shortcuts()) {
        for (const KeyEvent& event : chords_of(shortcut)) {
            if ((shortcut.scope & scope::kSidebar) != 0) {
                f.reset(Browser::Mode::grid);
                f.browser.sidebar().set_focused(true);
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()),
                              describe(event) << " in the sidebar");
            }
            if ((shortcut.scope & scope::kQuickLook) != 0) {
                f.reset(Browser::Mode::grid, "a.txt");
                f.browser.key(chord(XKB_KEY_space, {}), Browser::Clock::now());
                REQUIRE(f.browser.quick_look().is_open());
                CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()), describe(event)
                                                                               << " in Quick Look");
            }
        }
    }
}

TEST_CASE("shortcuts: every one listed for renaming works in the name's field") {
    Fixture f;
    for (const Shortcut& shortcut : shortcuts()) {
        if ((shortcut.scope & scope::kEditor) == 0) {
            continue;
        }
        for (const KeyEvent& event : chords_of(shortcut)) {
            f.reset(Browser::Mode::details, "a.txt");
            f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now());
            REQUIRE(f.browser.name_editor().active());
            CHECK_MESSAGE(f.browser.key(event, Browser::Clock::now()), describe(event));
        }
    }
}

TEST_CASE("shortcuts: nothing else does anything") {
    // The keys a file manager could plausibly react to, with each modifier.
    std::vector<xkb_keysym_t> keys{
        XKB_KEY_Up,        XKB_KEY_Down,   XKB_KEY_Left,       XKB_KEY_Right,  XKB_KEY_Page_Up,
        XKB_KEY_Page_Down, XKB_KEY_Home,   XKB_KEY_End,        XKB_KEY_Return, XKB_KEY_KP_Enter,
        XKB_KEY_BackSpace, XKB_KEY_Escape, XKB_KEY_space,      XKB_KEY_Tab,    XKB_KEY_Delete,
        XKB_KEY_Insert,    XKB_KEY_plus,   XKB_KEY_minus,      XKB_KEY_equal,  XKB_KEY_question,
        XKB_KEY_slash,     XKB_KEY_KP_Add, XKB_KEY_KP_Subtract};
    for (xkb_keysym_t key = XKB_KEY_a; key <= XKB_KEY_z; ++key) {
        keys.push_back(key);
    }
    for (xkb_keysym_t key = XKB_KEY_0; key <= XKB_KEY_9; ++key) {
        keys.push_back(key);
    }
    for (xkb_keysym_t key = XKB_KEY_F1; key <= XKB_KEY_F12; ++key) {
        keys.push_back(key);
    }
    const std::array<Modifiers, 5> modifiers{Modifiers{}, Modifiers{.ctrl = true},
                                             Modifiers{.shift = true}, Modifiers{.alt = true},
                                             Modifiers{.ctrl = true, .shift = true}};
    Fixture f;
    for (const Browser::Mode mode : kModes) {
        f.reset(mode);
        for (const xkb_keysym_t key : keys) {
            for (const Modifiers& mods : modifiers) {
                const KeyEvent event = chord(key, mods);
                if (!f.browser.key(event, Browser::Clock::now())) {
                    continue;
                }
                CHECK_MESSAGE((nordend::views::listed(event, scope_of(mode)) ||
                               nordend::views::command_for(event) != Command::none),
                              describe(event) << " in mode " << static_cast<int>(mode));
                f.reset(mode);
            }
        }
    }
    // Ctrl+Q is the window's: the browser leaves it alone.
    f.reset(Browser::Mode::grid);
    CHECK_FALSE(f.browser.key(chord(XKB_KEY_q, {.ctrl = true}), Browser::Clock::now()));
}

TEST_CASE("shortcuts: the overlay opens and closes, and takes every key meanwhile") {
    Fixture f;
    f.reset(Browser::Mode::details);
    const auto now = Browser::Clock::now();
    f.browser.key(chord(XKB_KEY_F1, {}), now);
    CHECK(f.browser.shortcuts_overlay().is_open());
    f.browser.shortcuts_overlay().layout({0, 0, 640, 480});
    CHECK(f.browser.key(chord(XKB_KEY_Right, {}), now));
    CHECK(f.browser.key(chord(XKB_KEY_Down, {}), now));
    CHECK(f.browser.shortcuts_overlay().scroll_offset() > 0.0F);
    CHECK(cursor_name_is(*f.folder(), "sub"));
    f.browser.key(chord(XKB_KEY_Escape, {}), now);
    CHECK_FALSE(f.browser.shortcuts_overlay().is_open());
    // "?" comes with Shift on most layouts.
    f.browser.key(chord(XKB_KEY_question, {.shift = true}), now);
    CHECK(f.browser.shortcuts_overlay().is_open());
    f.browser.key(chord(XKB_KEY_question, {.shift = true}), now);
    CHECK_FALSE(f.browser.shortcuts_overlay().is_open());
}

TEST_CASE("status bar: the status and the free space") {
    Fixture f;
    f.reset(Browser::Mode::details);
    std::shared_ptr<Folder> folder = f.folder();
    CHECK(nordend::views::status_of(*folder) == "3 items");
    folder->select_all();
    CHECK(nordend::views::status_of(*folder) == "3 of 3 selected");

    nordend::views::StatusBar& bar = f.browser.status_bar();
    bar.update(*folder);
    REQUIRE(run_until(f.loop, [&] { return !bar.loading(); }));
    REQUIRE(bar.free_space().has_value());
    CHECK(bar.free_space().value_or(nordend::views::FreeSpace{}).total > 0);
    bar.update(*folder);
    CHECK_FALSE(bar.loading());  // known for this listing

    // A file arrives: the listing changes, and the free space is read again.
    std::ofstream{f.root / "c.txt"} << "c";
    REQUIRE(run_until(f.loop, [&] { return folder->entries().size() == 4; }));
    bar.update(*folder);
    CHECK(bar.loading());
    CHECK(run_until(f.loop, [&] { return !bar.loading(); }));
}

namespace {
struct ResetBindings {
    ~ResetBindings() { (void)nordend::views::configure_shortcuts({}); }
};
}  // namespace

TEST_CASE("shortcuts: overrides replace defaults and reject conflicts atomically") {
    ResetBindings reset;
    using nordend::views::command_for;
    using nordend::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"rename", "Ctrl+r"}, {"trash", ""}}).empty());
    CHECK(command_for(chord(XKB_KEY_r, {.ctrl = true})) == Command::rename);
    CHECK(command_for(chord(XKB_KEY_R, {.ctrl = true})) == Command::rename);
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::none);
    CHECK(command_for(chord(XKB_KEY_Delete, {})) == Command::none);
    CHECK(nordend::views::keys_for(Command::rename) == "Ctrl+r");
    for (const auto& binding :
         std::vector<std::pair<std::string, std::string>>{{"rename", "Ctrl+c"},
                                                          {"rename", "Ctrl+q"},
                                                          {"rename", "Up"},
                                                          {"rename", "Ctrl+NoSuchKey"},
                                                          {"rename", "Ctrl+Ctrl+r"},
                                                          {"unknown", "F3"}}) {
        CHECK_FALSE(configure_shortcuts({binding}).empty());
        CHECK(command_for(chord(XKB_KEY_r, {.ctrl = true})) == Command::rename);
    }
    REQUIRE(configure_shortcuts({{"rename", "Ctrl+Shift+n"}, {"new_folder", "F2"}}).empty());
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::new_folder);
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(command_for(chord(XKB_KEY_F2, {})) == Command::rename);
}

TEST_CASE("shortcuts: custom selection and rename work in each view") {
    ResetBindings reset;
    REQUIRE(nordend::views::configure_shortcuts(
                {{"select_all", "Ctrl+g"}, {"toggle_selection", "Ctrl+j"}, {"rename", "Ctrl+r"}})
                .empty());
    Fixture f;
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        CHECK_FALSE(f.browser.key(chord(XKB_KEY_a, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.browser.key(chord(XKB_KEY_g, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.folder()->selected_count() == 3);
        CHECK(f.browser.key(chord(XKB_KEY_j, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.folder()->selected_count() == 2);
        CHECK(f.browser.key(chord(XKB_KEY_r, {.ctrl = true}), Browser::Clock::now()));
        CHECK(f.browser.name_editor().active());
        CHECK(f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now()));
        CHECK_FALSE(f.browser.name_editor().active());
    }
}

TEST_CASE("shortcuts: tab aliases and combined help rows follow overrides") {
    ResetBindings reset;
    using nordend::views::command_for;
    using nordend::views::configure_shortcuts;
    REQUIRE(configure_shortcuts({{"previous_tab", "Ctrl+Shift+Tab"}, {"back", "Alt+b"}}).empty());
    CHECK(command_for(chord(XKB_KEY_ISO_Left_Tab, {.ctrl = true, .shift = true})) ==
          Command::previous_tab);
    CHECK(nordend::views::keys_for(Command::back) == "Alt+b");
    CHECK(nordend::views::keys_for(Command::forward) == "Alt+Right");
    CHECK(nordend::views::keys_for(Command::next_tab) == "Ctrl+Tab");
}

TEST_CASE("shortcuts: overflow is reachable by wheel, keyboard and scrollbar") {
    nordend::views::ShortcutsOverlay overlay{[] {}};
    overlay.open();
    overlay.layout({0, 0, 640, 480});
    overlay.key(chord(XKB_KEY_Right, {}));
    REQUIRE(overlay.max_scroll() > 0.0F);
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::scroll, .wheel_y = 1.0F});
    CHECK(overlay.scroll_offset() == 48.0F);
    overlay.key(chord(XKB_KEY_End, {}));
    CHECK(overlay.scroll_offset() == overlay.max_scroll());
    overlay.key(chord(XKB_KEY_Page_Up, {}));
    CHECK(overlay.scroll_offset() < overlay.max_scroll());
    overlay.key(chord(XKB_KEY_Home, {}));
    CHECK(overlay.scroll_offset() == 0.0F);
    // Content clicks leave the reference open, while the scrollbar can be dragged to the end.
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::press,
                     .x = 100,
                     .y = 180,
                     .button = nordend::ui::PointerButton::left});
    CHECK(overlay.is_open());
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::press,
                     .x = 576,
                     .y = 128,
                     .button = nordend::ui::PointerButton::left});
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::motion, .x = 576, .y = 440});
    CHECK(overlay.scroll_offset() == overlay.max_scroll());
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::release});
    overlay.layout({0, 0, 1600, 4000});
    overlay.key(chord(XKB_KEY_Left, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    CHECK(overlay.scroll_offset() == 0.0F);
    overlay.layout({0, 0, 640, 480});
    overlay.key(chord(XKB_KEY_End, {}));
    overlay.close();
    overlay.open();
    CHECK(overlay.scroll_offset() == 0.0F);
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::press,
                     .x = 0,
                     .y = 0,
                     .button = nordend::ui::PointerButton::left});
    CHECK_FALSE(overlay.is_open());
}

TEST_CASE("shortcuts: groups start collapsed and toggle by mouse and keyboard") {
    nordend::views::ShortcutsOverlay overlay{[] {}};
    overlay.open();
    overlay.layout({0, 0, 1000, 800});
    CHECK(overlay.max_scroll() == 0.0F);
    // General is the first header; clicking either its title or row toggles it.
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::press,
                     .x = 100,
                     .y = 140,
                     .button = nordend::ui::PointerButton::left});
    REQUIRE(overlay.max_scroll() > 0.0F);
    overlay.pointer({.kind = nordend::ui::PointerEvent::Kind::press,
                     .x = 500,
                     .y = 140,
                     .button = nordend::ui::PointerButton::left});
    CHECK(overlay.max_scroll() == 0.0F);
    overlay.key(chord(XKB_KEY_Tab, {}));
    overlay.key(chord(XKB_KEY_Return, {}));
    CHECK(overlay.max_scroll() > 0.0F);
    overlay.key(chord(XKB_KEY_space, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    // Shift+Tab wraps to Quick Look; tabbing forward returns to General.
    overlay.key(chord(XKB_KEY_ISO_Left_Tab, {.shift = true}));
    overlay.key(chord(XKB_KEY_Right, {}));
    overlay.key(chord(XKB_KEY_Tab, {}));
    overlay.key(chord(XKB_KEY_Right, {}));
    CHECK(overlay.max_scroll() > 0.0F);
    overlay.key(chord(XKB_KEY_Left, {}));
    CHECK(overlay.max_scroll() == 0.0F);
    CHECK(overlay.is_open());
}

TEST_CASE("actions: shortcuts validate registry IDs and preserve bindings after errors") {
    ResetBindings reset;
    using nordend::views::action_for;
    using nordend::views::configure_shortcuts;
    const std::vector<nordend::core::Action> actions{
        {.id = "inspect", .label = "Inspect", .argv = {"inspector", "{file}"}, .mime = {}}};
    REQUIRE(configure_shortcuts(
                {{"action:inspect", "Ctrl+Alt+i"}, {"copy_location", "Ctrl+Shift+c"}}, actions)
                .empty());
    CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})) == "inspect");
    CHECK(action_for(chord(XKB_KEY_C, {.ctrl = true, .shift = true})) == "copy_location");
    CHECK(nordend::views::keys_for_action("inspect") == "Ctrl+Alt+i");
    for (const auto& binding :
         std::vector<std::pair<std::string, std::string>>{{"action:missing", "F3"},
                                                          {"action:inspect", "Ctrl+c"},
                                                          {"action:inspect", "Up"},
                                                          {"action:inspect", "Ctrl+q"}}) {
        CHECK_FALSE(configure_shortcuts({binding}, actions).empty());
        CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})) == "inspect");
    }
    CHECK_FALSE(
        configure_shortcuts({{"copy_location", "F3"}, {"action:copy_location", "F5"}}, actions)
            .empty());
    REQUIRE(configure_shortcuts({}).empty());
    CHECK(action_for(chord(XKB_KEY_i, {.ctrl = true, .alt = true})).empty());
}

TEST_CASE("actions: external shortcuts use the same targets in all views and respect editors") {
    ResetBindings reset;
    const std::vector<nordend::core::Action> actions{
        {.id = "inspect", .label = "Inspect", .argv = {"inspector", "{file}"}, .mime = {}}};
    REQUIRE(
        nordend::views::configure_shortcuts({{"action:inspect", "Ctrl+Alt+i"}}, actions).empty());
    Fixture f;
    f.browser.set_actions(actions);
    std::vector<std::string> calls;
    f.browser.set_action_handoff(
        [&](const auto&, const auto& context) {
            REQUIRE(context.targets.size() == 1);
            calls.push_back(context.targets.front().path.string());
        },
        [](const auto&, const auto&) { return true; });
    for (const auto mode : kModes) {
        f.reset(mode, "a.txt");
        const auto event = chord(XKB_KEY_i, {.ctrl = true, .alt = true});
        CHECK(f.browser.key(event, Browser::Clock::now()));
        REQUIRE_FALSE(calls.empty());
        CHECK(calls.back() == (f.root / "a.txt").string());
        const auto count = calls.size();
        f.browser.key(chord(XKB_KEY_F2, {}), Browser::Clock::now());
        REQUIRE(f.browser.name_editor().active());
        f.browser.key(event, Browser::Clock::now());
        CHECK(calls.size() == count);
        f.browser.key(chord(XKB_KEY_Escape, {}), Browser::Clock::now());
    }
}
