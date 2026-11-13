#include "rummage/fs/icon.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/png.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/text.hpp"
#include "temp_dir.hpp"

using rummage::fs::Entry;
using rummage::fs::EntryKind;
using rummage::fs::icon_name;
using rummage::fs::MimeDatabase;

namespace {

// The icon of a regular, non-empty file, typed by its name as the loader does.
std::string_view icon_of(const MimeDatabase& db, std::string name) {
    Entry entry{.name = std::move(name), .kind = EntryKind::file, .size = 100};
    entry.mime = db.by_name(entry.name).mime;
    return icon_name(db, entry);
}

}  // namespace

TEST_CASE("icons: every name exists in the icon font") {
    rummage::ui::FontSystem fonts;
    rummage::ui::IconSet icons{fonts};
    REQUIRE(icons.available());  // compiled into the program
    for (const std::string_view name : rummage::fs::icon_names_in_use()) {
        CHECK_MESSAGE(icons.has(name), name);
    }
    // The views' own: sidebar, breadcrumb, status bar, info pane, details header, title bar.
    for (const std::string_view name :
         {"player-eject", "chevron-right", "chevron-up", "chevron-down", "x", "layout-columns",
          "layout-grid", "list-details", "help", "files", "home", "server", "trash", "trash-filled",
          "usb", "database-off", "minus", "square", "squares"}) {
        CHECK_MESSAGE(icons.has(name), name);
    }
    CHECK_FALSE(icons.has("no-such-icon"));
    CHECK_FALSE(icons.has("folder-filled"));  // only the filled icons listed are kept
    CHECK_FALSE(icons.has("-filled"));
}

TEST_CASE("place icons: custom images, builtin overrides, fallback and live replacement") {
    namespace ui = rummage::ui;
    rummage::test::TempDir dir;
    const auto red = dir.write(
        "red.svg",
        "<svg width='32' height='32'><rect width='32' height='32' fill='#ff0000'/></svg>");
    const rummage::picture::Image green_image{.width = 1, .height = 1, .rgba = {0, 255, 0, 255}};
    const auto png = rummage::picture::encode_png(green_image);
    const auto green = dir.write(
        "green.png", std::string_view{reinterpret_cast<const char*>(png.data()), png.size()});
    ui::Offscreen gl{32, 32};
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    const auto render = [&](std::string_view key, std::string_view fallback = "home") {
        renderer.begin_frame(32, 32, 1.0F, ui::Color::rgb(0));
        icons.draw_place(renderer, key, fallback, 0, 0, 32, ui::Color::rgb(0xFFFFFF));
        renderer.end_frame();
        std::array<std::uint8_t, 32 * 32 * 4> pixels{};
        glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto normal = render("home");
    const auto photo = render("pictures", "photo");
    REQUIRE(normal != photo);
    icons.set_place_icons({{"home", red.string()}, {"trash", green.string()}});
    const auto custom = render("home");
    CHECK(custom != normal);
    CHECK(custom[0] == 255);
    CHECK(custom[1] == 0);
    CHECK(render("pictures", "photo") == photo);
    CHECK(render("trash_full")[1] == 255);  // inherits the trash override
    icons.set_place_icons(
        {{"home", green.string()}, {"trash", red.string()}, {"trash_full", green.string()}});
    CHECK(render("home")[1] == 255);
    CHECK(render("trash_full")[1] == 255);  // explicit full state wins
    icons.set_place_icons({{"home", "builtin:photo"}});
    CHECK(render("home") == photo);
    for (const auto& missing :
         {"builtin:does-not-exist", "no-such-theme-icon", "/no-such-directory/icon.svg"}) {
        icons.set_place_icons({{"home", missing}});
        CHECK(render("home") == normal);
    }
    icons.set_place_icons({});
    CHECK(render("home") == normal);
}

TEST_CASE("icons: kinds") {
    const auto db = MimeDatabase::load({});  // no database needed
    CHECK(icon_name(*db, Entry{.name = "Photos", .kind = EntryKind::directory}) == "folder");
    CHECK(icon_name(*db, Entry{.name = "l", .kind = EntryKind::directory, .is_link = true}) ==
          "folder-symlink");
    CHECK(icon_name(*db, Entry{.name = "l", .kind = EntryKind::other, .is_link = true}) ==
          "link-off");
    CHECK(icon_name(*db, Entry{.name = "fifo", .kind = EntryKind::other}) == "plug");
    CHECK(icon_name(*db, Entry{.name = "unknown-yet", .kind = EntryKind::file}) == "file");
    CHECK(icon_name(*db, Entry{.name = "_6240061.ORF.xmp", .kind = EntryKind::file}) ==
          "adjustments-horizontal");
}

TEST_CASE("icons: types, on the system database") {
    const auto db = MimeDatabase::load();
    if (db->glob_count() == 0) {
        MESSAGE("no shared-mime-info on this system: skipped");
        return;
    }
    CHECK(icon_of(*db, "P1010001.ORF") == "aperture");  // camera RAW: image/x-dcraw subtypes
    CHECK(icon_of(*db, "IMG_0001.CR2") == "aperture");
    CHECK(icon_of(*db, "DSC0001.NEF") == "aperture");
    CHECK(icon_of(*db, "photo.jpg") == "photo");
    CHECK(icon_of(*db, "drawing.svg") == "vector");
    CHECK(icon_of(*db, "paper.pdf") == "file-type-pdf");
    CHECK(icon_of(*db, "backup.tar.gz") == "file-zip");
    CHECK(icon_of(*db, "letter.odt") == "file-description");
    CHECK(icon_of(*db, "sheet.ods") == "file-spreadsheet");
    CHECK(icon_of(*db, "slides.odp") == "presentation");
    CHECK(icon_of(*db, "main.cpp") == "file-code");
    CHECK(icon_of(*db, "build.sh") == "file-code");
    CHECK(icon_of(*db, "config.toml") == "file-code");
    CHECK(icon_of(*db, "README.md") == "markdown");
    CHECK(icon_of(*db, "notes.txt") == "file-text");
    CHECK(icon_of(*db, "song.flac") == "music");
    CHECK(icon_of(*db, "clip.mp4") == "movie");
    CHECK(icon_of(*db, "font.ttf") == "typography");
    CHECK(icon_of(*db, "disk.iso") == "disc");
    CHECK(icon_of(*db, "app.desktop") == "app-window");

    // Empty: the type is x-zerosize, the icon follows the name.
    Entry empty{.name = "notes.txt", .kind = EntryKind::file, .size = 0};
    empty.mime = MimeDatabase::kZeroSize;
    CHECK(icon_name(*db, empty) == "file-text");
}
