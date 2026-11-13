// rummage-snapshot: renders the test scene (or the column view, or the grid) offscreen and writes
// it as a binary PPM.
//
//   rummage-snapshot [--columns DIR [--reveal PATH] [--keys KEYS] [--thumbnails CACHE]
//                    [--sort FIELDS] [--info-expanded] [--info-width W] [--bench FRAMES]
//                    [--walk FRAMES] [--message TEXT] [--transfer TEXT] [--conflict NAME]
//                    [--memory [--trim] [--default-malloc]]] [--csd STATE] [--desktop LOOK]
//                    [--font FAMILY] [--status-text-size SIZE] <out.ppm>
//                    [logical-width logical-height scale [colors.toml]]
//
// KEYS are pressed in the column view before the snapshot: comma-separated xkb key names with
// optional Ctrl+, Shift+, Alt+ prefixes, e.g. "Down,Shift+Down,Ctrl+Down" (a single letter also
// types itself: "F2,L,a,k,e" renames to "Lake"); "Ctrl+2" switches to
// the grid (then --bench scrolls the grid), "Ctrl+3" the details (--bench scrolls them; --sort
// clicks column titles after the keys: "size", "modified,modified" for newest first); with "space"
// last, Quick Look opens (and --bench scrolls its text page, when it shows one). --thumbnails gives
// the grid a thumbnail cache directory (none by default: icons only, nothing written to ~/.cache).
// The sidebar lists the places of $HOME and $XDG_CONFIG_HOME; "Ctrl+B" hides it, "F6" focuses it.
// --drag NAME presses on the entry NAME of the folder shown and drags it a little (the drag image
// drawn under the pointer); --drop-over X,Y then moves that drag (or, alone, another program's
// drag of files) over the point X,Y, which shows where it would drop.
// --band X1,Y1,X2,Y2 presses at X1,Y1 and moves to X2,Y2 with the button down: a rubber band
// when the press is on empty space.
// --fake-disks adds two disks to the sidebar as udisks2 would: "kinetic", external and not mounted,
// and "SD card", external and mounted on the folder shown (its eject icon).
// --message shows TEXT in the status bar as opening a file would ("!TEXT": as an error).
// --transfer shows TEXT as a transfer's progress, 40 % done; --conflict asks about NAME as a
// paste would. --memory prints the memory report (ROADMAP 6.3b) at the end, once the workers'
// results are in; with --trim, after malloc_trim(0) and the time it took. --default-malloc leaves
// glibc's mmap threshold as it is (the app sets it: core::give_back_large_blocks), to compare.
// --csd draws rummage's own title bar above, as where the compositor draws none; STATE is
// comma-separated words: "normal", "maximized", "inactive", "no-minimize", and "hover-close",
// "hover-maximize", "press-close" for the pointer over a button.
// --desktop draws with the palette made from a desktop's look, as without colors.toml under GNOME
// or Plasma: LOOK is "dark" or "light", with an accent after a comma: "dark,#3584e4".
// --font FAMILY and --status-text-size SIZE exercise UI font and status bar sizing.
//
// Uses a surfaceless EGL context (EGL_MESA_platform_surfaceless) and a framebuffer object, so
// it never talks to the compositor and never opens a window: it is how rendering is checked
// without touching the user's screen.

#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <malloc.h>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <xkbcommon/xkbcommon.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/log.hpp"
#include "rummage/core/memory.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/engines/engines.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/platform/opener.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/clipboard.hpp"
#include "rummage/ui/color.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/browser.hpp"
#include "rummage/views/file_picker.hpp"
#include "rummage/views/title_bar.hpp"
#include "test_scene.hpp"

namespace {

namespace log = rummage::log;
namespace core = rummage::core;
namespace engines = rummage::engines;
namespace isolate = rummage::isolate;
namespace preview = rummage::preview;
namespace ui = rummage::ui;
namespace views = rummage::views;

void write_ppm(const std::string& path, std::int32_t width, std::int32_t height,
               std::span<const std::uint8_t> rgba) {
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << width << ' ' << height << "\n255\n";
    // GL rows go bottom-up; PPM rows go top-down.
    const auto row_bytes = static_cast<std::size_t>(width) * 4;
    for (std::int32_t y = height - 1; y >= 0; --y) {
        const auto row = rgba.subspan(static_cast<std::size_t>(y) * row_bytes, row_bytes);
        for (std::size_t x = 0; x < row_bytes; x += 4) {
            out.put(static_cast<char>(row[x]));
            out.put(static_cast<char>(row[x + 1]));
            out.put(static_cast<char>(row[x + 2]));
        }
    }
    if (!out) {
        throw std::runtime_error{"cannot write " + path};
    }
}

// Runs the loop until done() holds or two seconds pass.
void run_until(core::EventLoop& loop, const std::function<bool()>& done) {
    const auto check =
        loop.add_timer(std::chrono::milliseconds{1}, std::chrono::milliseconds{1}, [&] {
            if (done()) {
                loop.quit(0);
            }
        });
    const auto limit = loop.add_timer(std::chrono::seconds{2}, std::chrono::milliseconds{0},
                                      [&] { loop.quit(1); });
    loop.run();
    loop.remove(check);
    loop.remove(limit);
}

// Presses on the entry called name in the view shown and moves past the drag threshold; where
// the drag image then is (nullopt: no such entry shown).
std::optional<std::pair<float, float>> drag_entry(views::Browser& browser, const std::string& name,
                                                  float width, float height) {
    const auto hit = [&browser](float x, float y) {
        switch (browser.mode()) {
            case views::Browser::Mode::grid:
                return browser.grid().hit(x, y);
            case views::Browser::Mode::details:
                return browser.details().hit(x, y);
            case views::Browser::Mode::columns:
                break;
        }
        return browser.columns().hit(x, y);
    };
    for (float y = 0.0F; y < height; y += 3.0F) {
        for (float x = 0.0F; x < width; x += 10.0F) {
            const std::optional<views::ViewHit> found = hit(x, y);
            if (!found || !found->entry || found->folder->entries()[*found->entry].name != name) {
                continue;
            }
            const float at_x = x + 30.0F;  // clear of the sidebar's edge
            const float at_y = y + 6.0F;
            using Kind = ui::PointerEvent::Kind;
            const auto now = views::Browser::Clock::now();
            browser.pointer({.kind = Kind::motion, .x = at_x, .y = at_y}, now);
            browser.pointer({.kind = Kind::press,
                             .x = at_x,
                             .y = at_y,
                             .button = ui::PointerButton::left,
                             .click_count = 1},
                            now);
            browser.pointer({.kind = Kind::motion, .x = at_x + 40.0F, .y = at_y + 20.0F}, now);
            if (!browser.dragging()) {
                log::warn("--drag {}: the press did not start a drag", name);
                return std::nullopt;
            }
            return std::pair{at_x + 48.0F, at_y + 28.0F};
        }
    }
    log::warn("--drag {}: no such entry shown", name);
    return std::nullopt;
}

// "Down,Shift+Down,Ctrl+a" -> key events.
std::vector<ui::KeyEvent> parse_keys(std::string_view keys) {
    std::vector<ui::KeyEvent> events;
    while (!keys.empty()) {
        const std::size_t comma = keys.find(',');
        std::string_view key = keys.substr(0, comma);
        keys = comma == std::string_view::npos ? std::string_view{} : keys.substr(comma + 1);
        ui::KeyEvent event;
        for (;;) {
            if (key.starts_with("Ctrl+")) {
                event.modifiers.ctrl = true;
                key.remove_prefix(5);
            } else if (key.starts_with("Shift+")) {
                event.modifiers.shift = true;
                key.remove_prefix(6);
            } else if (key.starts_with("Alt+")) {
                event.modifiers.alt = true;
                key.remove_prefix(4);
            } else {
                break;
            }
        }
        event.keysym = xkb_keysym_from_name(std::string{key}.c_str(), XKB_KEYSYM_NO_FLAGS);
        if (key.size() == 1 && !event.modifiers.ctrl && !event.modifiers.alt) {
            event.text = std::string{key};  // a letter types itself (a name being edited)
        }
        if (event.keysym == XKB_KEY_NoSymbol) {
            throw std::runtime_error{"unknown key: " + std::string{key}};
        }
        events.push_back(std::move(event));
    }
    return events;
}

}  // namespace

int main(int argc, char** argv) {
    log::init_from_env();  // RUMMAGE_LOG=debug: Quick Look timings, among others
    std::span args{argv, static_cast<std::size_t>(argc)};
    // --columns DIR: the column view instead of the test scene; --reveal PATH: with the columns
    // opened down to PATH (inside DIR); --bench N: then scroll the last column (or the grid) over N
    // frames and report the time per frame.
    std::filesystem::path columns_dir;
    std::filesystem::path reveal;
    std::string keys;
    std::string split_folder;
    bool stacked_panes = false;
    std::filesystem::path thumbnails;
    std::string sort;
    int bench_frames = 0;
    int walk_frames = 0;  // --walk N: Down once per frame, N frames, workers' results between
    bool info_expanded = false;
    bool memory = false;
    bool fake_disks = false;
    bool trim = false;
    bool default_malloc = false;
    float info_width = 0.0F;  // the info pane as if dragged to this width
    std::string message;
    std::string transfer;
    std::string conflict;
    std::string drag;
    std::optional<std::pair<float, float>> drop_over;
    std::vector<float> band;  // x1, y1, x2, y2
    std::optional<std::string> csd;
    std::optional<std::string> desktop;
    std::string icon_theme{"builtin"};
    std::string font_family{"sans-serif"};
    float status_text_size = 12.0F;
    std::optional<std::string> picker_mode;
    while (args.size() >= 2) {
        const std::string_view option{args[1]};
        if (option == "--icon-theme" && args.size() >= 3) {
            icon_theme = args[2];
            args = args.subspan(2);
            continue;
        }
        if (option == "--stacked") {
            stacked_panes = true;
            args = args.subspan(1);
            continue;
        }
        if (option == "--info-expanded") {  // the info pane with "More details" open
            info_expanded = true;
            args = args.subspan(1);
            continue;
        }
        if (option == "--fake-disks") {
            fake_disks = true;
            args = args.subspan(1);
            continue;
        }
        if (option == "--memory") {
            memory = true;
            args = args.subspan(1);
            continue;
        }
        if (option == "--trim") {
            trim = true;
            args = args.subspan(1);
            continue;
        }
        if (option == "--default-malloc") {
            default_malloc = true;
            args = args.subspan(1);
            continue;
        }
        if (args.size() < 3) {
            break;
        }
        if (option == "--font") {
            font_family = args[2];
        } else if (option == "--status-text-size") {
            status_text_size = std::stof(args[2]);
        } else if (option == "--picker") {
            picker_mode = args[2];
        } else if (option == "--columns") {
            columns_dir = args[2];
        } else if (option == "--reveal") {
            reveal = args[2];
        } else if (option == "--split") {
            split_folder = args[2];
        } else if (option == "--keys") {
            keys = args[2];
        } else if (option == "--thumbnails") {
            thumbnails = args[2];
        } else if (option == "--sort") {
            sort = args[2];
        } else if (option == "--info-width") {
            info_width = std::stof(args[2]);
        } else if (option == "--message") {
            message = args[2];
        } else if (option == "--transfer") {
            transfer = args[2];
        } else if (option == "--conflict") {
            conflict = args[2];
        } else if (option == "--drag") {
            drag = args[2];
        } else if (option == "--drop-over") {
            const std::string_view point{args[2]};
            const std::size_t comma = point.find(',');
            drop_over = std::pair{std::stof(std::string{point.substr(0, comma)}),
                                  std::stof(std::string{point.substr(comma + 1)})};
        } else if (option == "--band") {
            std::string_view rest{args[2]};
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                band.push_back(std::stof(std::string{rest.substr(0, comma)}));
                rest =
                    comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (band.size() != 4) {
                log::error("--band takes X1,Y1,X2,Y2");
                return 2;
            }
        } else if (option == "--csd") {
            csd = args[2];
        } else if (option == "--desktop") {
            desktop = args[2];
        } else if (option == "--walk") {
            walk_frames = std::stoi(args[2]);
        } else if (option == "--bench") {
            bench_frames = std::stoi(args[2]);
        } else {
            break;
        }
        args = args.subspan(2);
    }
    if (args.size() != 2 && args.size() != 5 && args.size() != 6) {
        log::error(
            "usage: rummage-snapshot [--columns DIR [--reveal PATH] [--keys KEYS] [--thumbnails "
            "CACHE] [--icon-theme NAME] [--sort FIELDS] [--info-expanded] [--info-width W] "
            "[--split DIR] [--stacked] "
            "[--bench FRAMES] [--walk "
            "FRAMES] [--message TEXT] [--transfer TEXT] [--conflict NAME] [--drag NAME] "
            "[--drop-over X,Y] [--band X1,Y1,X2,Y2] [--fake-disks] [--memory [--trim] "
            "[--default-malloc]]] [--csd STATE] [--desktop LOOK] "
            "[--font FAMILY] [--status-text-size SIZE] <out.ppm> "
            "[logical-width logical-height scale [colors.toml]]");
        return 2;
    }
    if (!default_malloc) {
        rummage::core::give_back_large_blocks();  // as the app does; before any thread starts
    }
    try {
        const std::string out{args[1]};
        const bool sized = args.size() >= 5;
        const float width = sized ? std::stof(args[2]) : 960.0F;
        const float height = sized ? std::stof(args[3]) : 640.0F;
        const float scale = sized ? std::stof(args[4]) : 1.0F;
        // The default palette unless a colors.toml is given: snapshots stay reproducible.
        rummage::theme::PaletteResult palette = args.size() == 6
                                                    ? rummage::theme::load_palette(args[5])
                                                    : rummage::theme::PaletteResult{};
        if (desktop) {
            const std::string_view look{*desktop};
            const std::size_t comma = look.find(',');
            rummage::theme::Desktop wanted{.dark = look.substr(0, comma) == "dark",
                                           .accent = std::nullopt};
            if (comma != std::string_view::npos) {
                const std::string hex{look.substr(comma + 1)};
                wanted.accent = rummage::ui::Color::rgb(static_cast<std::uint32_t>(
                    std::stoul(hex.substr(hex.starts_with('#') ? 1 : 0), nullptr, 16)));
            }
            palette.palette = rummage::theme::desktop_palette(wanted);
        }
        for (const std::string& problem : palette.problems) {
            log::warn("{}", problem);
        }
        const auto buffer_width = static_cast<std::int32_t>(std::lround(width * scale));
        const auto buffer_height = static_cast<std::int32_t>(std::lround(height * scale));

        const ui::Offscreen offscreen{buffer_width, buffer_height};
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(buffer_width) *
                                         static_cast<std::size_t>(buffer_height) * 4);
        {
            ui::FontSystem fonts{font_family};
            ui::Renderer renderer{fonts};
            ui::IconSet icons{fonts};
            icons.set_theme(icon_theme);
            core::EventLoop loop;
            core::WorkerPool pool{loop};
            // Pictures are made by the engines in their cages (ROADMAP 2b, 3a), never here.
            {
                auto registry =
                    std::make_shared<const engines::Registry>(engines::Registry::load_installed());
                isolate::use_engines(*registry);
                engines::set_installed(std::move(registry));
            }
            rummage::fs::DirectoryWatcher watcher{loop};
            std::unique_ptr<rummage::platform::Opener> opener;  // "Open With" lists, no launches
            std::unique_ptr<views::Browser> columns;
            std::unique_ptr<views::FilePicker> picker;
            const ui::Rect window_bounds{0.0F, 0.0F, width, height};
            views::TitleBar title_bar{{.move = [] {},
                                       .resize = [](ui::Edge /*edge*/) {},
                                       .minimize = [] {},
                                       .toggle_maximized = [] {},
                                       .window_menu = [](float /*x*/, float /*y*/) {},
                                       .close = [] {}},
                                      [] {},
                                      [](ui::CursorShape /*shape*/) {}};
            if (csd) {
                const auto has = [&](std::string_view word) {
                    return csd->find(word) != std::string::npos;
                };
                title_bar.set_state({.maximized = has("maximized"),
                                     .activated = !has("inactive"),
                                     .can_minimize = !has("no-minimize")});
                title_bar.layout(window_bounds);
                const float button_y = views::TitleBar::kHeight / 2.0F;
                const float close_x = width - 8.0F - 12.0F;
                if (has("hover-close") || has("press-close")) {
                    title_bar.pointer(
                        {.kind = ui::PointerEvent::Kind::motion, .x = close_x, .y = button_y});
                }
                if (has("press-close")) {
                    title_bar.pointer({.kind = ui::PointerEvent::Kind::press,
                                       .x = close_x,
                                       .y = button_y,
                                       .button = ui::PointerButton::left});
                }
                if (has("hover-maximize")) {
                    title_bar.pointer({.kind = ui::PointerEvent::Kind::motion,
                                       .x = close_x - 30.0F,
                                       .y = button_y});
                }
            }
            const ui::Rect bounds = csd ? title_bar.content(window_bounds) : window_bounds;
            std::optional<std::pair<float, float>> drag_image;  // where the pointer drags
            const auto frame = [&] {
                renderer.begin_frame(buffer_width, buffer_height, scale, palette.palette.surface);
                if (csd) {
                    title_bar.draw(renderer, window_bounds, palette.palette, icons, "rummage");
                }
                if (columns) {
                    if (picker) {
                        picker->draw(renderer, bounds, palette.palette, icons);
                    } else {
                        columns->draw(renderer, bounds, palette.palette, icons,
                                      views::Browser::Clock::now());
                    }
                    if (drag_image) {
                        columns->draw_drag_icon(
                            renderer,
                            {drag_image->first, drag_image->second, views::Browser::kDragIconWidth,
                             views::Browser::kDragIconHeight},
                            palette.palette, icons);
                    }
                } else {
                    rummage::app::draw_test_scene(renderer, bounds, palette.palette);
                }
                renderer.end_frame();
            };
            if (!columns_dir.empty()) {
                const auto mime = rummage::fs::MimeDatabase::load();
                columns = std::make_unique<views::Browser>(
                    loop, pool, watcher, mime, [] {}, [](ui::CursorShape /*shape*/) {},
                    thumbnails.empty()
                        ? nullptr
                        : std::make_shared<rummage::thumbs::Cache>(thumbnails, "rummage-dev"));
                std::vector<rummage::fs::Place> places =
                    rummage::fs::load_places(rummage::fs::PlaceSources::from_environment());
                if (fake_disks) {
                    places.push_back(
                        {.section = rummage::fs::Place::Section::devices,
                         .label = "kinetic",
                         .path = {},
                         .icon = "usb",
                         .disk = {.volume = "sdb1", .mounted = false, .ejectable = true}});
                    places.push_back(
                        {.section = rummage::fs::Place::Section::devices,
                         .label = "SD card",
                         .path = columns_dir,
                         .icon = "usb",
                         .disk = {.volume = "sdc1", .mounted = true, .ejectable = true}});
                }
                columns->status_bar().set_text_size(status_text_size);
                columns->set_places(std::move(places));
                opener = std::make_unique<rummage::platform::Opener>(
                    loop, pool, mime, rummage::platform::Environment::from_environment(),
                    [](const std::string& /*message*/, bool /*error*/) {});
                columns->set_open_with_handlers(
                    [&opener](const std::filesystem::path& directory, rummage::fs::Entry entry,
                              std::function<void(std::vector<views::Browser::AppChoice>)> done) {
                        opener->choices(directory, std::move(entry),
                                        [done = std::move(done)](
                                            std::vector<rummage::platform::AppChoice> found) {
                                            std::vector<views::Browser::AppChoice> apps;
                                            apps.reserve(found.size());
                                            for (rummage::platform::AppChoice& app : found) {
                                                apps.push_back({.id = std::move(app.id),
                                                                .name = std::move(app.name),
                                                                .is_default = app.is_default});
                                            }
                                            done(std::move(apps));
                                        });
                    },
                    [](const std::filesystem::path& /*directory*/,
                       const std::vector<rummage::fs::Entry>& /*files*/,
                       const std::string& /*app_id*/) {});
                columns->set_terminal_handler([](const std::filesystem::path& /*directory*/) {});
                if (reveal.empty()) {
                    columns->open(columns_dir);
                } else {
                    columns->reveal(columns_dir, reveal);
                }
                run_until(loop, [&] { return !columns->loading(); });
                if (picker_mode) {
                    core::PickerRequest request;
                    request.mode = *picker_mode == "save" ? core::PickerRequest::Mode::save
                                                          : core::PickerRequest::Mode::open;
                    request.directory = *picker_mode == "directory";
                    if (request.mode == core::PickerRequest::Mode::save) {
                        request.name = "Untitled.txt";
                    }
                    request.filters = {{"All files", {{0, "*"}}}, {"Text files", {{0, "*.txt"}}}};
                    request.choices = {{"readonly", "Read only", {}, "false"}};
                    picker = std::make_unique<views::FilePicker>(
                        *columns, request, mime, [] {}, [](auto) {});
                }
                columns->info_pane().set_expanded(info_expanded);
                if (info_width > 0.0F) {
                    columns->info_pane().set_width(info_width);
                }
                if (!message.empty()) {
                    const bool error = message.starts_with('!');
                    columns->show_message(error ? message.substr(1) : message, error);
                }
                if (!transfer.empty()) {
                    columns->status_bar().set_transfer(
                        views::TransferStatus{.text = transfer, .fraction = 0.4F});
                }
                if (!conflict.empty()) {
                    columns->conflict_dialog().open({.name = conflict, .destination = columns_dir},
                                                    columns_dir.filename(),
                                                    [](rummage::fs::Resolution, bool) {});
                }
                if (!split_folder.empty() && !picker) {
                    columns->toggle_split();
                    columns->open(split_folder);
                    columns->set_stacked(stacked_panes);
                    columns->focus_pane(0);
                }
                frame();  // lays out: keys need the viewport
                // Each key finds what the one before loaded (Space, then Ctrl+A on the text).
                for (const ui::KeyEvent& event : parse_keys(keys)) {
                    if (picker) {
                        picker->key(event);
                    } else {
                        columns->key(event, views::Browser::Clock::now());
                    }
                    frame();  // lays out newly opened dialogs and starts their visible work
                    run_until(loop, [&] { return !columns->loading(); });
                    frame();
                }
                std::string_view fields{sort};
                while (!fields.empty()) {
                    const std::size_t comma = std::min(fields.find(','), fields.size());
                    const std::string_view field = fields.substr(0, comma);
                    fields.remove_prefix(std::min(comma + 1, fields.size()));
                    using Field = views::DetailsView::Field;
                    Field chosen = Field::name;
                    if (field == "size") {
                        chosen = Field::size;
                    } else if (field == "modified") {
                        chosen = Field::modified;
                    } else if (field == "type") {
                        chosen = Field::type;
                    }
                    const auto start = std::chrono::steady_clock::now();
                    columns->details().sort_by(chosen);
                    log::info("sorted by {} in {:.2f} ms", field,
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count());
                }
                run_until(loop, [&] { return !columns->loading(); });
                // The first frame asks for the visible entries' types and, in the grid, their
                // thumbnails, made on workers; each frame uploads the ones that arrived.
                const auto start = std::chrono::steady_clock::now();
                frame();
                if (columns->loading()) {
                    for (int i = 0; i < 1000 && columns->loading(); ++i) {
                        run_until(loop, [&] { return !columns->loading(); });
                        frame();
                    }
                    log::info("thumbnails ready in {:.0f} ms",
                              std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count());
                }
                run_until(loop, [] { return false; });  // let sniffed types arrive (2 s)
                // Let the horizontal glide end, so the snapshot shows where it settles.
                for (int i = 0; i < 200 && columns->animating(); ++i) {
                    frame();
                }
                if (!drag.empty() || drop_over) {
                    columns->set_drag_and_drop(
                        {.start = [](const std::vector<views::DragAndDrop::Format>& /*f*/) {},
                         .cancel = {},
                         .types = [] { return std::vector<std::string>{"text/uri-list"}; },
                         .source_actions = [] { return ui::kDndCopy; },
                         .action = {},
                         .accept = {},
                         .read = {},
                         .finish = {},
                         .modifiers = {}});
                }
                if (!drag.empty()) {
                    drag_image = drag_entry(*columns, drag, width, height);
                    frame();
                }
                if (band.size() == 4) {
                    using Kind = ui::PointerEvent::Kind;
                    const auto at = [&](Kind kind, float x, float y) {
                        columns->pointer({.kind = kind,
                                          .x = x,
                                          .y = y,
                                          .button = ui::PointerButton::left,
                                          .click_count = 1},
                                         std::chrono::steady_clock::now());
                    };
                    at(Kind::motion, band[0], band[1]);
                    at(Kind::press, band[0], band[1]);
                    frame();
                    at(Kind::motion, band[2], band[3]);
                    if (!columns->banding()) {
                        log::warn("--band: the press did not start a band");
                    }
                    frame();
                }
                if (drop_over) {
                    const auto [x, y] = *drop_over;
                    columns->drop_event({.kind = ui::DropEvent::Kind::enter, .x = x, .y = y});
                    columns->drop_event({.kind = ui::DropEvent::Kind::motion, .x = x, .y = y});
                    if (drag_image) {
                        drag_image = std::pair{x + 8.0F, y + 8.0F};
                    }
                }
            }
            // What --bench scrolls: Quick Look's text page, the last column, or the grid.
            ui::Scroller* bench_scroller = nullptr;
            std::size_t bench_entries = 0;
            float bench_extent = 0.0F;
            const preview::Content* const shown =
                columns ? columns->quick_look().content() : nullptr;
            const auto* const text = shown ? std::get_if<preview::Text>(shown) : nullptr;
            if (text != nullptr) {
                bench_scroller = &columns->quick_look().text_scroller();
                bench_entries = text->lines.size();
                bench_extent =
                    static_cast<float>(bench_entries) * views::QuickLook::kTextLineHeight;
            } else if (columns && columns->mode() == views::Browser::Mode::details) {
                views::DetailsView& details = columns->details();
                bench_scroller = &details.scroller();
                bench_entries = details.folder()->entries().size();
                bench_extent = static_cast<float>(bench_entries) * views::DetailsView::kRowHeight;
            } else if (columns && columns->mode() == views::Browser::Mode::grid) {
                views::GridView& grid = columns->grid();
                bench_scroller = &grid.scroller();
                bench_entries = grid.folder()->entries().size();
                const std::size_t rows = (bench_entries + grid.columns() - 1) / grid.columns();
                bench_extent = static_cast<float>(rows) * grid.cell_height();
            } else if (columns) {
                views::Column& column =
                    columns->columns().column(columns->columns().column_count() - 1);
                bench_scroller = &column.scroller();
                bench_entries = column.folder().entries().size();
                bench_extent = static_cast<float>(bench_entries) * views::Column::kRowHeight;
            }
            if (bench_scroller != nullptr && bench_frames > 0) {
                // Scroll from top to bottom at constant speed; every frame waits for the GPU.
                std::vector<double> times;
                times.reserve(static_cast<std::size_t>(bench_frames));
                for (int i = 0; i < bench_frames; ++i) {
                    bench_scroller->jump_to(bench_extent * static_cast<float>(i) /
                                            static_cast<float>(bench_frames));
                    const auto start = std::chrono::steady_clock::now();
                    frame();
                    glFinish();
                    times.push_back(std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
                }
                std::ranges::sort(times);
                const auto at = [&](double q) {
                    return times[static_cast<std::size_t>(q *
                                                          static_cast<double>(times.size() - 1))];
                };
                std::printf(
                    "%zu entries, %d frames at %dx%d: median %.2f ms, p95 %.2f ms, "
                    "max %.2f ms per frame\n",
                    bench_entries, bench_frames, buffer_width, buffer_height, at(0.5), at(0.95),
                    times.back());
                bench_scroller->jump_to(0.0F);
            }
            if (columns && walk_frames > 0) {
                // The cursor walks down a frame at a time, as a held key would but faster; the
                // loop runs about 1 ms between frames so that workers' results arrive. Only the
                // frames are timed.
                std::vector<double> times;
                times.reserve(static_cast<std::size_t>(walk_frames));
                ui::KeyEvent down;
                down.keysym = XKB_KEY_Down;
                for (int i = 0; i < walk_frames; ++i) {
                    columns->key(down, views::Browser::Clock::now());
                    run_until(loop, [] { return true; });
                    const auto start = std::chrono::steady_clock::now();
                    frame();
                    glFinish();
                    times.push_back(std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - start)
                                        .count());
                }
                std::ranges::sort(times);
                const auto at = [&](double q) {
                    return times[static_cast<std::size_t>(q *
                                                          static_cast<double>(times.size() - 1))];
                };
                std::printf(
                    "walked %d entries at %dx%d: median %.2f ms, p95 %.2f ms, max %.2f ms "
                    "per frame\n",
                    walk_frames, buffer_width, buffer_height, at(0.5), at(0.95), times.back());
            }
            frame();
            glReadPixels(0, 0, buffer_width, buffer_height, GL_RGBA, GL_UNSIGNED_BYTE,
                         pixels.data());
            if (memory) {
                if (columns) {
                    run_until(loop, [&] { return !columns->loading(); });
                    frame();  // what arrived is uploaded
                    glFinish();
                }
                if (trim) {
                    const auto start = std::chrono::steady_clock::now();
                    malloc_trim(0);
                    std::printf("memory: malloc_trim(0) took %.2f ms\n",
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - start)
                                    .count());
                }
                for (const std::string& line :
                     rummage::core::describe(rummage::core::read_memory_report())) {
                    std::printf("memory: %s\n", line.c_str());
                }
                for (const std::string& line :
                     columns ? columns->memory_lines() : std::vector<std::string>{}) {
                    std::printf("memory: %s\n", line.c_str());
                }
            }
        }
        write_ppm(out, buffer_width, buffer_height, pixels);
        log::info("wrote {} ({}x{} at scale {})", out, buffer_width, buffer_height, scale);
        return 0;
    } catch (const std::exception& e) {
        log::error("{}", e.what());
        return 1;
    }
}
