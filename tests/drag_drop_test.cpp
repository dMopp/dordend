#include <GLES3/gl3.h>
#include <algorithm>
#include <chrono>
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
#include "rummage/core/file.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/clipboard.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_clipboard.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::test::TempDir;
using rummage::ui::DropEvent;
using rummage::ui::KeyEvent;
using rummage::ui::Modifiers;
using rummage::ui::PointerButton;
using rummage::ui::PointerEvent;
using rummage::views::Browser;
using rummage::views::ViewHit;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

constexpr std::int32_t kWidth = 1000;
constexpr std::int32_t kHeight = 700;

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

Names listing(const fs::path& directory) {
    Names names;
    std::error_code ignored;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

KeyEvent key(xkb_keysym_t keysym, Modifiers modifiers = {}) {
    KeyEvent event;
    event.keysym = keysym;
    event.modifiers = modifiers;
    return event;
}

// The GL context the drawing needs; nullopt where the platform has none (the tests then stop).
std::unique_ptr<rummage::ui::Offscreen> offscreen() {
    try {
        return std::make_unique<rummage::ui::Offscreen>(kWidth, kHeight);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

// What the app's Wayland side would do, recorded.
struct FakeDnd {
    std::vector<rummage::views::DragAndDrop::Format> started;
    int cancelled{0};
    int finished{0};
    std::vector<std::string> types;
    std::uint32_t source_actions{0};
    std::uint32_t action{0};
    std::optional<std::string> accepted;  // the type taken where the pointer is
    std::uint32_t accepted_actions{0};
    std::string data;  // what a read of another program's drop gives
    Modifiers modifiers;
};

// root/{album/, photos/{P1.ORF, P1.ORF.xmp, P2.ORF}, sub/inner/}, a trash of its own, drawn
// offscreen so the pointer finds what is shown.
struct Fixture {
    TempDir dir;
    fs::path root = dir.path / "root";
    fs::path photos = root / "photos";
    fs::path album = root / "album";
    fs::path trash = dir.path / "Trash";
    fs::path bookmarks = dir.path / "config" / "gtk-3.0" / "bookmarks";
    int reloads = 0;  // the places read again after the bookmarks changed
    std::unique_ptr<rummage::ui::Offscreen> gl = offscreen();
    std::unique_ptr<rummage::ui::FontSystem> fonts;
    std::unique_ptr<rummage::ui::Renderer> renderer;
    std::unique_ptr<rummage::ui::IconSet> icons;
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    FakeDnd dnd;

    Fixture() {
        fs::create_directories(photos);
        fs::create_directories(album);
        fs::create_directories(root / "sub" / "inner");
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P2.ORF"}) {
            std::ofstream{photos / name} << name;
        }
        browser.set_home_trash(trash);
        browser.set_bookmarks_file(bookmarks);
        browser.set_places_reload([this] {
            ++reloads;
            std::vector<rummage::fs::Place> places = browser.sidebar().places();
            std::erase_if(places, [](const rummage::fs::Place& place) {
                return place.section == rummage::fs::Place::Section::bookmarks;
            });
            for (rummage::fs::Place& place :
                 rummage::fs::parse_bookmarks(rummage::core::read_text_file(bookmarks).text)) {
                places.push_back(std::move(place));
            }
            browser.set_places(std::move(places));
        });
        browser.set_places({{.section = rummage::fs::Place::Section::places,
                             .label = "Album",
                             .path = album,
                             .icon = "folder",
                             .disk = {}},
                            {.section = rummage::fs::Place::Section::places,
                             .label = "Trash",
                             .path = rummage::fs::trash_path(),
                             .icon = "trash",
                             .disk = {}}});
        if (gl) {
            fonts = std::make_unique<rummage::ui::FontSystem>();
            renderer = std::make_unique<rummage::ui::Renderer>(*fonts);
            icons = std::make_unique<rummage::ui::IconSet>(*fonts);
        }
        browser.set_drag_and_drop({
            .start =
                [this](std::vector<rummage::views::DragAndDrop::Format> formats) {
                    dnd.started = std::move(formats);
                },
            .cancel = [this] { ++dnd.cancelled; },
            .types = [this] { return dnd.types; },
            .source_actions = [this] { return dnd.source_actions; },
            .action = [this] { return dnd.action; },
            .accept =
                [this](std::optional<std::string> type, std::uint32_t actions,
                       std::uint32_t /*preferred*/) {
                    dnd.accepted = std::move(type);
                    dnd.accepted_actions = actions;
                },
            .read =
                [this](const std::string& /*type*/, rummage::views::DragAndDrop::Done done) {
                    loop.post([this, done = std::move(done)] { done(dnd.data); });
                },
            .finish = [this] { ++dnd.finished; },
            .modifiers = [this] { return dnd.modifiers; },
        });
    }

    void draw() {
        renderer->begin_frame(kWidth, kHeight, 1.0F, {});
        browser.draw(*renderer, {0.0F, 0.0F, kWidth, kHeight}, rummage::theme::Palette{}, *icons,
                     Browser::Clock::now());
        renderer->end_frame();
    }

    bool settle() {
        const bool done = run_until(loop, [this] { return !browser.loading(); });
        draw();
        return done;
    }

    void show(const fs::path& folder, Browser::Mode mode) {
        browser.open(folder);
        browser.set_mode(mode);
        REQUIRE(settle());
    }

    [[nodiscard]] std::optional<ViewHit> hit(float x, float y) {
        switch (browser.mode()) {
            case Browser::Mode::grid:
                return browser.grid().hit(x, y);
            case Browser::Mode::details:
                return browser.details().hit(x, y);
            case Browser::Mode::columns:
                break;
        }
        return browser.columns().hit(x, y);
    }

    // A point over the entry called name of folder, on its icon or name (name empty: over
    // folder's view, off its entries), as last drawn: the middle of the first line of them.
    std::pair<float, float> point_of(const fs::path& folder, const std::string& name) {
        for (float y = 0.0F; y < kHeight; y += 3.0F) {
            std::vector<float> xs;
            for (float x = 0.0F; x < kWidth; x += 10.0F) {
                const std::optional<ViewHit> found = hit(x, y);
                if (!found || found->folder->path() != folder) {
                    continue;
                }
                if (name.empty() ? !found->entry
                                 : found->entry && found->on_item &&
                                       found->folder->entries()[*found->entry].name == name) {
                    xs.push_back(x);
                }
            }
            if (!xs.empty()) {
                return {xs[xs.size() / 2], y + 6.0F};
            }
        }
        FAIL("nothing shows " << (folder / name));
        return {};
    }

    void pointer(PointerEvent::Kind kind, std::pair<float, float> at, int clicks = 1,
                 Modifiers modifiers = {}) {
        browser.pointer({.kind = kind,
                         .x = at.first,
                         .y = at.second,
                         .button = PointerButton::left,
                         .click_count = clicks,
                         .modifiers = modifiers},
                        Browser::Clock::now());
    }

    // A point on the row or cell of the entry called name of folder, away from its icon and
    // name (where a band starts), as last drawn.
    std::pair<float, float> beside(const fs::path& folder, const std::string& name) {
        for (float y = 0.0F; y < kHeight; y += 3.0F) {
            std::vector<float> xs;
            for (float x = 0.0F; x < kWidth; x += 4.0F) {
                const std::optional<ViewHit> found = hit(x, y);
                if (found && found->folder->path() == folder && found->entry && !found->on_item &&
                    found->folder->entries()[*found->entry].name == name) {
                    xs.push_back(x);
                }
            }
            if (!xs.empty()) {
                return {xs[xs.size() / 2], y + 6.0F};
            }
        }
        FAIL("no room beside " << (folder / name));
        return {};
    }

    // A band from one point to another, the button still down.
    void band(std::pair<float, float> from, std::pair<float, float> to, Modifiers modifiers = {}) {
        pointer(PointerEvent::Kind::motion, from);
        pointer(PointerEvent::Kind::press, from, 1, modifiers);
        pointer(PointerEvent::Kind::motion, {from.first + 2.0F, from.second + 2.0F});
        CHECK_FALSE(browser.banding());  // not yet: a click may move a pixel or two
        pointer(PointerEvent::Kind::motion, to);
        REQUIRE(browser.banding());
        CHECK_FALSE(browser.dragging());
        draw();
    }

    [[nodiscard]] Names selected(const fs::path& folder) {
        std::optional<ViewHit> found;
        for (float y = 0.0F; y < kHeight && !found; y += 3.0F) {
            for (float x = 0.0F; x < kWidth; x += 10.0F) {
                if (std::optional<ViewHit> at = hit(x, y); at && at->folder->path() == folder) {
                    found = std::move(at);
                    break;
                }
            }
        }
        REQUIRE(found);
        return found.value_or(ViewHit{}).folder->selected_names();
    }

    // Presses on the entry and moves past the threshold: a drag starts.
    void drag(const fs::path& folder, const std::string& name) {
        const auto at = point_of(folder, name);
        pointer(PointerEvent::Kind::motion, at);
        pointer(PointerEvent::Kind::press, at);
        pointer(PointerEvent::Kind::motion, {at.first + 12.0F, at.second + 4.0F});
        REQUIRE(browser.dragging());
    }

    // Moves a drag over the point and drops it there.
    void drop_at(std::pair<float, float> at) {
        browser.drop_event({.kind = DropEvent::Kind::enter, .x = at.first, .y = at.second});
        browser.drop_event({.kind = DropEvent::Kind::motion, .x = at.first, .y = at.second});
        browser.drop_event({.kind = DropEvent::Kind::drop, .x = at.first, .y = at.second});
        browser.drag_ended();
        REQUIRE(settle());
    }
};

}  // namespace

TEST_CASE("grid: large icons fit narrow views and hit testing follows the drawn cells") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.root, Browser::Mode::grid);
    auto& grid = f.browser.grid();
    grid.set_info_visible(false);
    rummage::theme::Palette palette;
    palette.primary = rummage::ui::Color::rgb(0xFF0000);     // folder icons
    palette.on_surface = rummage::ui::Color::rgb(0x00FF00);  // names
    for (const int size : {384, 512}) {
        for (const int width : {200, 280}) {
            CAPTURE(size);
            CAPTURE(width);
            grid.set_size_index(Browser::grid_index(size));
            f.renderer->begin_frame(kWidth, kHeight, 1.0F, {});
            grid.draw(*f.renderer, {0, 0, static_cast<float>(width), kHeight}, palette, *f.icons,
                      Browser::Clock::now());
            f.renderer->end_frame();
            CHECK(grid.icon_size() == size);  // fitting does not change the saved zoom
            const float center = static_cast<float>(width) / 2.0F;
            const float icon_bottom = width == 200 ? 140.0F : 220.0F;
            const auto icon = grid.hit(center, 60);
            REQUIRE(icon);
            CHECK(icon->entry == 0);
            CHECK(icon->on_item);
            const auto gutter = grid.hit(30, 60);
            REQUIRE(gutter);
            CHECK_FALSE(gutter->on_item);
            const auto name = grid.hit(center, icon_bottom + 12);
            REQUIRE(name);
            CHECK(name->entry == 0);
            CHECK(name->on_item);
            const auto gap = grid.hit(center, icon_bottom + 30);
            REQUIRE(gap);
            CHECK_FALSE(gap->on_item);
            const auto next = grid.hit(center, icon_bottom + 80);
            REQUIRE(next);
            CHECK(next->entry == 1);
            CHECK(next->on_item);
            CHECK_FALSE(grid.hit(static_cast<float>(width), 60));

            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
            glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            int red = 0;
            int left = width;
            int right = 0;
            int top = kHeight;
            int bottom = 0;
            for (int y = 0; y < static_cast<int>(icon_bottom + 40); ++y) {
                for (int x = 0; x < width; ++x) {
                    const auto at = (static_cast<std::size_t>(kHeight - 1 - y) * kWidth +
                                     static_cast<std::size_t>(x)) *
                                    4;
                    if (pixels[at] > 128 && pixels[at + 1] < 32) {
                        ++red;
                        left = std::min(left, x);
                        right = std::max(right, x);
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
                }
            }
            CHECK(red > 0);
            CHECK(left >= 44);
            CHECK(right < width - 44);
            CHECK(top >= 28);
            CHECK(bottom < icon_bottom);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("drag: a press on an entry moved a few pixels drags the selection") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.photos, Browser::Mode::details);
    CHECK(f.browser.key(key(XKB_KEY_a, {.ctrl = true}), Browser::Clock::now()));
    f.draw();

    // A click on one of them without moving: that one alone, once the button is up.
    const auto at = f.point_of(f.photos, "P2.ORF");
    f.pointer(PointerEvent::Kind::press, at);
    CHECK(f.browser.details().folder()->selected_count() == 3);
    f.pointer(PointerEvent::Kind::release, at);
    CHECK(f.browser.details().folder()->selected_count() == 1);
    CHECK_FALSE(f.browser.dragging());

    CHECK(f.browser.key(key(XKB_KEY_a, {.ctrl = true}), Browser::Clock::now()));
    f.drag(f.photos, "P2.ORF");
    CHECK(f.browser.details().folder()->selected_count() == 3);
    REQUIRE(f.dnd.started.size() == 3);
    CHECK(f.dnd.started[0].first == "text/uri-list");
    const rummage::views::ClipboardFiles offered =
        rummage::views::parse_uri_list(f.dnd.started[0].second);
    CHECK(offered.paths ==
          std::vector<fs::path>{f.photos / "P1.ORF", f.photos / "P1.ORF.xmp", f.photos / "P2.ORF"});
    CHECK(f.dnd.started[1].second == (f.photos / "P1.ORF").string() + "\n" +
                                         (f.photos / "P1.ORF.xmp").string() + "\n" +
                                         (f.photos / "P2.ORF").string());

    // Escape stops it.
    CHECK(f.browser.key(key(XKB_KEY_Escape), Browser::Clock::now()));
    CHECK(f.dnd.cancelled == 1);
    CHECK_FALSE(f.browser.dragging());
}

TEST_CASE("drop: onto a folder in the columns moves on the same disk, Ctrl copies; Ctrl+Z") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.browser.reveal(f.root, f.photos / "P2.ORF");
    REQUIRE(f.settle());
    f.drag(f.photos, "P2.ORF");
    const auto album = f.point_of(f.root, "album");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = album.first, .y = album.second});
    CHECK(f.dnd.accepted == std::optional<std::string>{"text/uri-list"});
    CHECK(f.browser.drop_mark().directory == f.root);
    CHECK(f.browser.drop_mark().name == "album");
    f.drop_at(album);
    CHECK(f.dnd.finished == 1);
    CHECK(listing(f.album) == Names{"P2.ORF"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});
    CHECK(f.browser.drop_mark().directory.empty());

    f.dnd.modifiers = {.ctrl = true};
    f.drag(f.photos, "P1.ORF");
    f.drop_at(f.point_of(f.root, "album"));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp"});

    CHECK(f.browser.key(key(XKB_KEY_z, {.ctrl = true}), Browser::Clock::now()));
    REQUIRE(f.settle());
    CHECK(listing(f.album) == Names{"P2.ORF"});  // the copy went to the trash
}

TEST_CASE("drop: Shift moves, onto the empty part of a view too") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.root, Browser::Mode::columns);
    f.browser.columns().focused_folder()->select_name("photos");
    REQUIRE(f.settle());
    f.dnd.modifiers = {.shift = true};
    f.drag(f.photos, "P1.ORF");
    f.drop_at(f.point_of(f.root, ""));  // the root's column, below its rows
    CHECK(listing(f.root) == Names{"P1.ORF", "P1.ORF.xmp", "album", "photos", "sub"});
}

TEST_CASE("drop: not into itself, nor where they already are") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.root, Browser::Mode::details);
    f.drag(f.root, "sub");
    const auto sub = f.point_of(f.root, "sub");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = sub.first, .y = sub.second});
    CHECK_FALSE(f.dnd.accepted);
    CHECK(f.browser.drop_mark().directory.empty());
    const auto here = f.point_of(f.root, "");
    f.browser.drop_event({.kind = DropEvent::Kind::motion, .x = here.first, .y = here.second});
    CHECK_FALSE(f.dnd.accepted);
    const auto album = f.point_of(f.root, "album");
    f.browser.drop_event({.kind = DropEvent::Kind::motion, .x = album.first, .y = album.second});
    CHECK(f.dnd.accepted);
    f.browser.drop_event({.kind = DropEvent::Kind::leave, .x = album.first, .y = album.second});
    CHECK(f.browser.drop_mark().directory.empty());
    f.drop_at(sub);  // dropped there all the same: nothing happens
    CHECK(listing(f.root / "sub") == Names{"inner"});
}

TEST_CASE("drop: from another program, its files are copied; web addresses are not files") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    const fs::path outside = f.dir.path / "Download dir";
    fs::create_directories(outside);
    std::ofstream{outside / "naïve photo.jpg"} << "jpeg";
    f.show(f.root, Browser::Mode::grid);
    f.dnd.types = {"text/uri-list", "text/plain"};
    f.dnd.source_actions = rummage::ui::kDndCopy;
    f.dnd.action = rummage::ui::kDndCopy;
    f.dnd.data = rummage::views::uri_list(std::vector<fs::path>{outside / "naïve photo.jpg"}) +
                 "https://example.com/a.jpg\r\n";
    const auto album = f.point_of(f.root, "album");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = album.first, .y = album.second});
    CHECK(f.dnd.accepted == std::optional<std::string>{"text/uri-list"});
    CHECK(f.dnd.accepted_actions == rummage::ui::kDndCopy);  // the source allows nothing else
    f.drop_at(album);
    CHECK(listing(f.album) == Names{"naïve photo.jpg"});
    CHECK(listing(outside) == Names{"naïve photo.jpg"});

    f.dnd.data = "https://example.com/a.jpg\r\n";
    f.drop_at(f.point_of(f.root, "sub"));
    CHECK(f.browser.status_bar().message() == "Only files can be dropped");

    // Without files in it, a drag finds nowhere to go.
    f.dnd.types = {"text/plain"};
    f.dnd.accepted.reset();
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = album.first, .y = album.second});
    CHECK_FALSE(f.dnd.accepted);
}

TEST_CASE("drop: onto a step of the breadcrumb and a place of the sidebar; the trash trashes") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.photos, Browser::Mode::details);
    // The breadcrumb's step for root.
    std::optional<std::pair<float, float>> crumb;
    for (float y = 0; y < 160 && !crumb; y += 4) {
        for (float x = 200.0F; x < kWidth && !crumb; x += 4.0F) {
            if (f.browser.breadcrumb().path_at(x, y) == f.root)
                crumb = std::pair{x, y};
        }
    }
    REQUIRE(crumb);
    f.drag(f.photos, "P2.ORF");
    f.drop_at(crumb.value_or(std::pair{0.0F, 0.0F}));
    CHECK(listing(f.root) == Names{"P2.ORF", "album", "photos", "sub"});

    // The sidebar's places: Album, then the trash.
    const auto place = [&f](const fs::path& path) {
        for (float y = 0.0F; y < kHeight; y += 3.0F) {
            if (const rummage::fs::Place* found = f.browser.sidebar().place_under(40.0F, y);
                found != nullptr && found->path == path) {
                return std::pair{40.0F, y};
            }
        }
        FAIL("no place for " << path);
        return std::pair{0.0F, 0.0F};
    };
    f.drag(f.photos, "P1.ORF");
    f.drop_at(place(f.album));
    CHECK(listing(f.album) == Names{"P1.ORF", "P1.ORF.xmp"});

    f.show(f.root, Browser::Mode::details);
    f.drag(f.root, "P2.ORF");
    f.drop_at(place(rummage::fs::trash_path()));
    CHECK(listing(f.trash / "files") == Names{"P2.ORF"});
}

TEST_CASE("sidebar bookmarks: Ctrl+D and the menus add folders once; removed again") {
    Fixture f;
    f.browser.open(f.root);
    f.browser.set_mode(Browser::Mode::details);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.key(key(XKB_KEY_d, {.ctrl = true}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(rummage::core::read_text_file(f.bookmarks).text ==
          rummage::core::file_uri(f.root) + "\n");
    CHECK(f.browser.status_bar().message() == "“root” added to the sidebar");
    CHECK(f.reloads == 1);
    CHECK(f.browser.sidebar().places().back().path == f.root);

    CHECK(f.browser.key(key(XKB_KEY_d, {.ctrl = true}), Browser::Clock::now()));
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.status_bar().message() == "Already in the sidebar");
    CHECK(f.reloads == 1);

    // "Add to Sidebar" on two folders selected; files have none.
    const auto run_item = [&f](const std::string& id) {
        const auto& items = f.browser.context_menu().items();
        const auto item = std::ranges::find(items, id, &rummage::views::MenuItem::id);
        REQUIRE(item != items.end());
        const std::function<void()> action = item->action;
        f.browser.context_menu().close();
        action();
        REQUIRE(run_until(f.loop, [&f] { return !f.browser.loading(); }));
    };
    std::shared_ptr<rummage::views::Folder> shown = f.browser.details().folder();
    shown->select_name("album");
    shown->set_cursor(1, rummage::views::SelectMode::toggle);  // album, photos, sub
    REQUIRE(shown->selected_names() == Names{"album", "photos"});
    f.browser.open_menu_from_keyboard();
    run_item("add_bookmark");
    CHECK(f.browser.status_bar().message() == "2 folders added to the sidebar");

    f.browser.remove_from_sidebar(f.album);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.status_bar().message() == "“album” removed from the sidebar");
    CHECK(rummage::fs::parse_bookmarks(rummage::core::read_text_file(f.bookmarks).text).size() ==
          2);

    // The folder's own menu, from its empty part.
    f.browser.open(f.album);
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    f.browser.open_menu(500.0F, 500.0F, false);
    run_item("add_bookmark");
    CHECK(f.browser.status_bar().message() == "“album” added to the sidebar");

    // A file where the file should be: a message, nothing else.
    f.browser.set_bookmarks_file(f.root);
    f.browser.add_to_sidebar({f.photos});
    REQUIRE(run_until(f.loop, [&] { return !f.browser.loading(); }));
    CHECK(f.browser.status_bar().message().starts_with("The sidebar could not be changed: "));
}

TEST_CASE("sidebar bookmarks: folders dropped below the places are added; files are not") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.root, Browser::Mode::details);
    const std::pair<float, float> below{60.0F, 600.0F};  // the sidebar, under its last place
    REQUIRE(f.browser.sidebar().in_bookmark_zone(below.first, below.second));

    f.drag(f.photos.parent_path(), "photos");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = below.first, .y = below.second});
    CHECK(f.dnd.accepted);
    f.drop_at(below);
    CHECK(f.browser.status_bar().message() == "“photos” added to the sidebar");
    CHECK(listing(f.root) == Names{"album", "photos", "sub"});  // nothing moved
    // Now there is a "BOOKMARKS" title above the bookmark: it takes them too.
    bool title = false;
    for (float y = 0.0F; y < 400.0F && !title; y += 3.0F) {
        title = f.browser.sidebar().in_bookmark_zone(60.0F, y) &&
                f.browser.sidebar().place_under(60.0F, y) == nullptr;
    }
    CHECK(title);

    // A file is no bookmark.
    f.show(f.photos, Browser::Mode::details);
    f.drag(f.photos, "P2.ORF");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = below.first, .y = below.second});
    CHECK_FALSE(f.dnd.accepted);  // a file
    f.drop_at(below);
    CHECK(listing(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});

    // Right click on the bookmark: its menu removes it.
    for (float y = 0.0F; y < kHeight; y += 3.0F) {
        const rummage::fs::Place* place = f.browser.sidebar().place_under(40.0F, y);
        if (place != nullptr && place->section == rummage::fs::Place::Section::bookmarks) {
            f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = 40.0F, .y = y},
                              Browser::Clock::now());
            f.browser.pointer({.kind = PointerEvent::Kind::press,
                               .x = 40.0F,
                               .y = y,
                               .button = PointerButton::right,
                               .click_count = 1},
                              Browser::Clock::now());
            break;
        }
    }
    REQUIRE(f.browser.context_menu().is_open());
    REQUIRE(f.browser.context_menu().items().size() == 1);
    CHECK(f.browser.context_menu().items().front().id == "remove_bookmark");
}

TEST_CASE("sidebar disks: a click on the eject icon ejects; the right click offers the rest") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    std::vector<std::string> calls;
    f.browser.set_devices({
        .mount =
            [&calls](const std::string& volume, const rummage::views::Devices::Mounted& /*done*/) {
                calls.push_back("mount " + volume);
            },
        .unmount =
            [&calls](const std::string& volume, const rummage::views::Devices::Done& /*done*/) {
                calls.push_back("unmount " + volume);
            },
        .eject =
            [&calls](const std::string& volume, const rummage::views::Devices::Done& /*done*/) {
                calls.push_back("eject " + volume);
            },
    });
    std::vector<rummage::fs::Place> places = f.browser.sidebar().places();
    places.push_back({.section = rummage::fs::Place::Section::devices,
                      .label = "kinetic",
                      .path = f.album,
                      .icon = "usb",
                      .disk = {.volume = "sdb1", .mounted = true, .ejectable = true}});
    places.push_back({.section = rummage::fs::Place::Section::devices,
                      .label = "spare",
                      .path = {},
                      .icon = "database-off",
                      .disk = {.volume = "sdc1", .mounted = false, .ejectable = false}});
    f.browser.set_places(places);
    f.show(f.root, Browser::Mode::details);

    const std::optional<rummage::ui::Rect> eject = f.browser.sidebar().eject_icon(f.album);
    REQUIRE(eject);
    const rummage::ui::Rect icon = eject.value_or(rummage::ui::Rect{});
    const auto click = [&f](float x, float y, PointerButton button) {
        f.browser.pointer({.kind = PointerEvent::Kind::motion, .x = x, .y = y},
                          Browser::Clock::now());
        f.browser.pointer(
            {.kind = PointerEvent::Kind::press, .x = x, .y = y, .button = button, .click_count = 1},
            Browser::Clock::now());
        f.browser.pointer({.kind = PointerEvent::Kind::release, .x = x, .y = y, .button = button},
                          Browser::Clock::now());
    };
    click(icon.x + (icon.width / 2.0F), icon.y + (icon.height / 2.0F), PointerButton::left);
    CHECK(calls == std::vector<std::string>{"eject sdb1"});

    // The disk not mounted: a click mounts it; its menu can only mount it.
    float spare = -1.0F;
    for (float y = 0.0F; y < kHeight && spare < 0.0F; y += 3.0F) {
        const rummage::fs::Place* const place = f.browser.sidebar().place_under(40.0F, y);
        if (place != nullptr && place->label == "spare") {
            spare = y;
        }
    }
    REQUIRE(spare >= 0.0F);
    click(40.0F, spare, PointerButton::left);
    CHECK(calls.back() == "mount sdc1");
    click(40.0F, spare, PointerButton::right);
    REQUIRE(f.browser.context_menu().is_open());
    const auto& items = f.browser.context_menu().items();
    REQUIRE(items.size() == 2);
    CHECK(items[0].id == "mount");
    CHECK(items[0].enabled);
    CHECK(items[1].id == "unmount");
    CHECK_FALSE(items[1].enabled);
    f.browser.context_menu().close();

    // Nothing can be dropped onto a disk not mounted.
    f.drag(f.root, "sub");
    f.browser.drop_event({.kind = DropEvent::Kind::enter, .x = 40.0F, .y = spare});
    CHECK_FALSE(f.dnd.accepted);
}

TEST_CASE("band: from empty space, or beside a name, selects what it touches; Ctrl adds") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    for (const Browser::Mode mode :
         {Browser::Mode::columns, Browser::Mode::details, Browser::Mode::grid}) {
        CAPTURE(static_cast<int>(mode));
        f.show(f.photos, mode);
        // From empty space to P1.ORF.xmp: in the lists up from below the entries (that one and
        // the one after it), in the grid down from the margin above the cells (that one).
        const auto empty = f.point_of(f.photos, "");
        const auto xmp = f.point_of(f.photos, "P1.ORF.xmp");
        f.band(empty, xmp);
        CHECK(f.selected(f.photos) ==
              (mode == Browser::Mode::grid ? Names{"P1.ORF.xmp"} : Names{"P1.ORF.xmp", "P2.ORF"}));
        f.pointer(PointerEvent::Kind::release, xmp);
        CHECK_FALSE(f.browser.banding());
        f.draw();

        // Beside P1.ORF's name down to P2.ORF: all three, and nothing dragged.
        f.band(f.beside(f.photos, "P1.ORF"), f.point_of(f.photos, "P2.ORF"));
        CHECK(f.selected(f.photos) == Names{"P1.ORF", "P1.ORF.xmp", "P2.ORF"});
        f.pointer(PointerEvent::Kind::release, f.point_of(f.photos, "P2.ORF"));
        CHECK(f.dnd.started.empty());
        f.draw();

        // A click on P1.ORF, then a band with Ctrl over P2.ORF alone: both.
        const auto first = f.point_of(f.photos, "P1.ORF");
        f.pointer(PointerEvent::Kind::press, first);
        f.pointer(PointerEvent::Kind::release, first);
        f.draw();
        REQUIRE(f.selected(f.photos) == Names{"P1.ORF"});
        const auto last = f.beside(f.photos, "P2.ORF");
        f.band(last, {last.first + 1.0F, last.second + 7.0F}, {.ctrl = true});
        CHECK(f.selected(f.photos) == Names{"P1.ORF", "P2.ORF"});
        f.pointer(PointerEvent::Kind::release, last);
        f.draw();

        // A click beside a name, without moving, selects that entry as before.
        const auto beside = f.beside(f.photos, "P2.ORF");
        f.pointer(PointerEvent::Kind::press, beside);
        f.pointer(PointerEvent::Kind::release, beside);
        CHECK(f.selected(f.photos) == Names{"P2.ORF"});
        CHECK_FALSE(f.browser.banding());
    }
}

TEST_CASE("band: beyond the bottom of the list, it scrolls and selects as it goes") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    const fs::path many = f.root / "many";
    fs::create_directories(many);
    for (int i = 0; i < 80; ++i) {
        std::ofstream{many / std::format("photo-{:02}.jpg", i)} << i;
    }
    f.show(many, Browser::Mode::details);
    const std::shared_ptr<rummage::views::Folder> folder = f.browser.details().folder();
    const auto start = f.beside(many, "photo-00.jpg");
    f.band(start, {start.first, static_cast<float>(kHeight) - 2.0F});
    // The rows shown are selected at once; the rest come while the list scrolls.
    const std::size_t shown = folder->selected_count();
    CHECK(shown > 5);
    CHECK(shown < 80);
    CHECK(run_until(f.loop, [&] {
        f.draw();
        return folder->selected_count() == 80;
    }));
    f.pointer(PointerEvent::Kind::release, {start.first, static_cast<float>(kHeight) - 2.0F});
    CHECK_FALSE(f.browser.banding());
    CHECK(folder->selected_count() == 80);
}

TEST_CASE("details tree: disclosure clicks, child opening and dragging use the child directory") {
    Fixture f;
    if (!f.gl) {
        return;
    }
    f.show(f.root, Browser::Mode::details);
    f.browser.details().folder()->select_name("photos");
    f.draw();
    const auto parent = f.point_of(f.root, "photos");
    // The disclosure is the gap in the hit area before this folder's icon.
    std::optional<std::pair<float, float>> arrow;
    bool in_rows = false;
    for (float x = 0; x < parent.first; x += 1.0F) {
        const auto hit = f.hit(x, parent.second);
        if (hit)
            in_rows = true;
        else if (in_rows) {
            arrow = {x + 2.0F, parent.second};
            break;
        }
    }
    REQUIRE(arrow);
    f.pointer(PointerEvent::Kind::press, *arrow);
    f.pointer(PointerEvent::Kind::motion, {arrow->first + 12.0F, arrow->second});
    CHECK_FALSE(f.browser.dragging());
    CHECK_FALSE(f.browser.banding());
    f.pointer(PointerEvent::Kind::release, *arrow);
    REQUIRE(f.settle());
    CHECK(f.browser.details().root_folder()->path() == f.root);
    const auto child = f.point_of(f.photos, "P2.ORF");
    const auto hit = f.hit(child.first, child.second);
    REQUIRE(hit);
    REQUIRE(hit->entry);
    CHECK(hit->folder->path() == f.photos);
    CHECK(hit->folder->entries()[*hit->entry].name == "P2.ORF");
    fs::path opened;
    f.browser.set_open_handler(
        [&](const fs::path& directory, std::vector<rummage::fs::Entry> files) {
            REQUIRE(files.size() == 1);
            opened = directory / files.front().name;
        });
    f.pointer(PointerEvent::Kind::press, child, 2);
    f.pointer(PointerEvent::Kind::release, child, 2);
    CHECK(opened == f.photos / "P2.ORF");
    f.draw();
    f.drag(f.photos, "P2.ORF");
    REQUIRE_FALSE(f.dnd.started.empty());
    f.browser.key(key(XKB_KEY_Escape), Browser::Clock::now());
    f.pointer(PointerEvent::Kind::release, child);
    f.draw();
    // Collapsing an ancestor clears its selected child and restores the parent's context.
    f.pointer(PointerEvent::Kind::press, *arrow);
    f.pointer(PointerEvent::Kind::release, *arrow);
    f.draw();
    CHECK(f.browser.details().folder()->path() == f.root);
    CHECK(f.browser.details().folder()->selected_names() == Names{"photos"});
}
