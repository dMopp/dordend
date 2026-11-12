#include "nordend/views/properties_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <sys/stat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "nordend/core/i18n.hpp"
#include "nordend/fs/directory.hpp"
#include "nordend/fs/folder_icon.hpp"
#include "nordend/fs/icon.hpp"
#include "nordend/views/format.hpp"

namespace nordend::views {
namespace {
std::string lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
}  // namespace

PropertiesDialog::PropertiesDialog(core::WorkerPool& pool,
                                   std::shared_ptr<const fs::MimeDatabase> mime,
                                   std::function<void()> redraw)
    : pool_{pool}, mime_{std::move(mime)}, redraw_{std::move(redraw)} {}

PropertiesDialog::~PropertiesDialog() {
    load_job_.cancel();
    save_job_.cancel();
    icons_job_.cancel();
}

void PropertiesDialog::open(std::filesystem::path path, std::function<void()> changed,
                            bool editable) {
    if (saving_)
        return;
    ++generation_;
    scroll_ = 0;
    load_job_.cancel();
    path_ = std::move(path);
    if (!path_.has_filename() && path_ != path_.root_path())
        path_ = path_.parent_path();
    changed_ = std::move(changed);
    open_ = loading_ = true;
    picking_ = false;
    editable_ = editable;
    data_ = {};
    draft_.clear();
    original_.clear();
    error_.clear();
    controls_.clear();
    focus_ = "close";
    load_job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, mime = mime_](const core::CancelToken& token) {
            Data result;
            const auto parent = path == path.root_path() ? path : path.parent_path();
            const auto name = path == path.root_path() ? "." : path.filename().string();
            const auto entries = fs::read_entries(parent, {name}, {.mime = mime});
            if (!entries.empty() && entries[0])
                result.entry = *entries[0];
            if (result.entry.kind == fs::EntryKind::file && result.entry.mime == fs::kUnknownMime)
                result.entry.mime = mime->detect(parent, result.entry);
            result.info = preview::read_info(path, token);
            if (result.entry.is_directory()) {
                const auto setting = fs::read_folder_icon_setting(path);
                result.icon = setting.icon;
                result.icon_error = setting.error;
                result.default_icon = fs::FolderIcons{}.default_for_folder(path);
            }
            struct stat info{};
            if (::stat(path.c_str(), &info) == 0) {
                result.device = info.st_dev;
                result.inode = info.st_ino;
            }
            return result;
        },
        [this](Data data) {
            data_ = std::move(data);
            original_ = draft_ = data_.icon;
            error_ = data_.info.error;
            loading_ = false;
            focus_ = can_edit_icon() ? "change" : "close";
            redraw_();
        });
    redraw_();
}

void PropertiesDialog::close() {
    if (saving_)
        return;
    load_job_.cancel();
    open_ = picking_ = loading_ = false;
    controls_.clear();
    redraw_();
}

bool PropertiesDialog::can_edit_icon() const {
    return open_ && !loading_ && !saving_ && editable_ && data_.entry.is_directory() &&
           data_.info.error.empty() && data_.icon_error.empty();
}

void PropertiesDialog::choose_icon(std::string icon) {
    if (!can_edit_icon())
        return;
    draft_ = std::move(icon);
    picking_ = false;
    scroll_ = 0;
    error_.clear();
    focus_ = "apply";
    redraw_();
}

void PropertiesDialog::pick_icon() {
    if (!can_edit_icon())
        return;
    picking_ = true;
    scroll_ = 0;
    search_.set_text({});
    custom_.set_text(draft_);
    focus_ = "search";
    error_.clear();
    filter();
    redraw_();
}

void PropertiesDialog::apply() {
    if (!can_edit_icon() || !dirty())
        return;
    saving_ = true;
    error_.clear();
    save_job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, icon = draft_, device = data_.device,
         inode = data_.inode](const core::CancelToken&) {
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0 || info.st_dev != device || info.st_ino != inode)
                return std::string{"The folder was removed or replaced. Reopen Properties."};
            if (!icon.empty()) {
                // Reject unsupported paths without ever writing a setting that cannot be read.
                if (icon.find('\0') != std::string::npos || icon.size() > 4096)
                    return std::string{"Invalid icon name or path"};
                if (icon.find('/') != std::string::npos && !icon.starts_with('/') &&
                    !icon.starts_with("./"))
                    return std::string{"Use an absolute image path or a path beginning with ./"};
                if (icon.find('/') != std::string::npos) {
                    const auto file =
                        icon.starts_with("./") ? path / icon : std::filesystem::path{icon};
                    if ((file.extension() != ".svg" && file.extension() != ".png") ||
                        ui::IconTheme::render(file, 48).empty())
                        return std::string{"Choose a readable SVG or PNG image"};
                }
            }
            return fs::save_folder_icon(path, icon);
        },
        [this](std::string error) {
            saving_ = false;
            error_ = std::move(error);
            if (error_.empty()) {
                original_ = draft_;
                auto changed = changed_;
                close();
                if (changed)
                    changed();
            } else
                redraw_();
        });
    redraw_();
}

void PropertiesDialog::load_catalogue(std::string theme) {
    if (catalogue_theme_ && theme == *catalogue_theme_)
        return;
    icons_job_.cancel();
    catalogue_theme_ = theme;
    catalogue_.clear();
    filter();
    icons_loading_ = true;
    icons_job_ = pool_.submit(
        core::Priority::visible,
        [theme = std::move(theme)](const core::CancelToken&) {
            const auto resolved =
                theme.empty() || theme == "builtin" ? ui::desktop_icon_theme() : theme;
            return std::pair{resolved,
                             ui::IconTheme{resolved, ui::icon_theme_roots()}.folder_icons()};
        },
        [this](std::pair<std::string, std::vector<std::string>> found) {
            resolved_theme_ = std::move(found.first);
            catalogue_ = std::move(found.second);
            icons_loading_ = false;
            filter();
            redraw_();
        });
}

void PropertiesDialog::filter() {
    filtered_.clear();
    const auto query = lower(search_.text());
    for (const auto& icon : catalogue_)
        if (lower(icon).find(query) != std::string::npos)
            filtered_.push_back(icon);
    selected_ = page_ = 0;
    if (const auto it = std::ranges::find(filtered_, draft_); it != filtered_.end()) {
        selected_ = static_cast<std::size_t>(it - filtered_.begin());
        page_ = selected_ / kPageSize;
    }
}

std::vector<std::string> PropertiesDialog::focus_order() const {
    if (picking_)
        return {"search", "grid", "custom", "back", "use"};
    if (can_edit_icon())
        return {"change", "default", "close", "apply"};
    return {"close"};
}

void PropertiesDialog::accept_custom() {
    if (!custom_.text().empty())
        choose_icon(custom_.text());
}

void PropertiesDialog::activate(std::string_view id) {
    if (id == "close")
        close();
    else if (id == "apply")
        apply();
    else if (id == "change")
        pick_icon();
    else if (id == "default")
        choose_icon({});
    else if (id == "back") {
        picking_ = false;
        scroll_ = 0;
        focus_ = "change";
        error_.clear();
        redraw_();
    } else if (id == "use")
        accept_custom();
    else if (id == "previous" && page_ > 0) {
        --page_;
        selected_ = page_ * kPageSize;
        redraw_();
    } else if (id == "next" && (page_ + 1) * kPageSize < filtered_.size()) {
        ++page_;
        selected_ = page_ * kPageSize;
        redraw_();
    } else if (id.starts_with("icon:"))
        choose_icon(std::string{id.substr(5)});
}

bool PropertiesDialog::key(const ui::KeyEvent& event) {
    if (!open_)
        return false;
    if (saving_)
        return true;
    if (event.keysym == XKB_KEY_Escape) {
        if (picking_)
            activate("back");
        else
            close();
        return true;
    }
    if (event.keysym == XKB_KEY_Tab || event.keysym == XKB_KEY_ISO_Left_Tab) {
        const auto order = focus_order();
        const auto it = std::ranges::find(order, focus_);
        const auto at = it == order.end() ? 0 : static_cast<std::size_t>(it - order.begin());
        const bool backwards = event.modifiers.shift || event.keysym == XKB_KEY_ISO_Left_Tab;
        focus_ = order[(at + (backwards ? order.size() - 1 : 1)) % order.size()];
        ui::Rect rect{};
        if (focus_ == "search")
            rect = search_.bounds();
        else if (focus_ == "custom")
            rect = custom_.bounds();
        else if (focus_ == "grid")
            rect = grid_;
        else
            for (const auto& control : controls_)
                if (control.id == focus_)
                    rect = control.rect;
        if (focus_ == "change" || focus_ == "default" || focus_ == "search" || focus_ == "custom" ||
            focus_ == "grid") {
            if (rect.y < content_.y)
                scroll_ = std::max(0.0F, scroll_ + rect.y - content_.y);
            else if (rect.bottom() > content_.bottom())
                scroll_ = std::min(max_scroll_, scroll_ + rect.bottom() - content_.bottom());
        }
        redraw_();
        return true;
    }
    if (picking_ && (focus_ == "grid" || focus_ == "search") && !filtered_.empty()) {
        int step = 0;
        switch (event.keysym) {
            case XKB_KEY_Down:
                step = 5;
                break;
            case XKB_KEY_Up:
                step = -5;
                break;
            case XKB_KEY_Right:
                if (focus_ == "grid")
                    step = 1;
                break;
            case XKB_KEY_Left:
                if (focus_ == "grid")
                    step = -1;
                break;
            case XKB_KEY_Page_Down:
                step = static_cast<int>(kPageSize);
                break;
            case XKB_KEY_Page_Up:
                step = -static_cast<int>(kPageSize);
                break;
            default:
                break;
        }
        if (step != 0) {
            focus_ = "grid";
            selected_ = static_cast<std::size_t>(std::clamp(
                static_cast<int>(selected_) + step, 0, static_cast<int>(filtered_.size()) - 1));
            page_ = selected_ / kPageSize;
            redraw_();
            return true;
        }
    }
    if (event.keysym == XKB_KEY_Return || event.keysym == XKB_KEY_KP_Enter ||
        (event.keysym == XKB_KEY_space && focus_ != "search" && focus_ != "custom")) {
        if (picking_ && (focus_ == "grid" || focus_ == "search")) {
            if (!filtered_.empty())
                choose_icon(filtered_[selected_]);
        } else if (focus_ == "custom")
            accept_custom();
        else
            activate(focus_);
        return true;
    }
    if (picking_ && focus_ == "search" && search_.key(event)) {
        filter();
        redraw_();
    } else if (picking_ && focus_ == "custom" && custom_.key(event))
        redraw_();
    return true;
}

void PropertiesDialog::paste(std::string_view text) {
    if (!open_ || !picking_ || saving_)
        return;
    text = text.substr(0, text.find_first_of("\r\n"));
    if (focus_ == "search") {
        search_.insert(text);
        filter();
    } else if (focus_ == "custom")
        custom_.insert(text);
    redraw_();
}

void PropertiesDialog::pointer(const ui::PointerEvent& event) {
    if (!open_ || saving_)
        return;
    if (event.kind == ui::PointerEvent::Kind::scroll && max_scroll_ > 0 &&
        content_.contains(event.x, event.y)) {
        scroll_ = std::clamp(scroll_ + event.wheel_y, 0.0F, max_scroll_);
        redraw_();
        return;
    }
    if (event.kind == ui::PointerEvent::Kind::scroll && picking_ &&
        grid_.contains(event.x, event.y)) {
        activate(event.wheel_y > 0 ? "next" : "previous");
        return;
    }
    if (event.kind != ui::PointerEvent::Kind::press || event.button != ui::PointerButton::left)
        return;
    if (picking_ && content_.contains(event.x, event.y) &&
        search_.bounds().contains(event.x, event.y)) {
        focus_ = "search";
        search_.click(event.x, event.modifiers.shift);
        redraw_();
        return;
    }
    if (picking_ && content_.contains(event.x, event.y) &&
        custom_.bounds().contains(event.x, event.y)) {
        focus_ = "custom";
        custom_.click(event.x, event.modifiers.shift);
        redraw_();
        return;
    }
    for (const auto& control : controls_) {
        const bool footer = control.id == "close" || control.id == "apply" ||
                            control.id == "back" || control.id == "use";
        if (!footer && !content_.contains(event.x, event.y))
            continue;
        if (control.enabled && control.rect.contains(event.x, event.y)) {
            const auto id = control.id;
            focus_ = id;
            activate(id);
            return;
        }
    }
}

ui::CursorShape PropertiesDialog::cursor_at(float x, float y) const {
    if (saving_)
        return ui::CursorShape::standard;
    if (picking_ && content_.contains(x, y) &&
        (search_.bounds().contains(x, y) || custom_.bounds().contains(x, y)))
        return ui::CursorShape::text;
    for (const auto& control : controls_) {
        const bool footer = control.id == "close" || control.id == "apply" ||
                            control.id == "back" || control.id == "use";
        if (!footer && !content_.contains(x, y))
            continue;
        if (control.enabled && control.rect.contains(x, y))
            return ui::CursorShape::pointer;
    }
    return ui::CursorShape::standard;
}

void PropertiesDialog::draw(ui::Renderer& renderer, const ui::Rect& bounds,
                            const theme::Palette& palette, ui::IconSet& icons) {
    if (!open_)
        return;
    controls_.clear();
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.55F));
    const float width = std::min(640.0F, bounds.width - 24.0F);
    const float height = std::min(picking_ ? 550.0F : 520.0F, bounds.height - 24.0F);
    const ui::Rect card{bounds.x + (bounds.width - width) / 2,
                        bounds.y + (bounds.height - height) / 2, width, height};
    renderer.draw_box(card, {.fill = palette.surface_container_high,
                             .radius = ui::Corners::all(14),
                             .border_width = 1,
                             .border = palette.outline_variant});
    content_ = {card.x, card.y, card.width, card.height - 70};
    const float natural_height = picking_ ? 550.0F : 520.0F;
    max_scroll_ = std::max(0.0F, natural_height - height);
    scroll_ = std::clamp(scroll_, 0.0F, max_scroll_);
    const float top = card.y - scroll_;
    renderer.push_clip(content_);
    const float left = card.x + 24, room = width - 48;
    const auto text = [&](std::string_view value, float x, float y, float size, float space,
                          bool dim = false) {
        renderer.draw_text(value, x, y, size, dim ? palette.on_surface_variant : palette.on_surface,
                           space, ui::Ellipsis::middle);
    };
    const auto button = [&](std::string id, std::string label, ui::Rect rect, bool enabled = true,
                            bool primary = false) {
        const auto fill = primary && enabled ? palette.primary : palette.surface_container_highest;
        renderer.draw_box(rect, {.fill = fill,
                                 .radius = ui::Corners::all(7),
                                 .border_width = focus_ == id ? 2.0F : 0.0F,
                                 .border = palette.primary});
        renderer.draw_text(i18n::tr(label), rect.x + 12, rect.y + 7, 13,
                           (primary && enabled ? palette.on_primary : palette.on_surface)
                               .with_alpha(enabled ? 1.0F : 0.4F),
                           rect.width - 20);
        controls_.push_back({std::move(id), rect, enabled});
    };
    const float bottom = top + natural_height - 56;
    text(i18n::tr(picking_ ? "Choose Folder Icon" : "Properties"), left, top + 20, 18, room);
    if (picking_) {
        load_catalogue(icons.selected_theme());
        if (!preview_icons_)
            preview_icons_ = std::make_unique<ui::IconSet>(renderer.fonts());
        preview_icons_->set_theme(resolved_theme_);
        const auto field_colors = [&](bool focused) {
            return ui::FieldColors{palette.surface,
                                   focused ? palette.primary : palette.outline_variant,
                                   palette.on_surface, palette.primary_container,
                                   focused ? palette.primary : ui::Color{}};
        };
        text(i18n::tr("Search folder icons"), left, top + 55, 12, room, true);
        search_.draw(renderer, {left, top + 76, room, 30}, 13, field_colors(focus_ == "search"));
        grid_ = {left, top + 120, room, std::max(90.0F, bottom - (top + 120) - 136)};
        const float cell_width = room / 5;
        const float cell_height = grid_.height / 3;
        if (filtered_.empty())
            text(i18n::tr(icons_loading_ ? "Loading icons…"
                                         : "No matching icons. You can enter an image path below."),
                 left, grid_.y + 12, 13, room, true);
        for (std::size_t index = page_ * kPageSize;
             index < std::min(filtered_.size(), (page_ + 1) * kPageSize); ++index) {
            const auto slot = index % kPageSize;
            const ui::Rect cell{left + static_cast<float>(slot % 5) * cell_width,
                                grid_.y + static_cast<float>(slot / 5) * cell_height,
                                cell_width - 4, cell_height - 4};
            if (index == selected_)
                renderer.draw_box(cell, {.fill = palette.primary_container,
                                         .radius = ui::Corners::all(6),
                                         .border_width = focus_ == "grid" ? 2.0F : 0.0F,
                                         .border = palette.primary});
            const float size = std::min(44.0F, cell_height - 26);
            preview_icons_->draw_file(renderer, "inode/directory", "folder",
                                      cell.x + (cell.width - size) / 2, cell.y + 3, size,
                                      palette.on_surface, filtered_[index]);
            std::string label = filtered_[index];
            if (label.starts_with("folder-"))
                label.erase(0, 7);
            renderer.draw_text(
                label, cell.x + 4, cell.y + size + 6, 11,
                index == selected_ ? palette.on_primary_container : palette.on_surface,
                cell.width - 8, ui::Ellipsis::middle);
            controls_.push_back({"icon:" + filtered_[index], cell, true});
        }
        const float pager = grid_.bottom() + 3;
        button("previous", "Previous", {left, pager, 92, 30}, page_ > 0);
        text(std::format("{} icons · page {} / {}", filtered_.size(), page_ + 1,
                         std::max<std::size_t>(1, (filtered_.size() + kPageSize - 1) / kPageSize)),
             left + 108, pager + 6, 12, room - 210, true);
        button("next", "Next", {card.right() - 106, pager, 82, 30},
               (page_ + 1) * kPageSize < filtered_.size());
        text(i18n::tr("Custom icon name or SVG/PNG path"), left, pager + 42, 12, room, true);
        custom_.draw(renderer, {left, pager + 63, room, 30}, 13, field_colors(focus_ == "custom"));

    } else {
        const auto preferred = draft_.empty() ? data_.default_icon
                               : draft_.starts_with("./")
                                   ? (path_ / draft_).lexically_normal().string()
                                   : draft_;
        icons.draw_file(renderer, mime_->name(data_.entry.mime), fs::icon_name(*mime_, data_.entry),
                        left, top + 60, 56, palette.primary, preferred);
        text(path_.filename().empty() ? path_.string() : path_.filename().string(), left + 76,
             top + 60, 16, room - 76);
        text(loading_ ? std::string{i18n::tr("Loading…")}
                      : std::string{mime_->name(data_.entry.mime)},
             left + 76, top + 87, 12, room - 76, true);
        float y = top + 132;
        const float row_height = 26.0F;
        const auto row = [&](std::string_view label, const std::string& value) {
            text(i18n::tr(label), left, y, 12, 90, true);
            text(value, left + 102, y, 12, room - 102);
            y += row_height;
        };
        row("Location", path_.parent_path().string());
        row(data_.entry.is_directory() ? "Contents" : "Size",
            loading_ ? "…"
            : data_.entry.is_directory()
                ? (data_.info.items ? std::format("{} ({})",
                                                  *data_.info.items == 1
                                                      ? std::string{i18n::tr("1 item")}
                                                      : i18n::trf("{} items", *data_.info.items),
                                                  i18n::trf("{} hidden", data_.info.hidden_items))
                                    : std::string{i18n::tr("Unavailable")})
                : i18n::trf("{} ({} bytes)", format_size(data_.entry.size), data_.entry.size));
        row("Modified", loading_ ? "…" : format_time(data_.entry.modified_ns));
        row("Created", data_.info.created_ns ? format_time(*data_.info.created_ns)
                                             : std::string{i18n::tr("Unavailable")});
        row("Owner", data_.info.owner);
        row("Group", data_.info.group);
        row("Permissions", data_.info.permissions);
        if (!data_.info.link_target.empty())
            row("Link target", data_.info.link_target);
        if (data_.entry.is_directory() && editable_) {
            button("change", "Change Icon…", {left, bottom - 79, 144, 32}, can_edit_icon());
            button("default", "Default", {left + 152, bottom - 79, 92, 32},
                   can_edit_icon() && !draft_.empty());
            text(draft_.empty() ? std::string{i18n::tr("Default folder icon")} : draft_, left + 256,
                 bottom - 72, 12, room - 256, true);
            if (icons.selected_theme() == "builtin")
                text(i18n::tr("Custom icons appear when using a system icon theme."), left,
                     bottom - 40, 12, room, true);
            else if (data_.entry.is_link)
                text(i18n::tr("Changing this icon affects the linked folder."), left, bottom - 40,
                     12, room, true);
        }
        const auto message = !error_.empty() ? error_ : data_.icon_error;
        if (!message.empty())
            renderer.draw_text(i18n::tr(message), left, bottom - 22, 12, palette.error, room,
                               ui::Ellipsis::middle);
    }
    renderer.pop_clip();
    if (max_scroll_ > 0) {
        const float track = content_.height - 24;
        const float thumb = track * content_.height / (natural_height - 70);
        renderer.draw_box(
            {card.right() - 7, card.y + 12 + (track - thumb) * scroll_ / max_scroll_, 3, thumb},
            {.fill = palette.outline, .radius = ui::Corners::all(2)});
    }
    const float footer = card.bottom() - 56;
    if (picking_) {
        button("back", "Back", {card.right() - 246, footer, 94, 34});
        button("use", "Use Custom", {card.right() - 144, footer, 120, 34}, !custom_.text().empty(),
               true);
    } else {
        button(
            "close", dirty() ? "Cancel" : "Close",
            {card.right() - (data_.entry.is_directory() && editable_ ? 232 : 124), footer, 100, 34},
            !saving_);
        if (data_.entry.is_directory() && editable_)
            button("apply", saving_ ? "Saving…" : "Apply", {card.right() - 124, footer, 100, 34},
                   can_edit_icon() && dirty(), true);
    }
}

}  // namespace nordend::views
