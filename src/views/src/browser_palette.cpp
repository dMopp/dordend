// The command palette's items (ROADMAP 8a): what Browser offers through Ctrl+K.

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "nordend/ui/seat.hpp"
#include "nordend/views/browser.hpp"
#include "nordend/views/command_palette.hpp"
#include "nordend/views/file_clipboard.hpp"
#include "nordend/views/shortcuts.hpp"
#include "nordend/core/i18n.hpp"

namespace nordend::views {
namespace {

// The keys shown for a command: its shortcut line's first chord ("Alt+←" of "Alt+←  Alt+→").
std::string first_chord(Command command) {
    std::string keys = keys_for(command);
    if (const std::size_t gap = keys.find("  "); gap != std::string::npos) {
        keys.resize(gap);
    }
    return keys;
}

}  // namespace

void Browser::open_palette() {
    menu_.close();
    shortcuts_.close();
    palette_.open(palette_items(), palette_recent_,
                  std::string{i18n::tr("Type a command, a place or a tab")});
}

std::vector<PaletteItem> Browser::palette_items() {
    std::vector<PaletteItem> items;
    const auto add = [&items](std::string id, std::string label, std::string keys,
                              std::function<void()> action, bool remember = true) {
        items.push_back({.id = std::move(id),
                         .label = std::string{i18n::tr(label)},
                         .keys = std::move(keys),
                         .remember = remember,
                         .action = std::move(action),
                         .path = false,
                         .quick_look = {},
                         .open = {}});
    };
    // A command goes the way its key goes: through run().
    const auto command = [&](Command which, const char* id, std::string label) {
        add(std::string{"command:"} + id, std::move(label), first_chord(which),
            [this, which] { run(which, Clock::now()); });
    };
    // A key the view handles itself (zoom, select all, parent): the same key, sent to it.
    const auto view_command = [&](const char* id, std::string label, std::string keys,
                                  xkb_keysym_t keysym, ui::Modifiers modifiers) {
        add(std::string{"view:"} + id, std::move(label), std::move(keys),
            [this, keysym, modifiers] {
                ui::KeyEvent event;
                event.keysym = keysym;
                event.modifiers = modifiers;
                view_key(event, Clock::now());
            });
    };

    const std::shared_ptr<Folder> folder = current_folder();
    const bool in_trash = folder && folder->is_trash();
    const std::vector<std::string> names = folder ? targets(*folder) : std::vector<std::string>{};
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    const bool editable = folder && !in_trash;
    if (folder && names.size() <= 1 && (!in_trash || !names.empty()))
        command(Command::properties, "properties", "Properties…");

    // Opening and moving.
    if (cursor != nullptr) {
        add("command:open", "Open", "Enter", [this] {
            ui::KeyEvent enter;
            enter.keysym = XKB_KEY_Return;
            view_key(enter, Clock::now());
        });
    }
    if (cursor != nullptr && !in_trash && choices_ && open_with_) {
        add("command:open_with", "Open With…", "", [this] { open_with_palette(); });
    }
    if (folder && !in_trash && !names.empty()) {
        std::vector<std::filesystem::path> folders;
        for (const fs::Entry& entry : folder->entries()) {
            if (entry.is_directory() && std::ranges::find(names, entry.name) != names.end()) {
                folders.push_back(folder->path() / entry.name);
            }
        }
        if (!folders.empty() && folders.size() == names.size()) {
            add("command:open_tab", folders.size() == 1 ? "Open in New Tab" : "Open in New Tabs",
                "", [this, folders] { open_tabs(folders); });
        }
    }
    if (editable && terminal_) {
        command(Command::open_terminal, "open_terminal", "Open in Terminal");
    }
    if (folder && folder->path().has_parent_path() &&
        folder->path() != folder->path().root_path()) {
        view_command("parent", "Go to Parent Folder", "Alt+↑", XKB_KEY_Up, {.alt = true});
    }
    if (pane().here > 0) {
        command(Command::back, "back", "Go Back");
    }
    if (pane().here + 1 < pane().history.size()) {
        add("command:forward", "Go Forward", "Alt+→",
            [this] { run(Command::forward, Clock::now()); });
    }

    // Files.
    if (editable) {
        command(Command::new_folder, "new_folder", "New Folder");
        add("command:new_file", "New Empty File", "", [this] { new_file(std::string{i18n::tr("New File")}, {}); });
    }
    if (!names.empty() && !in_trash) {
        command(Command::copy, "copy", "Copy");
        command(Command::cut, "cut", "Cut");
    }
    bool can_paste = !clipboard_.names.empty();
    if (!can_paste && system_.types) {
        const std::vector<std::string> types = system_.types();
        can_paste = std::ranges::find(types, kGnomeFiles) != types.end() ||
                    std::ranges::find(types, kUriList) != types.end();
    }
    if (editable && can_paste) {
        command(Command::paste, "paste", "Paste");
    }
    if (editable && names.size() == 1) {
        command(Command::rename, "rename", "Rename…");
    }
    if (!names.empty() && !in_trash) {
        command(Command::trash, "trash", "Move to Trash");
    }
    if (!names.empty()) {
        command(Command::erase, "erase", "Delete Permanently…");
    }
    if (!names.empty() && in_trash) {
        add("command:restore", "Restore", "", [this] { restore(); });
    }
    add("command:empty_trash", "Empty Trash…", "", [this] { empty_trash(); });
    if (undo_) {
        command(Command::undo, "undo", "Undo");
    }
    if (redo_) {
        command(Command::redo, "redo", "Redo");
    }
    if (folder) {
        view_command("select_all", "Select All", "Ctrl+A", XKB_KEY_a, {.ctrl = true});
    }
    if (editable) {
        command(Command::add_bookmark, "add_bookmark", "Add to Sidebar");
    }
    command(Command::go_to, "go_to", "Go to Folder or Server…");

    // The same registry entries and captured targets as the context menu.
    for (auto& item : action_items(action_context(folder))) {
        if (item.submenu) {
            for (auto& child : item.children) {
                if (child.enabled) {
                    add(child.id, child.label, child.keys, std::move(child.action));
                }
            }
        } else if (item.enabled) {
            add(item.id, item.label, item.keys, std::move(item.action));
        }
    }

    // Views.
    if (pane().mode != Mode::columns) {
        command(Command::columns_view, "columns_view", "Show Columns");
    }
    if (pane().mode != Mode::grid) {
        command(Command::grid_view, "grid_view", "Show Grid");
    }
    if (pane().mode != Mode::details) {
        command(Command::details_view, "details_view", "Show Details");
    }
    if (cursor != nullptr || quick_look_.is_open()) {
        command(Command::quick_look, "quick_look",
                quick_look_.is_open() ? "Close Quick Look" : "Quick Look");
    }
    if (pane().mode != Mode::columns) {  // the columns always show it
        command(Command::toggle_info, "toggle_info",
                info_visible_ ? "Hide Info Pane" : "Show Info Pane");
    }
    command(Command::toggle_sidebar, "toggle_sidebar",
            sidebar_visible_ ? "Hide Sidebar" : "Show Sidebar");
    command(Command::focus_sidebar, "focus_sidebar",
            sidebar_.focused() ? "Back to the View" : "Move to the Sidebar");
    command(Command::toggle_hidden, "toggle_hidden",
            show_hidden_ ? "Hide Hidden Files" : "Show Hidden Files");
    const fs::SortOrder& order = sort_order();

    struct SortChoice {
        fs::SortKey key;
        const char* id;
        const char* label;
    };

    constexpr std::array<SortChoice, 4> sort_keys{{
        {.key = fs::SortKey::name, .id = "sort:name", .label = "Sort by Name"},
        {.key = fs::SortKey::size, .id = "sort:size", .label = "Sort by Size"},
        {.key = fs::SortKey::modified, .id = "sort:modified", .label = "Sort by Modified"},
        {.key = fs::SortKey::type, .id = "sort:type", .label = "Sort by Type"},
    }};
    for (const SortChoice& choice : sort_keys) {
        if (order.key != choice.key) {
            add(choice.id, choice.label, "",
                [this, key = choice.key] { set_sort(key, sort_order().descending); });
        }
    }
    if (order.descending) {
        add("sort:ascending", "Sort Ascending", "", [this] { set_sort(sort_order().key, false); });
    } else {
        add("sort:descending", "Sort Descending", "", [this] { set_sort(sort_order().key, true); });
    }
    if (folder) {
        view_command("zoom_in", "Zoom In", "Ctrl++", XKB_KEY_plus, {.ctrl = true});
        view_command("zoom_out", "Zoom Out", "Ctrl+−", XKB_KEY_minus, {.ctrl = true});
        view_command("zoom_reset", "Usual Size", "Ctrl+0", XKB_KEY_0, {.ctrl = true});
    }

    // Tabs.
    command(Command::new_tab, "new_tab", "New Tab");
    if (panes_.size() > 1) {
        command(Command::close_tab, "close_tab", "Close Tab");
        command(Command::next_tab, "next_tab", "Next Tab");
        command(Command::previous_tab, "previous_tab", "Previous Tab");
        const std::vector<std::string> titles = tab_titles();
        for (std::size_t i = 0; i < panes_.size(); ++i) {
            if (i == active_) {
                continue;
            }
            add(
                std::format("tab:{}", i), i18n::trf("Tab {}: {}", i + 1, titles[i]),
                i < 9 ? std::format("Alt+{}", i + 1) : std::string{}, [this, i] { select_tab(i); },
                false);
        }
    }

    // Places, as the sidebar lists them.
    for (const fs::Place& place : places_) {
        const std::string where = place.path.empty() ? place.disk.volume : place.path.string();
        add("go:" + where, i18n::trf("Go to {}", place.label), "", [this, place] { open_place(place); });
        if (place.section == fs::Place::Section::devices && place.disk.mounted &&
            !place.disk.volume.empty()) {
            add("eject:" + place.disk.volume,
                place.disk.ejectable ? i18n::trf("Eject {}", place.label)
                                     : i18n::trf("Unmount {}", place.label),
                "", [this, place] {
                    if (place.disk.ejectable) {
                        eject_place(place);
                    } else {
                        unmount_place(place);
                    }
                });
        }
    }

    // Nordend.
    if (folder && !in_trash) {
        command(Command::filter_names, "filter_names", "Filter Names…");
        command(Command::find_files, "find_files", "Find Files…");
    }
    command(Command::show_shortcuts, "show_shortcuts", "Keyboard Shortcuts");
    if (quit_) {
        add("command:quit", "Quit", "Ctrl+Q", [this] { quit_(); });
    }
    return items;
}

void Browser::open_with_palette() {
    const std::shared_ptr<Folder> folder = current_folder();
    const fs::Entry* const cursor = folder ? folder->cursor_entry() : nullptr;
    if (cursor == nullptr || !choices_ || !open_with_) {
        return;
    }
    // What it would open: the selected entries of the cursor's kind, else the cursor's (as the
    // context menu's "Open With").
    const std::vector<std::string> names = targets(*folder);
    std::vector<fs::Entry> files;
    for (const fs::Entry& entry : folder->entries()) {
        if (entry.is_directory() == cursor->is_directory() &&
            std::ranges::find(names, entry.name) != names.end()) {
            fs::Entry typed = entry;
            typed.mime = folder->mime_of(entry);
            files.push_back(std::move(typed));
        }
    }
    fs::Entry typed = *cursor;
    typed.mime = folder->mime_of(*cursor);
    ++menu_loads_;
    choices_(folder->path(), std::move(typed),
             [this, alive = alive_, directory = folder->path(),
              files = std::move(files)](const std::vector<AppChoice>& apps) {
                 if (!*alive) {
                     return;
                 }
                 --menu_loads_;
                 if (palette_.is_open() || menu_.is_open()) {
                     return;  // something else meanwhile
                 }
                 std::vector<PaletteItem> items;
                 items.reserve(apps.size());
                 for (const AppChoice& app : apps) {
                     items.push_back({.id = "open_with:" + app.id,
                                      .label = app.is_default ? app.name + std::string{i18n::tr(" (default)")} : app.name,
                                      .keys = {},
                                      .remember = false,
                                      .action = [this, directory, files,
                                                 id = app.id] { open_with_(directory, files, id); },
                                      .path = false,
                                      .quick_look = {},
                                      .open = {}});
                 }
                 if (items.empty()) {
                     show_message(std::string{i18n::tr("No applications open it")}, false);
                     return;
                 }
                 palette_.open(std::move(items), {}, std::string{i18n::tr("Open with…")});
             });
}

void Browser::open_finder() {
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder) {
        return;
    }
    if (folder->is_trash()) {
        show_message(std::string{i18n::tr("Files are not looked for in the trash")}, false);
        return;
    }
    menu_.close();
    shortcuts_.close();
    palette_.close();
    finder_ = std::make_unique<FileFinder>(pool_, [this] { refresh_found(); });
    const std::filesystem::path root = folder->path();
    std::string name = root.filename().string();
    if (name.empty()) {
        name = root.string();  // "/"
    }
    palette_.open_external(i18n::trf("Find in {}", name), [this](const std::string& query) {
        if (finder_) {
            finder_->set_query(query);
        }
    });
    finder_->start(root, show_hidden_);
    refresh_found();
}

void Browser::refresh_found() {
    if (!finder_ || !palette_.external()) {
        return;
    }
    const std::filesystem::path& root = finder_->root();
    std::vector<PaletteItem> items;
    items.reserve(finder_->results().size());
    for (const FoundFile& found : finder_->results()) {
        const auto then = [this, root, path = found.path](FoundAction after) {
            return [this, root, path, after] { show_found(root, path, after); };
        };
        items.push_back({.id = "file:" + found.path,
                         .label = found.path,
                         .keys = found.directory ? std::string{i18n::tr("Folder")} : std::string{},
                         .remember = false,
                         .action = then(FoundAction::show),
                         .path = true,
                         .quick_look = then(FoundAction::quick_look),
                         .open = then(FoundAction::open)});
    }
    palette_.set_results(std::move(items), finder_->status());
}

void Browser::show_found(const std::filesystem::path& root, const std::string& relative,
                         FoundAction after) {
    const std::filesystem::path target = root / relative;
    const std::filesystem::path parent = target.parent_path();
    quick_look_.close();
    if (pane().mode == Mode::columns) {
        // From the columns' first folder when the file is inside it: the way there stays.
        std::filesystem::path from = root;
        if (pane().columns.column_count() > 0) {
            const std::filesystem::path first = pane().columns.column(0).folder().path();
            const auto [in_first, in_target] = std::ranges::mismatch(first, target);
            if (in_first == first.end()) {
                from = first;
            }
        }
        pane().columns.reveal(from, target);
        redraw_();
    } else {
        open(parent);
        if (const std::shared_ptr<Folder> shown = current_folder()) {
            shown->select_name(target.filename().string());
        }
    }
    note_location();
    found_after_.reset();
    if (after != FoundAction::show) {
        found_after_ =
            FoundAfter{.folder = parent, .name = target.filename().string(), .action = after};
    }
}

void Browser::settle_found(Clock::time_point now) {
    if (!found_after_) {
        return;
    }
    const std::shared_ptr<Folder> folder = current_folder();
    if (!folder || folder->path() != found_after_->folder || folder->cursor_pending()) {
        return;  // still on its way
    }
    const FoundAfter after = *found_after_;
    found_after_.reset();
    const fs::Entry* const cursor = folder->cursor_entry();
    if (cursor == nullptr || cursor->name != after.name) {
        return;  // gone meanwhile
    }
    if (after.action == FoundAction::quick_look) {
        quick_look_.open(folder, now);
    } else if (!open_selection() && cursor->is_directory()) {
        ui::KeyEvent enter;  // a folder: into it
        enter.keysym = XKB_KEY_Return;
        view_key(enter, now);
    }
    redraw_();
}

}  // namespace nordend::views
