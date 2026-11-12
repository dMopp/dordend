#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon.h>

#include "nordend/core/actions.hpp"
#include "nordend/ui/seat.hpp"

namespace nordend::views {

// Where a shortcut works, as flags.
namespace scope {
inline constexpr std::uint16_t kColumns = 1U << 0U;
inline constexpr std::uint16_t kGrid = 1U << 1U;
inline constexpr std::uint16_t kDetails = 1U << 2U;
inline constexpr std::uint16_t kViews = kColumns | kGrid | kDetails;
inline constexpr std::uint16_t kSidebar = 1U << 3U;  // the keyboard in the sidebar
inline constexpr std::uint16_t kQuickLook = 1U << 4U;
inline constexpr std::uint16_t kWindow = 1U << 5U;    // handled by the app, not the browser
inline constexpr std::uint16_t kEditor = 1U << 6U;    // while a name is being edited
inline constexpr std::uint16_t kConflict = 1U << 7U;  // while a copy asks about a taken name
inline constexpr std::uint16_t kConfirm = 1U << 8U;   // while a delete asks to be confirmed
}  // namespace scope

// What the browser itself does for a shortcut; none: the view (or the sidebar) handles it.
enum class Command : std::uint8_t {
    none,
    columns_view,
    grid_view,
    details_view,
    toggle_info,
    toggle_sidebar,
    focus_sidebar,
    quick_look,
    show_shortcuts,
    rename,
    new_folder,
    copy,
    cut,
    paste,
    trash,
    erase,
    undo,
    redo,
    context_menu,
    properties,
    back,
    forward,
    toggle_hidden,
    add_bookmark,
    open_terminal,
    new_tab,
    close_tab,
    next_tab,
    previous_tab,
    select_tab,  // Alt+1…9: the digit says which
    command_palette,
    find_files,
    filter_names,
    go_to,
};

// One line of the shortcuts overlay, and the key chords it stands for.
struct Shortcut {
    std::string_view group;  // the overlay's section
    std::string_view keys;   // as shown ("Ctrl+1", "Shift+arrows"); empty: on the line before
    std::string_view action;
    std::uint16_t scope{0};
    ui::Modifiers modifiers{};
    // Shift does not matter: the key is a character that may need it ("?", "+").
    bool any_shift{false};
    std::array<xkb_keysym_t, 8> keysyms{};  // unused slots are 0 (XKB_KEY_NoSymbol)
    Command command{Command::none};
    std::string_view action_id{};  // registry actions; storage owned by the active bindings

    [[nodiscard]] bool matches(const ui::KeyEvent& event) const;
};

// Replaces command bindings atomically. Errors leave the current bindings intact.
// Empty strings disable a binding; an empty map restores defaults.
[[nodiscard]] std::vector<std::string> configure_shortcuts(
    const std::map<std::string, std::string>& bindings, std::span<const core::Action> custom = {});
[[nodiscard]] ui::KeyEvent view_key_event(const ui::KeyEvent& event);

// Every shortcut, in the overlay's order. The browser runs its commands from here and passes a
// view (or the sidebar) only the chords listed for it, so this list is what works.
[[nodiscard]] std::span<const Shortcut> shortcuts();

// The keys shown for command in the overlay ("Ctrl+C"); empty when it has none.
[[nodiscard]] std::string keys_for(Command command);

// The browser's command for event; Command::none when it has none.
[[nodiscard]] Command command_for(const ui::KeyEvent& event);

[[nodiscard]] std::string action_for(const ui::KeyEvent& event);
[[nodiscard]] std::string keys_for_action(std::string_view id);

// event is listed for one of the scopes in where.
[[nodiscard]] bool listed(const ui::KeyEvent& event, std::uint16_t where);

}  // namespace nordend::views
