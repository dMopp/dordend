#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/list_size.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::CursorShape;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::ui::PointerButton;
using rummage::ui::PointerEvent;
using rummage::views::Browser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

constexpr std::int32_t kWidth = 1000;
constexpr std::int32_t kHeight = 700;

// Construct before the fixture starts workers; restore after they have stopped.
struct ScopedHome {
    std::optional<std::string> previous;

    explicit ScopedHome(const char* value) {
        if (const char* home = std::getenv("HOME")) {
            previous = home;
        }
        if (value) {
            REQUIRE(setenv("HOME", value, 1) == 0);
        } else {
            REQUIRE(unsetenv("HOME") == 0);
        }
    }

    ~ScopedHome() {
        if (previous) {
            setenv("HOME", previous->c_str(), 1);
        } else {
            unsetenv("HOME");
        }
    }
};

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

constexpr Modifiers kCtrl{.ctrl = true};
constexpr Modifiers kCtrlShift{.ctrl = true, .shift = true};

std::unique_ptr<rummage::ui::Offscreen> offscreen(std::int32_t height = kHeight) {
    try {
        return std::make_unique<rummage::ui::Offscreen>(kWidth, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// a/{one.txt, sub/}, b/{two.txt}, in the columns at a.
struct Fixture {
    TempDir dir;
    fs::path a = dir.path / "a";
    fs::path b = dir.path / "b";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::size_t redraws{0};
    Browser browser{loop,
                    pool,
                    watcher,
                    rummage::fs::MimeDatabase::load(),
                    [this] { ++redraws; },
                    [](CursorShape /*shape*/) {}};

    Fixture() {
        fs::create_directories(a / "sub");
        fs::create_directories(b);
        std::ofstream{a / "one.txt"} << "1";
        std::ofstream{b / "two.txt"} << "2";
        browser.set_home_trash(dir.path / "Trash");
        browser.open(a);
        REQUIRE(settle());
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    bool press(const KeyEvent& event) {
        const bool handled = browser.key(event, Browser::Clock::now());
        REQUIRE(settle());
        return handled;
    }
};

}  // namespace

TEST_CASE("panes: tab sets, navigation, selection and filters stay independent") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    auto left = f.browser.details().folder();
    f.browser.open_tab(f.a / "sub", false);
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_F3)));
    REQUIRE(f.browser.split_visible());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_count() == 1);
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.set_sort(rummage::fs::SortKey::size, true);
    f.browser.set_show_hidden(true);
    f.browser.set_name_filter("two");
    f.browser.details().folder()->select_name("two.txt");
    auto right = f.browser.details().folder();
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK(f.browser.active_pane() == 0);
    CHECK(f.browser.tab_titles() == Names{"a", "sub"});
    CHECK(f.browser.sort_order().key == rummage::fs::SortKey::name);
    CHECK_FALSE(f.browser.shows_hidden());
    CHECK(f.browser.name_filter().empty());
    REQUIRE(left->cursor_entry());
    CHECK(left->cursor_entry()->name == "one.txt");
    CHECK(f.press(key(XKB_KEY_Tab, kCtrl)));
    CHECK(f.browser.details().folder()->path() == f.a / "sub");
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK(f.browser.tab_titles() == Names{"b"});
    CHECK(f.browser.name_filter() == "two");
    CHECK(f.browser.sort_order().descending);
    CHECK(f.browser.shows_hidden());
    REQUIRE(right->cursor_entry());
    CHECK(right->cursor_entry()->name == "two.txt");
    CHECK(f.press(key(XKB_KEY_w, kCtrl)));
    CHECK(f.browser.tab_count() == 1);
    CHECK(f.press(key(XKB_KEY_F3)));
    CHECK_FALSE(f.browser.split_visible());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.press(key(XKB_KEY_F3)));
    f.browser.focus_pane(0);
    CHECK(f.browser.active_tab() == 1);
    CHECK(f.browser.tab_count() == 2);
}

TEST_CASE("panes: transfers capture both ends, reverse direction, and use shared undo") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    f.browser.transfer_to_other_pane(false);
    // Change destination immediately; the queued copy still goes to b.
    f.browser.focus_pane(1);
    f.browser.open(f.a / "sub");
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK_FALSE(fs::exists(f.a / "sub" / "one.txt"));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.b / "one.txt"));
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.details().folder()->select_name("two.txt");
    f.browser.transfer_to_other_pane(true);
    REQUIRE(f.settle());
    CHECK(fs::exists(f.a / "two.txt"));
    CHECK_FALSE(fs::exists(f.b / "two.txt"));
    f.browser.undo();
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "two.txt"));
    CHECK_FALSE(fs::exists(f.a / "two.txt"));
    f.browser.details().folder()->select_name("two.txt");
    f.browser.toggle_split();
    f.browser.transfer_to_other_pane(false);
    REQUIRE(f.settle());
    CHECK_FALSE(fs::exists(f.a / "two.txt"));
}

TEST_CASE("panes: captured menu transfers cannot follow a later selection or destination") {
    Fixture f;
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    auto items = f.browser.palette_items();
    auto found =
        std::ranges::find(items, "command:copy_other_pane", &rummage::views::PaletteItem::id);
    REQUIRE(found != items.end());
    auto action = found->action;
    CHECK(found->label.find(f.b.string()) != std::string::npos);
    f.browser.details().folder()->select_name("sub");
    f.browser.focus_pane(1);
    f.browser.open(f.a / "sub");
    action();
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK_FALSE(fs::exists(f.b / "sub"));
    CHECK_FALSE(fs::exists(f.a / "sub" / "one.txt"));
}

TEST_CASE("panes: session round trip retains both tab sets even while one is hidden") {
    Fixture f;
    f.browser.open_tab(f.a / "sub", true);
    REQUIRE(f.settle());
    f.browser.toggle_split();
    f.browser.open(f.b);
    f.browser.set_mode(Browser::Mode::grid);
    f.browser.set_sort(rummage::fs::SortKey::modified, true);
    f.browser.set_show_hidden(true);
    f.browser.set_name_filter("two");
    f.browser.set_stacked(true);
    f.browser.set_split_ratio(0.65F);
    f.browser.toggle_split();
    REQUIRE(f.settle());
    const auto saved = f.browser.state();
    const auto parsed = rummage::views::parse_state(rummage::views::format_state(saved), "test");
    REQUIRE(parsed.problems.empty());
    CHECK(parsed.state == saved);
    Browser restored{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    restored.apply(parsed.state);
    REQUIRE(restored.restore_session(parsed.state));
    REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
    CHECK_FALSE(restored.split_visible());
    CHECK(restored.active_pane() == 1);
    CHECK(restored.tab_titles() == Names{"b"});
    CHECK(restored.mode() == Browser::Mode::grid);
    CHECK(restored.name_filter() == "two");
    CHECK(restored.shows_hidden());
    CHECK(restored.sort_order().key == rummage::fs::SortKey::modified);
    CHECK(restored.sort_order().descending);
    CHECK(restored.stacked());
    CHECK(restored.split_ratio() == doctest::Approx(0.65F));
    restored.toggle_split();
    restored.focus_pane(0);
    CHECK(restored.tab_titles() == Names{"a", "sub"});
    CHECK(restored.active_tab() == 1);
}

TEST_CASE("panes: opening elsewhere preserves destination tabs; swapping follows content") {
    Fixture f;
    f.browser.toggle_split();
    f.browser.open(f.b);
    f.browser.focus_pane(0);
    f.browser.open_in_other_pane(f.a / "sub");
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_titles() == Names{"b", "sub"});
    f.browser.swap_panes();
    CHECK(f.browser.active_pane() == 0);
    CHECK(f.browser.tab_titles() == Names{"b", "sub"});
    f.browser.focus_pane(1);
    CHECK(f.browser.tab_titles() == Names{"a"});
}

TEST_CASE("panes: editor and filter Tab never switch panes; picker cannot split") {
    Fixture f;
    f.browser.toggle_split();
    REQUIRE(f.settle());
    CHECK(f.press(key(XKB_KEY_f, kCtrlShift)));
    CHECK(f.browser.filtering());
    CHECK(f.press(key(XKB_KEY_Tab)));
    CHECK_FALSE(f.browser.filtering());
    CHECK(f.browser.active_pane() == 1);
    f.browser.columns().focused_folder()->select_name("one.txt");
    CHECK(f.press(key(XKB_KEY_F2)));
    REQUIRE(f.browser.name_editor().active());
    f.browser.key(key(XKB_KEY_Tab), Browser::Clock::now());
    CHECK(f.browser.active_pane() == 1);
    f.browser.name_editor().cancel();
    Browser picker{
        f.loop, f.pool, f.watcher, rummage::fs::MimeDatabase::load(), [] {}, [](CursorShape) {}};
    picker.set_panes_enabled(false);
    picker.open(f.a);
    picker.toggle_split();
    CHECK_FALSE(picker.split_visible());
    picker.open_in_other_pane(f.b);
    CHECK(picker.tab_count() == 1);
}

TEST_CASE("panes: pointer focus, inactive scrolling, both tab bars, resizing and drop targets") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.set_mode(Browser::Mode::details);
    f.browser.details().folder()->select_name("one.txt");
    auto left = f.browser.details().folder();
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1.0F, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons,
                       Browser::Clock::now());
        renderer.end_frame();
    };
    const auto click = [&](float x, float y, int count = 1) {
        f.browser.pointer({.kind = PointerEvent::Kind::press,
                           .x = x,
                           .y = y,
                           .button = PointerButton::left,
                           .click_count = count},
                          Browser::Clock::now());
        f.browser.pointer(
            {.kind = PointerEvent::Kind::release, .x = x, .y = y, .button = PointerButton::left},
            Browser::Clock::now());
    };
    draw();
    const auto l = f.browser.pane_bounds(0);
    const auto r = f.browser.pane_bounds(1);
    CHECK(l.right() < r.x);
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = l.x + 20, .y = l.y + 220},
                      Browser::Clock::now());
    f.browser.pointer(
        {.kind = PointerEvent::Kind::scroll, .x = l.x + 20, .y = l.y + 220, .wheel_y = 1},
        Browser::Clock::now());
    CHECK(f.browser.active_pane() == 1);
    click(l.x + 20, l.y + 10);
    CHECK(f.browser.active_pane() == 0);
    REQUIRE(left->cursor_entry());
    CHECK(left->cursor_entry()->name == "one.txt");
    // Use the inactive tab bar's plus button: it focuses that pane and adds only there.
    f.browser.focus_pane(1);
    const auto add = f.browser.tab_bar().add_rect();
    f.browser.focus_pane(0);
    click(add.x + 3, add.y + 3);
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.tab_count() == 2);
    f.browser.focus_pane(0);
    CHECK(f.browser.tab_count() == 1);
    draw();
    auto divider = f.browser.divider_bounds();
    CHECK(f.browser.cursor_at(divider.x + 2, divider.y + 10) == CursorShape::ew_resize);
    f.browser.pointer({.kind = PointerEvent::Kind::press,
                       .x = divider.x + 2,
                       .y = divider.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = 650, .y = divider.y + 10},
                      Browser::Clock::now());
    f.browser.pointer({.kind = PointerEvent::Kind::release,
                       .x = 650,
                       .y = divider.y + 10,
                       .button = PointerButton::left},
                      Browser::Clock::now());
    CHECK(f.browser.split_ratio() > 0.6F);
    draw();
    divider = f.browser.divider_bounds();
    click(divider.x + 2, divider.y + 10, 2);
    CHECK(f.browser.split_ratio() == 0.5F);
    draw();
    f.browser.set_drag_and_drop(
        {.start = {},
         .cancel = {},
         .types = [] { return std::vector<std::string>{std::string{rummage::views::kUriList}}; },
         .source_actions = [] { return rummage::ui::kDndCopy; },
         .action = [] { return rummage::ui::kDndCopy; },
         .accept = {},
         .read =
             [&](std::string, rummage::views::DragAndDrop::Done done) {
                 const std::vector<fs::path> paths{f.a / "one.txt"};
                 done(rummage::views::uri_list(paths));
             },
         .finish = {},
         .modifiers = {}});
    f.browser.drop_event(
        {.kind = rummage::ui::DropEvent::Kind::drop, .x = r.x + 40, .y = r.bottom() - 50});
    REQUIRE(f.settle());
    CHECK(fs::exists(f.b / "one.txt"));
    CHECK(f.browser.active_pane() == 0);
    f.browser.set_stacked(true);
    draw();
    CHECK(f.browser.pane_bounds(0).bottom() < f.browser.pane_bounds(1).y);
    divider = f.browser.divider_bounds();
    CHECK(f.browser.cursor_at(divider.x + 10, divider.y + 2) == CursorShape::ns_resize);
}

TEST_CASE("panes: a delayed mount opens in the originating tab without stealing focus") {
    Fixture f;
    rummage::views::Devices::Mounted complete;
    f.browser.set_devices(
        {.mount = [&](const std::string&, auto done) { complete = std::move(done); },
         .unmount = {},
         .eject = {}});
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    f.browser.focus_pane(0);
    const rummage::fs::Place disk{.section = rummage::fs::Place::Section::devices,
                                  .label = "Disk",
                                  .path = {},
                                  .icon = "drive",
                                  .disk = {.volume = "test-volume", .mounted = false}};
    f.browser.mount_place(disk, true);
    REQUIRE(complete);
    f.browser.focus_pane(1);
    complete(f.a / "sub", {});
    REQUIRE(f.settle());
    CHECK(f.browser.active_pane() == 1);
    CHECK(f.browser.columns().focused_folder()->path() == f.b);
    f.browser.focus_pane(0);
    CHECK(f.browser.columns().focused_folder()->path() == f.a / "sub");
}

TEST_CASE("panes: missing saved folders use a valid fallback and retain the other group") {
    TempDir home_dir;
    std::optional<std::string> home = home_dir.path.string();
    fs::path expected = home_dir.path;
    SUBCASE("existing home") {}
    SUBCASE("missing home") {
        home = (home_dir.path / "missing").string();
        expected = "/";
    }
    SUBCASE("home is a file") {
        home = home_dir.write("file", "not a directory").string();
        expected = "/";
    }
    SUBCASE("relative home") {
        home = ".";
        expected = "/";
    }
    SUBCASE("empty home") {
        home = "";
        expected = "/";
    }
    SUBCASE("unset home") {
        home.reset();
        expected = "/";
    }
    const ScopedHome environment{home ? home->c_str() : nullptr};
    for (const std::size_t missing : {std::size_t{0}, std::size_t{1}}) {
        INFO("missing pane: " << missing);
        Fixture f;
        f.browser.toggle_split();
        f.browser.open(f.b);
        REQUIRE(f.settle());
        auto saved = f.browser.state();
        for (auto& tab : saved.tabs) {
            if (tab.pane == missing) {
                tab.folder = f.dir.path / "missing";
            }
        }
        saved.active_pane = 1 - missing;
        Browser restored{f.loop,    f.pool,
                         f.watcher, rummage::fs::MimeDatabase::load(),
                         [] {},     [](CursorShape) {}};
        restored.apply(saved);
        REQUIRE(restored.restore_session(saved));
        REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
        CHECK(restored.split_visible());
        CHECK(restored.active_pane() == 1 - missing);
        CHECK(restored.tab_count() == 1);
        REQUIRE(restored.columns().focused_folder());
        CHECK(restored.columns().focused_folder()->path() == (missing == 0 ? f.b : f.a));
        restored.focus_pane(missing);
        REQUIRE(run_until(f.loop, [&] { return !restored.loading(); }));
        CHECK(restored.tab_count() == 1);
        REQUIRE(restored.columns().focused_folder());
        CHECK(restored.columns().focused_folder()->path() == expected);
        CHECK_FALSE(restored.columns().focused_folder()->model().error());
    }
}

TEST_CASE("panes: an idle split does not schedule another frame while drawing") {
    Fixture f;
    const auto gl = offscreen();
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_info_visible(false);
    f.browser.set_mode(Browser::Mode::details);
    f.browser.toggle_split();
    f.browser.open(f.b);
    REQUIRE(f.settle());
    const auto now = Browser::Clock::now() + 2s;
    const auto draw = [&] {
        renderer.begin_frame(kWidth, kHeight, 1, {});
        f.browser.draw(renderer, {0, 0, kWidth, kHeight}, rummage::theme::Palette{}, icons, now);
        renderer.end_frame();
    };
    draw();
    REQUIRE(f.settle());
    draw();
    const auto redraws = f.redraws;
    draw();
    CHECK(f.redraws == redraws);
    CHECK_FALSE(f.browser.animating());
}

TEST_CASE("panes: split toolbars and status bar respect configured font metrics") {
    Fixture f;
    const auto gl = offscreen(kHeight * 2);
    if (!gl)
        return;
    rummage::ui::FontSystem fonts;
    rummage::ui::Renderer renderer{fonts};
    rummage::ui::IconSet icons{fonts};
    f.browser.set_sidebar_visible(false);
    f.browser.set_info_visible(false);
    f.browser.toggle_split();
    REQUIRE(f.settle());
    auto& status = f.browser.status_bar();
    status.set_text_size(32);
    for (const bool stacked : {false, true}) {
        f.browser.set_stacked(stacked);
        for (const float scale : {1.0F, 1.5F, 2.0F}) {
            for (const std::size_t index : {0U, 1U}) {
                f.browser.focus_pane(index);
                f.browser.set_name_filter("one");
                renderer.begin_frame(kWidth, kHeight * 2, scale, {});
                const float bottom = kHeight * 2 / scale;
                f.browser.draw(renderer, {0, 0, 600, bottom}, rummage::theme::Palette{}, icons,
                               Browser::Clock::now());
                renderer.end_frame();
                const auto pane = f.browser.pane_bounds(index);
                const auto field = f.browser.filter_bounds();
                const auto button = f.browser.new_folder_bounds();
                CHECK(field.x >= pane.x + 10 + fonts.layout("Filter", 13, scale).width / scale + 9);
                CHECK(field.width >= 48);
                CHECK(field.right() <= pane.right() - 10);
                if (!button.empty()) {
                    CHECK(field.right() + 8 <= button.x);
                    CHECK(button.right() <= pane.right() - 10);
                }
                const float height = status.height(renderer);
                CHECK(status.claims(20, bottom - height + 1));
                CHECK_FALSE(status.claims(20, bottom - height - 1));
            }
        }
    }
}

TEST_CASE("panes: archive dialogs capture keys and password paste without switching panes") {
    Fixture f;
    f.browser.toggle_split();
    REQUIRE(f.settle());
    const auto active = f.browser.active_pane();
    rummage::views::SystemClipboard clipboard;
    clipboard.types = [] { return std::vector<std::string>{"text/plain"}; };
    clipboard.read = [](std::string, rummage::views::SystemClipboard::Done done) {
        done("archive secret");
    };
    f.browser.set_system_clipboard(std::move(clipboard));
    std::optional<std::string> answer;
    f.browser.password_dialog().open_password("secret.7z", false,
                                              [&](auto value) { answer = std::move(value); });
    CHECK(f.browser.has_overlay());
    f.press(key(XKB_KEY_v, kCtrl));
    CHECK(f.browser.name_filter().empty());
    f.press(key(XKB_KEY_Return));
    CHECK(answer == "archive secret");
    CHECK(f.browser.active_pane() == active);
    f.browser.show_action_result("Archive failed\nInstall unrar", true);
    f.press(key(XKB_KEY_Tab));
    CHECK(f.browser.active_pane() == active);
    f.press(key(XKB_KEY_Escape));
    CHECK_FALSE(f.browser.has_overlay());
    f.press(key(XKB_KEY_Tab));
    CHECK(f.browser.active_pane() != active);
}
