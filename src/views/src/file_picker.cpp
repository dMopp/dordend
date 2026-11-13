#include "rummage/views/file_picker.hpp"
#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <fnmatch.h>
#include <set>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace rummage::views {
namespace {
using Mode = core::PickerRequest::Mode;

bool valid_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
           name.find('\0') == std::string::npos;
}
}  // namespace

bool picker_matches(const core::PickerFilter& filter, const fs::Entry& entry,
                    const fs::MimeDatabase& mime) {
    if (entry.is_directory()) {
        return true;
    }
    return std::ranges::any_of(filter.patterns, [&](const auto& pattern) {
        if (pattern.kind == 0) {
            return fnmatch(pattern.value.c_str(), entry.name.c_str(), 0) == 0;
        }
        const auto type =
            entry.mime == fs::kUnknownMime ? mime.by_name(entry.name).mime : entry.mime;
        const auto name = mime.name(type);
        if (pattern.value.ends_with("/*")) {
            return name.starts_with(
                std::string_view{pattern.value}.substr(0, pattern.value.size() - 1));
        }
        const auto parent = mime.find(pattern.value);
        return parent != fs::kUnknownMime && mime.is_a(type, parent);
    });
}

PickerSelection picker_selection(const core::PickerRequest& request,
                                 const std::filesystem::path& folder,
                                 const std::vector<std::string>& selected,
                                 const std::string& typed) {
    PickerSelection out;
    const auto fail = [&](std::string error) {
        out.paths.clear();
        out.error = std::string{i18n::tr(error)};
        return out;
    };
    if (!folder.is_absolute() || folder == fs::trash_path()) {
        return fail("Choose a folder on disk.");
    }
    std::vector<std::filesystem::path> paths;
    if (request.mode == Mode::save) {
        if (typed.empty()) {
            return fail("Enter a filename to save.");
        }
        if (!valid_name(typed)) {
            return fail("Use a filename other than “.” or “..”, without slashes.");
        }
        paths.push_back(folder / typed);
    } else if (request.mode == Mode::save_files) {
        if (request.files.empty()) {
            return fail("No filenames were supplied.");
        }
        const auto target = selected.size() == 1 ? folder / selected.front() : folder;
        if (selected.size() > 1) {
            return fail("Choose one destination folder.");
        }
        for (const auto& name : request.files) {
            if (!valid_name(name)) {
                return fail("The application supplied an invalid filename.");
            }
            paths.push_back(target / name);
        }
    } else {
        if (!typed.empty()) {
            paths.push_back(folder / typed);
        } else {
            for (const auto& name : selected) {
                paths.push_back(folder / name);
            }
        }
        if (paths.empty() && request.directory) {
            paths.push_back(folder);
        }
        if (paths.empty()) {
            return fail("Select a file first.");
        }
        if (!request.multiple && paths.size() != 1) {
            return fail(request.directory ? "Select just one folder." : "Select just one file.");
        }
    }
    for (auto path : paths) {
        path = path.lexically_normal();
        std::error_code ec;
        const auto status = std::filesystem::status(path, ec);
        const bool exists = std::filesystem::exists(status);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return fail(i18n::trf("Cannot access “{}”: {}.", path.string(), ec.message()));
        }
        if (request.mode == Mode::open) {
            if (!exists) {
                const auto link = std::filesystem::symlink_status(path, ec);
                if (std::filesystem::is_symlink(link)) {
                    return fail(
                        i18n::trf("“{}” is a broken symbolic link.", path.filename().string()));
                }
                if (!typed.empty()) {
                    return fail(
                        request.directory
                            ? i18n::trf(
                                  "Folder “{}” does not exist. Use New Folder to create it first.",
                                  typed)
                            : i18n::trf("File “{}” does not exist. Check the name or choose an "
                                        "existing file.",
                                        typed));
                }
                return fail(
                    request.directory
                        ? i18n::trf("The selected folder “{}” no longer exists. Choose another.",
                                    path.filename().string())
                        : i18n::trf("The selected file “{}” no longer exists. Choose another.",
                                    path.filename().string()));
            }
            if (request.directory != std::filesystem::is_directory(status)) {
                return fail(request.directory
                                ? i18n::trf("“{}” is not a folder. Choose a folder.",
                                            path.filename().string())
                                : i18n::trf("“{}” is a folder. Open it to choose a file.",
                                            path.filename().string()));
            }
        } else {
            if (!std::filesystem::is_directory(path.parent_path(), ec)) {
                return fail("The destination folder does not exist.");
            }
            if (access(path.parent_path().c_str(), W_OK | X_OK) != 0) {
                return fail("The destination folder is not writable.");
            }
            if (exists && !std::filesystem::is_regular_file(status)) {
                return fail("The destination is not a regular file.");
            }
            // lstat catches a dangling symlink too: it must never silently become a new file.
            const auto link = std::filesystem::symlink_status(path, ec);
            if (std::filesystem::is_symlink(link) && !exists) {
                return fail("The destination is a broken symbolic link.");
            }
            if (exists && access(path.c_str(), W_OK) != 0) {
                return fail("The destination file is not writable.");
            }
            out.overwrite = out.overwrite || exists;
        }
        out.paths.push_back(path.string());
    }
    return out;
}

FilePicker::FilePicker(Browser& browser, core::PickerRequest request,
                       std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
                       Done done)
    : browser_{browser},
      request_{std::move(request)},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      done_{std::move(done)},
      name_{request_.name},
      menu_{redraw_},
      confirm_{redraw_} {
    browser_.set_panes_enabled(false);
    focus_ = request_.mode == Mode::save ? 1 : 0;
    if (focus_ == 1) {
        name_.select_all();
    }
    for (auto& choice : request_.choices) {
        if (choice.options.empty()) {
            if (choice.selected != "true") {
                choice.selected = "false";
            }
        } else if (std::ranges::none_of(choice.options,
                                        [&](const auto& o) { return o.id == choice.selected; })) {
            choice.selected = choice.options.front().id;
        }
    }
    set_filter(request_.filter);
    browser_.set_open_handler([this](const auto&, const auto& files) {
        if (request_.mode == Mode::save && !files.empty()) {
            name_.set_text(files.front().name);
        }
        if (request_.mode == Mode::open) {
            name_.set_text({});
        }
        accept();
    });
    sync_selection();
}

FilePicker::~FilePicker() {
    browser_.set_open_handler({});
    browser_.set_entry_filter({});
}

void FilePicker::set_filter(std::uint32_t index) {
    if (index >= request_.filters.size() && !request_.filters.empty()) {
        return;
    }
    request_.filter = index;
    const auto filter =
        request_.filters.empty() ? std::optional<core::PickerFilter>{} : request_.filters[index];
    const bool directories = request_.directory || request_.mode == Mode::save_files;
    browser_.set_entry_filter([filter, directories, mime = mime_](const fs::Entry& entry) {
        return entry.is_directory() ||
               (!directories && (!filter || picker_matches(*filter, entry, *mime)));
    });
    redraw_();
}

void FilePicker::finish(std::vector<std::string> paths) {
    if (!done_) {
        return;
    }
    core::PickerResult result;
    result.response = 0;
    result.paths = std::move(paths);
    result.filter = request_.filter;
    for (const auto& choice : request_.choices) {
        result.choices.push_back(choice.selected);
    }
    auto done = std::move(done_);
    done(std::move(result));
}

void FilePicker::accept() {
    sync_selection();
    const auto folder = browser_.picker_folder();
    if (!folder || folder->model().loading()) {
        return;
    }
    auto selected = folder->selected_names();
    if (selected.empty() && folder->cursor_entry()) {
        selected.push_back(folder->cursor_entry()->name);
    }
    const auto result = picker_selection(request_, folder->path(), selected, name_.text());
    error_ = result.error;
    if (error_.empty()) {
        if (result.overwrite) {
            confirm_.open(
                result.paths.size() == 1
                    ? i18n::trf("Replace “{}”?",
                                std::filesystem::path{result.paths.front()}.filename().string())
                    : std::string{i18n::tr("Replace existing files?")},
                folder->path().string(), std::string{i18n::tr("Replace")},
                [this, paths = result.paths](bool yes) {
                    if (yes) {
                        finish(paths);
                    }
                },
                std::string{i18n::tr("The application will overwrite the selected file when it saves.")});
        } else {
            finish(result.paths);
        }
    }
    redraw_();
}

void FilePicker::cancel() {
    if (done_) {
        auto done = std::move(done_);
        done(core::PickerResult{});
    }
}

void FilePicker::sync_selection() {
    const auto folder = browser_.picker_folder();
    if (!folder) {
        return;
    }
    std::vector<std::string> selection{folder->path().string()};
    const auto names = folder->selected_names();
    selection.insert(selection.end(), names.begin(), names.end());
    if (selection == last_selection_) {
        return;
    }
    last_selection_ = std::move(selection);
    error_.clear();
    if (request_.mode == Mode::open) {
        name_.set_text({});
    }
    if (request_.mode == Mode::save && names.size() == 1) {
        const auto* entry = folder->cursor_entry();
        if (entry && !entry->is_directory() && entry->name == names.front()) {
            name_.set_text(entry->name);
        }
    }
}

ui::CursorShape FilePicker::cursor_at(float x, float y) const {
    if (confirm_.is_open()) {
        return confirm_.clickable_at(x, y) ? ui::CursorShape::pointer : ui::CursorShape::standard;
    }
    if (!footer_.contains(x, y)) {
        return browser_.cursor_at(x, y);
    }
    if (controls_[1].contains(x, y)) {
        return ui::CursorShape::text;
    }
    for (std::size_t i = 2; i < controls_.size(); ++i) {
        if (controls_[i].contains(x, y)) {
            return ui::CursorShape::pointer;
        }
    }
    return ui::CursorShape::standard;
}

std::string FilePicker::copy_name(bool cut) {
    const auto [first, last] = name_.selection();
    auto text = name_.text().substr(first, last - first);
    if (cut && first != last) {
        name_.insert({});
        error_.clear();
        redraw_();
    }
    return text;
}

void FilePicker::paste(std::string_view text) {
    const auto end = text.find_first_of("\r\n");
    name_.insert(text.substr(0, end));
    error_.clear();
    redraw_();
}

void FilePicker::filters_menu(bool keyboard) {
    std::vector<MenuItem> items;
    for (std::size_t i = 0; i < request_.filters.size(); ++i) {
        MenuItem item;
        item.label = request_.filters[i].label;
        item.radio = true;
        item.checked = i == request_.filter;
        item.action = [this, i] { set_filter(static_cast<std::uint32_t>(i)); };
        items.push_back(std::move(item));
    }
    menu_.open(controls_[2].x, controls_[2].y, std::move(items), keyboard);
}

void FilePicker::choices_menu(bool keyboard) {
    std::vector<MenuItem> items;
    for (std::size_t i = 0; i < request_.choices.size(); ++i) {
        auto& choice = request_.choices[i];
        MenuItem item;
        item.label = choice.label;
        if (choice.options.empty()) {
            item.radio = true;
            item.checked = choice.selected == "true";
            item.action = [this, i] {
                auto& value = request_.choices[i].selected;
                value = value == "true" ? "false" : "true";
                redraw_();
            };
        } else {
            item.submenu = true;
            for (const auto& option : choice.options) {
                MenuItem child;
                child.label = option.label;
                child.radio = true;
                child.checked = option.id == choice.selected;
                child.action = [this, i, value = option.id] {
                    request_.choices[i].selected = value;
                    redraw_();
                };
                item.children.push_back(std::move(child));
            }
        }
        items.push_back(std::move(item));
    }
    menu_.open(controls_[3].x, controls_[3].y, std::move(items), keyboard);
}

bool FilePicker::key(const ui::KeyEvent& event) {
    if (confirm_.key(event) || menu_.key(event)) {
        return true;
    }
    const auto command = command_for(event);
    if (!browser_.has_overlay() &&
        (command == Command::filter_names || command == Command::new_folder)) {
        focus_ = 0;
        return browser_.key(event, Browser::Clock::now());
    }
    // The browser's live filter owns text editing and Escape while it has focus.
    if (focus_ == 0 && browser_.filtering() && !browser_.has_overlay()) {
        if (event.keysym != XKB_KEY_Tab && event.keysym != XKB_KEY_ISO_Left_Tab) {
            return browser_.key(event, Browser::Clock::now());
        }
        browser_.blur_filter();
    }
    if (event.keysym == XKB_KEY_Escape) {
        if (browser_.has_overlay()) {
            browser_.key(event, Browser::Clock::now());
        } else {
            cancel();
        }
        return true;
    }
    if (browser_.has_overlay()) {
        return browser_.key(event, Browser::Clock::now());
    }
    if ((event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) &&
        !event.modifiers.ctrl && !event.modifiers.alt) {
        const int step = event.modifiers.shift ? -1 : 1;
        do {
            focus_ = (focus_ + step + 6) % 6;
        } while ((focus_ == 1 && request_.mode == Mode::save_files) ||
                 (focus_ == 2 && request_.filters.empty()) ||
                 (focus_ == 3 && request_.choices.empty()));
        redraw_();
        return true;
    }
    const bool enter = event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter;
    if (enter && event.modifiers.ctrl) {
        accept();
        return true;
    }
    if (enter && focus_ != 0) {
        if (focus_ == 2) {
            filters_menu(true);
        } else if (focus_ == 3) {
            choices_menu(true);
        } else if (focus_ == 4) {
            cancel();
        } else {
            accept();
        }
        return true;
    }
    if (focus_ == 1) {
        if (name_.key(event)) {
            error_.clear();
        }
        redraw_();
        return true;
    }
    if (focus_ == 0) {
        const bool handled = browser_.key(event, Browser::Clock::now());
        sync_selection();
        return handled;
    }
    return true;
}

void FilePicker::pointer(const ui::PointerEvent& event) {
    if (confirm_.is_open()) {
        confirm_.pointer(event);
        return;
    }
    if (menu_.pointer(event)) {
        return;
    }
    if (browser_.has_overlay()) {
        browser_.pointer(event, Browser::Clock::now());
        return;
    }
    using Kind = ui::PointerEvent::Kind;
    const bool footer = footer_.contains(event.x, event.y);
    if (event.kind == Kind::press && event.button == ui::PointerButton::left) {
        if (!footer) {
            focus_ = 0;
            browser_pressed_ = true;
        } else {
            browser_.blur_filter();
            browser_.pointer({.kind = Kind::leave}, Browser::Clock::now());
            for (std::size_t i = 1; i < controls_.size(); ++i) {
                if (!controls_[i].contains(event.x, event.y)) {
                    continue;
                }
                focus_ = static_cast<int>(i);
                if (i == 1) {
                    name_.click(event.x, false);
                }
                if (i == 2) {
                    filters_menu(false);
                }
                if (i == 3) {
                    choices_menu(false);
                }
                if (i == 4) {
                    cancel();
                }
                if (i == 5) {
                    accept();
                }
                redraw_();
                return;
            }
        }
    }
    if (!footer || browser_pressed_ || event.kind == Kind::leave) {
        browser_.pointer(event, Browser::Clock::now());
        sync_selection();
    }
    if (event.kind == Kind::release || event.kind == Kind::leave) {
        browser_pressed_ = false;
    }
}

void FilePicker::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                      ui::IconSet& icons) {
    const float height = request_.mode == Mode::save_files ? 88.0F : 128.0F;
    ui::Rect content = bounds;
    content.height = std::max(0.0F, content.height - height);
    browser_.draw(renderer, content, palette, icons, Browser::Clock::now());
    footer_ = {bounds.x, content.bottom(), bounds.width, height};
    renderer.fill_background(footer_, palette.surface_container);
    controls_.fill({});
    float y = footer_.y + 10.0F;
    if (request_.mode != Mode::save_files) {
        renderer.draw_text(i18n::tr(name_label()), bounds.x + 12, y + 7, 13, palette.on_surface);
        controls_[1] = {bounds.x + 64, y, std::max(1.0F, bounds.width - 76), 32};
        name_.draw(renderer, controls_[1], 14,
                   {.background = palette.surface,
                    .border = focus_ == 1 ? palette.primary : palette.outline_variant,
                    .text = palette.on_surface,
                    .selection = palette.primary_container,
                    .caret = palette.primary});
        y += 40;
    }
    const auto button = [&](std::size_t i, const std::string& text, ui::Rect rect, bool primary) {
        controls_[i] = rect;
        renderer.draw_box(rect,
                          {.fill = primary ? palette.primary : palette.surface_container_highest,
                           .radius = {},
                           .border_width = focus_ == static_cast<int>(i) ? 2.0F : 0.0F,
                           .border = palette.outline});
        renderer.draw_text(text, rect.x + 10, rect.y + 8, 13,
                           primary ? palette.on_primary : palette.on_surface, rect.width - 20);
    };
    const float right = bounds.right() - 12;
    button(5,
           request_.accept_label.empty()
               ? std::string{request_.mode == Mode::open ? i18n::tr("Select") : i18n::tr("Save")}
               : request_.accept_label,
           {right - 108, y, 108, 32}, true);
    button(4, std::string{i18n::tr("Cancel")}, {right - 212, y, 96, 32}, false);
    float x = bounds.x + 12;
    if (!request_.choices.empty()) {
        button(3, std::string{i18n::tr("Options")}, {x, y, 86, 32}, false);
        x += 94;
    }
    if (!request_.filters.empty()) {
        button(2, request_.filters[request_.filter].label,
               {x, y, std::max(24.0F, right - 224 - x), 32}, false);
    }
    renderer.draw_text(error_.empty() ? std::string{i18n::tr("Ctrl+Enter selects · Esc cancels")} : error_, bounds.x + 12,
                       footer_.bottom() - 27, 13,
                       error_.empty() ? palette.on_surface_variant : palette.error,
                       bounds.width - 24);
    menu_.draw(renderer, bounds, palette);
    confirm_.draw(renderer, bounds, palette);
}
}  // namespace rummage::views
