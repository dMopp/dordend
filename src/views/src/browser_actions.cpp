#include <algorithm>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {

void Browser::set_context_menu(core::MenuLayout layout) {
    menu_layout_ = std::move(layout);
    menu_.close();
    redraw_();
}

std::vector<MenuItem> Browser::configured_menu(
    std::vector<MenuItem> items, const std::optional<std::vector<core::MenuNode>>& specific) const {
    const auto& layout = specific ? specific : menu_layout_.items;
    return layout ? arrange_menu(items, *layout) : std::move(items);
}

void Browser::set_actions(std::span<const core::Action> custom) {
    actions_ = core::action_registry(custom);
    menu_.close();
    if (!palette_.external()) {
        palette_.close();
    }
    redraw_();
}

core::ActionContext Browser::action_context(const std::shared_ptr<Folder>& folder,
                                            bool background) const {
    core::ActionContext context;
    if (!folder) {
        return context;
    }
    std::error_code error;
    context.directory = std::filesystem::absolute(folder->path(), error);
    if (error) {
        return {};
    }
    context.trash = folder->is_trash();
    if (!background) {
        const auto names = targets(*folder);
        for (const auto& entry : folder->entries()) {
            if (std::ranges::find(names, entry.name) != names.end()) {
                context.targets.push_back(
                    {.path = context.directory / entry.name,
                     .mime = entry.is_directory()
                                 ? "inode/directory"
                                 : std::string{folder->mime().name(folder->mime_of(entry))}});
            }
        }
    }
    return context;
}

std::vector<MenuItem> Browser::action_items(const core::ActionContext& context) {
    std::vector<MenuItem> items;
    MenuItem custom;
    custom.id = "actions";
    custom.label = i18n::tr("Actions");
    custom.submenu = true;
    for (const auto& action : actions_) {
        if (!core::action_matches(action, context) ||
            (action.kind != core::Action::Kind::copy_location && !action_handoff_)) {
            continue;
        }
        MenuItem item;
        item.id = "action:" + action.id;
        item.label = i18n::tr(action.label);
        item.keys = keys_for_action(action.id);
        item.enabled = action.kind == core::Action::Kind::copy_location || !action_available_ ||
                       action_available_(action, context);
        if (!item.enabled) {
            const bool archive = action.kind == core::Action::Kind::extract_here ||
                                 action.kind == core::Action::Kind::extract_to;
            item.label += archive ? std::string{" (setup required)"} : i18n::tr(" (not installed)");
            // Let built-in extraction explain which dependency is missing.
            item.enabled = archive;
        }
        item.action = [this, action, context] { invoke_action(action, context); };
        if (action.placement == core::Action::Placement::menu) {
            items.push_back(std::move(item));
        } else {
            custom.children.push_back(std::move(item));
        }
    }
    if (!custom.children.empty()) {
        items.push_back(std::move(custom));
    }
    return items;
}

void Browser::invoke_action(const core::Action& action, const core::ActionContext& context) {
    if (!core::action_matches(action, context)) {
        return;
    }
    if (action.kind == core::Action::Kind::copy_location) {
        std::string text;
        for (const auto& target : context.targets) {
            if (!text.empty()) {
                text += '\n';
            }
            text += target.path.string();
        }
        copy_text(context.targets.empty() ? context.directory.string() : std::move(text));
    } else if (action_handoff_) {
        if (action.kind == core::Action::Kind::external && action_available_ &&
            !action_available_(action, context)) {
            show_message(
                std::string{i18n::tr("No installed program supports this action and selection")},
                true);
            return;
        }
        action_handoff_(action, context);
    }
}

void Browser::run_action(std::string_view id) {
    const auto found = std::ranges::find(actions_, id, &core::Action::id);
    if (found != actions_.end()) {
        invoke_action(*found, action_context(current_folder()));
    }
}

}  // namespace rummage::views
