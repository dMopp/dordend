#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <system_error>
#include <utility>

#include "rummage/views/browser.hpp"
#include "rummage/views/format.hpp"

namespace rummage::views {
namespace {

std::filesystem::path session_fallback() {
    // HOME may name a missing directory (for example /homeless-shelter in a Nix build).
    if (const char* home = std::getenv("HOME"); home != nullptr) {
        const std::filesystem::path path{home};
        std::error_code error;
        if (path.is_absolute() && std::filesystem::is_directory(path, error)) {
            return path;
        }
    }
    return "/";
}

}  // namespace

Browser::BrowsingArea::BrowsingArea(Browser& browser)
    : breadcrumb{browser.redraw_,
                 [&browser](const std::filesystem::path& folder, const std::string& child) {
                     browser.go_to(folder, child);
                 },
                 [] {
                     const char* home = std::getenv("HOME");
                     return home ? std::filesystem::path{home} : std::filesystem::path{};
                 }()},
      tab_bar{browser.redraw_,
              {.select = [&browser](std::size_t index) { browser.select_tab(index); },
               .close = [&browser](std::size_t index) { browser.close_tab(index); },
               .add = [&browser] { browser.new_tab(); }}} {}

void Browser::toggle_split() {
    if (!panes_enabled_)
        return;
    leave_pane();
    if (!areas_[1]) {
        const auto folder =
            pane().mode == Mode::details ? pane().details.root_folder() : current_folder();
        const auto path = folder ? folder->path() : std::filesystem::path{"/"};
        const auto mode = pane().mode;
        const auto list = pane().details.list_size();
        const auto grid = pane().grid.size_index();
        const auto order = pane().details.order();
        const bool hidden = pane().show_hidden;
        areas_[1] = std::make_unique<BrowsingArea>(*this);
        context_area_ = active_area_ = 1;
        add_pane(0);
        pane().mode = mode;
        set_list_size(list);
        set_grid_size(grid);
        set_sort(order.key, order.descending);
        set_show_hidden(hidden, false);
        open(path);
    }
    split_visible_ = !split_visible_;
    button_held_ = false;
    pointer_area_ = active_area_;
    pointer_target_ = Target::view;
    sidebar_.set_focused(false);
    redraw_();
}

void Browser::focus_pane(std::size_t index) {
    if (!split_visible_ || index >= areas_.size() || !areas_[index] || index == active_area_)
        return;
    context_area_ = active_area_;
    note_location();
    leave_pane();
    context_area_ = active_area_ = index;
    sidebar_.set_focused(false);
    button_held_ = false;
    pointer_area_ = index;
    pointer_target_ = Target::view;
    redraw_();
}

void Browser::switch_pane() { focus_pane(1 - active_area_); }

void Browser::swap_panes() {
    if (!split_visible_)
        return;
    leave_pane();
    std::swap(areas_[0], areas_[1]);
    context_area_ = active_area_ = 1 - active_area_;
    pointer_area_ = active_area_;
    pointer_target_ = Target::view;
    button_held_ = false;
    redraw_();
}

void Browser::set_stacked(bool stacked) {
    stacked_ = stacked;
    redraw_();
}

void Browser::set_split_ratio(float ratio) {
    if (std::isfinite(ratio))
        split_ratio_ = std::clamp(ratio, 0.15F, 0.85F);
    redraw_();
}

std::shared_ptr<Folder> Browser::other_folder() const {
    if (!split_visible_ || !areas_[1 - active_area_])
        return {};
    const auto& other = *areas_[1 - active_area_];
    const Pane& tab = *other.tabs[other.active];
    // Expanded rows in Details do not change the destination shown by its breadcrumb.
    return tab.mode == Mode::details ? tab.details.root_folder() : folder_of(tab);
}

void Browser::open_in_other_pane(const std::filesystem::path& directory) {
    if (!panes_enabled_)
        return;
    if (!split_visible_) {
        const auto source = active_area_;
        toggle_split();
        focus_pane(1 - source);
    } else {
        switch_pane();
    }
    open_tab(directory, true);
}

void Browser::transfer_to_other_pane(bool move) {
    const auto source = current_folder();
    const auto destination = other_folder();
    if (!source || source->is_trash() || !destination || destination->is_trash())
        return;
    std::vector<std::filesystem::path> paths;
    for (const auto& name : targets(*source))
        paths.push_back(source->path() / name);
    if (paths.empty())
        return;
    if (const auto why = refusal(destination->path(), paths); !why.empty()) {
        show_message(why, false);
        return;
    }
    transfer_paths(destination->path(), paths, move, std::nullopt);
}

std::vector<MenuItem> Browser::pane_items() {
    if (!panes_enabled_)
        return {};
    std::vector<MenuItem> items;
    const auto add = [&](std::string id, std::string label, Command command,
                         std::function<void()> action) {
        MenuItem item;
        item.id = std::move(id);
        item.label = std::move(label);
        item.keys = keys_for(command);
        item.action = std::move(action);
        items.push_back(std::move(item));
    };
    add("toggle_split", split_visible_ ? "Show Active Pane Only" : "Show Two Panes",
        Command::toggle_split, [this] { toggle_split(); });
    if (split_visible_) {
        add("switch_pane", "Switch Pane", Command::switch_pane, [this] { switch_pane(); });
        add("swap_panes", "Swap Panes", Command::swap_panes, [this] { swap_panes(); });
        add("orient_panes", stacked_ ? "Arrange Side by Side" : "Arrange Top / Bottom",
            Command::orient_panes, [this] { set_stacked(!stacked_); });
    }
    const auto source = current_folder();
    if (!source || source->is_trash())
        return items;
    const auto same = pane().mode == Mode::details ? pane().details.root_folder() : source;
    if (same)
        add("same_other_pane", "Show Same Folder in Other Pane", Command::same_other_pane,
            [this, path = same->path()] { open_in_other_pane(path); });
    const auto names = targets(*source);
    if (names.size() == 1) {
        const auto entry = std::ranges::find(source->entries(), names.front(), &fs::Entry::name);
        if (entry != source->entries().end() && entry->is_directory()) {
            add("open_other_pane", "Open in Other Pane", Command::open_other_pane,
                [this, path = source->path() / entry->name] { open_in_other_pane(path); });
        }
    }
    const auto destination = other_folder();
    if (!destination || destination->is_trash() || names.empty())
        return items;
    std::vector<std::filesystem::path> paths;
    for (const auto& name : names)
        paths.push_back(source->path() / name);
    if (!refusal(destination->path(), paths).empty())
        return items;
    const std::string direction =
        stacked_ ? (active_area_ == 0 ? "↓" : "↑") : (active_area_ == 0 ? "→" : "←");
    for (const bool move : {false, true}) {
        add(move ? "move_other_pane" : "copy_other_pane",
            std::format("{} to “{}” {}", move ? "Move" : "Copy", destination->path().string(),
                        direction),
            move ? Command::move_other_pane : Command::copy_other_pane,
            [this, path = destination->path(), paths, move] {
                transfer_paths(path, paths, move, std::nullopt);
            });
    }
    return items;
}

bool Browser::restore_session(const State& state) {
    if (!panes_enabled_)
        return restore_tabs(state.tabs, state.shown_tab);
    std::array<std::vector<State::Tab>, 2> tabs;
    for (const auto& tab : state.tabs) {
        if (tab.pane < tabs.size() && tabs[tab.pane].size() < State::kMaxTabs)
            tabs[tab.pane].push_back(tab);
    }
    context_area_ = active_area_ = 0;
    bool restored = restore_tabs(tabs[0], state.shown_tab);
    if (state.panes_initialized || !tabs[1].empty()) {
        areas_[1] = std::make_unique<BrowsingArea>(*this);
        context_area_ = 1;
        add_pane(0);
        apply(state);
        const bool second = restore_tabs(tabs[1], state.other_shown_tab);
        if (!second) {
            open(session_fallback());
        }
        if (!restored) {
            context_area_ = 0;
            open(session_fallback());
        }
        restored = true;
        split_visible_ = state.split_visible;
        active_area_ = std::min(state.active_pane, std::size_t{1});
    }
    stacked_ = state.stacked;
    set_split_ratio(state.split_ratio);
    context_area_ = pointer_area_ = active_area_;
    return restored;
}

std::optional<std::size_t> Browser::area_at(float x, float y) const {
    for (std::size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i] && (split_visible_ || i == active_area_) && areas_[i]->bounds.contains(x, y))
            return i;
    }
    return {};
}

}  // namespace rummage::views
