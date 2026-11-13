#include "rummage/views/shortcuts.hpp"

#include <algorithm>
#include <deque>
#include <format>
#include <optional>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace rummage::views {
namespace {

constexpr ui::Modifiers kCtrl{.ctrl = true};
constexpr ui::Modifiers kShift{.shift = true};
constexpr ui::Modifiers kAlt{.alt = true};
constexpr ui::Modifiers kCtrlShift{.ctrl = true, .shift = true};

// The keys that move the cursor; Left and Right move it only in the grid (in the columns and the
// details they go up and in), but Shift and Ctrl work with all of them.
constexpr std::array<xkb_keysym_t, 8> kMoves{XKB_KEY_Up,    XKB_KEY_Down,    XKB_KEY_Left,
                                             XKB_KEY_Right, XKB_KEY_Page_Up, XKB_KEY_Page_Down,
                                             XKB_KEY_Home,  XKB_KEY_End};

constexpr std::array kShortcuts{
    // General
    Shortcut{.group = "General",
             .keys = "Ctrl+1",
             .action = "Columns",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_1, XKB_KEY_KP_1},
             .command = Command::columns_view},
    Shortcut{.group = "General",
             .keys = "Ctrl+2",
             .action = "Grid",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_2, XKB_KEY_KP_2},
             .command = Command::grid_view},
    Shortcut{.group = "General",
             .keys = "Ctrl+3",
             .action = "Details",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_3, XKB_KEY_KP_3},
             .command = Command::details_view},
    Shortcut{.group = "General",
             .keys = "Space",
             .action = "Quick Look",
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_space},
             .command = Command::quick_look},
    Shortcut{.group = "General",
             .keys = "Ctrl+I",
             .action = "Info pane",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_i, XKB_KEY_I},
             .command = Command::toggle_info},
    Shortcut{.group = "General",
             .keys = "Ctrl+B",
             .action = "Sidebar",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_b, XKB_KEY_B},
             .command = Command::toggle_sidebar},
    Shortcut{.group = "General",
             .keys = "Ctrl+H",
             .action = "Hidden files",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_h, XKB_KEY_H},
             .command = Command::toggle_hidden},
    Shortcut{.group = "General",
             .keys = "Ctrl+D",
             .action = "Add the folder to the sidebar",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_d, XKB_KEY_D},
             .command = Command::add_bookmark},
    Shortcut{.group = "General",
             .keys = "Ctrl+L",
             .action = "Go to a folder or a server",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_l, XKB_KEY_L},
             .command = Command::go_to},
    Shortcut{.group = "General",
             .keys = "F6",
             .action = "Into the sidebar and out",
             .scope = scope::kViews | scope::kSidebar,
             .keysyms = {XKB_KEY_F6},
             .command = Command::focus_sidebar},
    Shortcut{.group = "General",
             .keys = "Ctrl+K",
             .action = "Command palette",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_k, XKB_KEY_K},
             .command = Command::command_palette},
    Shortcut{.group = "General",
             .keys = "Ctrl+Shift+F",
             .action = "Filter names in the view",
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrlShift,
             .keysyms = {XKB_KEY_f, XKB_KEY_F},
             .command = Command::filter_names},
    Shortcut{.group = "General",
             .keys = "Ctrl+F",
             .action = "Find files in the folder",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_f, XKB_KEY_F},
             .command = Command::find_files},
    Shortcut{.group = "General",
             .keys = "F1  ?",
             .action = "These shortcuts",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .any_shift = true,
             .keysyms = {XKB_KEY_F1, XKB_KEY_question},
             .command = Command::show_shortcuts},
    Shortcut{.group = "General",
             .keys = "Menu  Shift+F10",
             .action = "Context menu",
             .scope = scope::kViews,
             .any_shift = true,
             .keysyms = {XKB_KEY_Menu, XKB_KEY_F10},
             .command = Command::context_menu},
    Shortcut{.group = "General",
             .keys = "Alt+Enter",
             .action = "Properties",
             .scope = scope::kViews,
             .modifiers = {.alt = true},
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter},
             .command = Command::properties},
    Shortcut{.group = "General",
             .keys = "Ctrl+Q",
             .action = "Quit",
             .scope = scope::kWindow,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_q, XKB_KEY_Q}},

    // Moving
    Shortcut{.group = "Moving",
             .keys = "↑  ↓",
             .action = "Previous, next",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down}},
    Shortcut{.group = "Moving",
             .keys = "←  →",
             .action = "Previous, next (grid)",
             .scope = scope::kGrid,
             .keysyms = {XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = "Moving",
             .keys = "PgUp  PgDn",
             .action = "A screenful",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Page_Up, XKB_KEY_Page_Down}},
    Shortcut{.group = "Moving",
             .keys = "Home  End",
             .action = "First, last",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = "Moving",
             .keys = "Enter",
             .action = "Open (files in their app)",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = "Moving",
             .keys = "→",
             .action = "Open (columns), expand (details)",
             .scope = scope::kColumns | scope::kDetails,
             .keysyms = {XKB_KEY_Right}},
    Shortcut{.group = "Moving",
             .keys = "Alt+↓",
             .action = "Open (grid, details)",
             .scope = scope::kGrid | scope::kDetails,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Down}},
    Shortcut{.group = "Moving",
             .keys = "F4",
             .action = "Open in a terminal",
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_F4},
             .command = Command::open_terminal},
    Shortcut{.group = "Moving",
             .keys = "Backspace",
             .action = "Parent folder",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_BackSpace}},
    Shortcut{.group = "Moving",
             .keys = "←",
             .action = "Parent (columns), collapse / parent (details)",
             .scope = scope::kColumns | scope::kDetails,
             .keysyms = {XKB_KEY_Left}},
    Shortcut{.group = "Moving",
             .keys = "Alt+↑",
             .action = "Parent folder",
             .scope = scope::kViews,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Up}},
    Shortcut{.group = "Moving",
             .keys = "Alt+←  Alt+→",
             .action = "Back, forward (mouse too)",
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Left},
             .command = Command::back},
    Shortcut{.group = "Moving",
             .keys = "",
             .action = "Forward",
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_Right},
             .command = Command::forward},

    // Selecting
    Shortcut{.group = "Selecting",
             .keys = "Shift+moves",
             .action = "Select a range",
             .scope = scope::kViews,
             .modifiers = kShift,
             .keysyms = kMoves},
    Shortcut{.group = "Selecting",
             .keys = "Ctrl+moves",
             .action = "Move without selecting",
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = kMoves},
    Shortcut{.group = "Selecting",
             .keys = "Ctrl+Space",
             .action = "Select or unselect",
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_space}},
    Shortcut{.group = "Selecting",
             .keys = "Ctrl+A",
             .action = "Select all",
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_a, XKB_KEY_A}},
    Shortcut{.group = "Selecting",
             .keys = "Escape",
             .action = "Back to one entry",
             .scope = scope::kViews,
             .keysyms = {XKB_KEY_Escape}},

    // Panes
    Shortcut{.group = "Panes",
             .keys = "F3",
             .action = "Show two panes / active pane only",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_F3},
             .command = Command::toggle_split},
    Shortcut{.group = "Panes",
             .keys = "Tab",
             .action = "Switch pane",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_Tab},
             .command = Command::switch_pane},
    Shortcut{.group = "Panes",
             .keys = "",
             .action = "Swap panes",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::swap_panes},
    Shortcut{.group = "Panes",
             .keys = "",
             .action = "Change pane orientation",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::orient_panes},
    Shortcut{.group = "Panes",
             .keys = "",
             .action = "Open folder in other pane",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::open_other_pane},
    Shortcut{.group = "Panes",
             .keys = "",
             .action = "Show same folder in other pane",
             .scope = scope::kViews,
             .modifiers = {},
             .keysyms = {XKB_KEY_NoSymbol},
             .command = Command::same_other_pane},
    Shortcut{.group = "Panes",
             .keys = "Ctrl+F5",
             .action = "Copy to other pane",
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_F5},
             .command = Command::copy_other_pane},
    Shortcut{.group = "Panes",
             .keys = "Ctrl+F6",
             .action = "Move to other pane",
             .scope = scope::kViews,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_F6},
             .command = Command::move_other_pane},

    // Tabs
    Shortcut{.group = "Tabs",
             .keys = "Ctrl+T",
             .action = "New tab",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_t, XKB_KEY_T},
             .command = Command::new_tab},
    Shortcut{.group = "Tabs",
             .keys = "Ctrl+W",
             .action = "Close the tab",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_w, XKB_KEY_W},
             .command = Command::close_tab},
    Shortcut{.group = "Tabs",
             .keys = "Ctrl+Tab  Ctrl+Shift+Tab",
             .action = "Next tab, previous",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_Tab},
             .command = Command::next_tab},
    Shortcut{.group = "Tabs",
             .keys = "",
             .action = "Previous tab",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kCtrlShift,
             .keysyms = {XKB_KEY_ISO_Left_Tab, XKB_KEY_Tab},
             .command = Command::previous_tab},
    Shortcut{.group = "Tabs",
             .keys = "Alt+1…9",
             .action = "Tab 1 to 9",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_1, XKB_KEY_2, XKB_KEY_3, XKB_KEY_4, XKB_KEY_5, XKB_KEY_6,
                         XKB_KEY_7, XKB_KEY_8},
             .command = Command::select_tab},
    Shortcut{.group = "Tabs",
             .keys = "",
             .action = "Tab 9",
             .scope = scope::kViews | scope::kSidebar | scope::kQuickLook,
             .modifiers = kAlt,
             .keysyms = {XKB_KEY_9},
             .command = Command::select_tab},

    // Files
    Shortcut{.group = "Files",
             .keys = "F2",
             .action = "Rename",
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_F2},
             .command = Command::rename},
    Shortcut{.group = "Files",
             .keys = "Ctrl+Shift+N",
             .action = "New folder",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = {.ctrl = true, .shift = true},
             .keysyms = {XKB_KEY_n, XKB_KEY_N},
             .command = Command::new_folder},
    Shortcut{.group = "Files",
             .keys = "Ctrl+C",
             .action = "Copy",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_c, XKB_KEY_C},
             .command = Command::copy},
    Shortcut{.group = "Files",
             .keys = "Ctrl+X",
             .action = "Cut (Escape: keep them)",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_x, XKB_KEY_X},
             .command = Command::cut},
    Shortcut{.group = "Files",
             .keys = "Ctrl+V",
             .action = "Paste: copy or move here",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_v, XKB_KEY_V},
             .command = Command::paste},
    Shortcut{.group = "Files",
             .keys = "Delete",
             .action = "Move to the trash",
             .scope = scope::kViews | scope::kQuickLook,
             .keysyms = {XKB_KEY_Delete, XKB_KEY_KP_Delete},
             .command = Command::trash},
    Shortcut{.group = "Files",
             .keys = "Shift+Delete",
             .action = "Delete permanently",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kShift,
             .keysyms = {XKB_KEY_Delete, XKB_KEY_KP_Delete},
             .command = Command::erase},
    Shortcut{.group = "Files",
             .keys = "Ctrl+Z",
             .action = "Undo the last operation",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_z, XKB_KEY_Z},
             .command = Command::undo},
    Shortcut{.group = "Files",
             .keys = "Ctrl+Shift+Z",
             .action = "Redo it",
             .scope = scope::kViews | scope::kQuickLook,
             .modifiers = {.ctrl = true, .shift = true},
             .keysyms = {XKB_KEY_z, XKB_KEY_Z},
             .command = Command::redo},
    Shortcut{.group = "Files",
             .keys = "Enter  Escape",
             .action = "Renaming: keep, give up",
             .scope = scope::kEditor,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter, XKB_KEY_Escape}},
    Shortcut{.group = "Files",
             .keys = "Ctrl+←  Ctrl+→",
             .action = "Renaming: by words",
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = "Files",
             .keys = "Ctrl+Z",
             .action = "Renaming: undo (Shift: redo)",
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .any_shift = true,  // with Shift it redoes
             .keysyms = {XKB_KEY_z, XKB_KEY_Z}},
    Shortcut{.group = "Files",
             .keys = "Ctrl+V",
             .action = "Renaming: paste text",
             .scope = scope::kEditor,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_v, XKB_KEY_V}},
    Shortcut{.group = "Files",
             .keys = "R  S  Enter",
             .action = "Name taken: replace, skip, both",
             .scope = scope::kConflict,
             .keysyms = {XKB_KEY_r, XKB_KEY_s, XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = "Files",
             .keys = "A  Escape",
             .action = "Name taken: for the rest, stop",
             .scope = scope::kConflict,
             .keysyms = {XKB_KEY_a, XKB_KEY_Escape}},
    Shortcut{.group = "Files",
             .keys = "Enter  Escape",
             .action = "Delete permanently? Yes, no",
             .scope = scope::kConfirm,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter, XKB_KEY_Escape}},

    // Grid
    Shortcut{.group = "Zoom",
             .keys = "Ctrl++  Ctrl+−",
             .action = "Larger, smaller (wheel too)",
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrl,
             .any_shift = true,
             .keysyms = {XKB_KEY_plus, XKB_KEY_equal, XKB_KEY_KP_Add, XKB_KEY_minus,
                         XKB_KEY_KP_Subtract}},
    Shortcut{.group = "Zoom",
             .keys = "Ctrl+0",
             .action = "Usual size",
             .scope = scope::kViews | scope::kSidebar,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_0, XKB_KEY_KP_0}},

    // Sidebar
    Shortcut{.group = "Sidebar",
             .keys = "↑  ↓  Home  End",
             .action = "Choose a place",
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = "Sidebar",
             .keys = "Enter",
             .action = "Open it",
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Return, XKB_KEY_KP_Enter}},
    Shortcut{.group = "Sidebar",
             .keys = "Escape",
             .action = "Back to the view",
             .scope = scope::kSidebar,
             .keysyms = {XKB_KEY_Escape}},

    // Quick Look
    Shortcut{.group = "Quick Look",
             .keys = "Space  Escape",
             .action = "Close",
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_space, XKB_KEY_Escape}},
    Shortcut{.group = "Quick Look",
             .keys = "Arrows",
             .action = "The next file, the previous",
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Up, XKB_KEY_Down, XKB_KEY_Left, XKB_KEY_Right}},
    Shortcut{.group = "Quick Look",
             .keys = "Home  End",
             .action = "A text file's top, its bottom",
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Home, XKB_KEY_End}},
    Shortcut{.group = "Quick Look",
             .keys = "PgUp  PgDn",
             .action = "A page up, down (Shift+wheel: sideways)",
             .scope = scope::kQuickLook,
             .keysyms = {XKB_KEY_Page_Up, XKB_KEY_Page_Down}},
    Shortcut{.group = "Quick Look",
             .keys = "Ctrl+A",
             .action = "Select a text file's text (or drag on it)",
             .scope = scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_a, XKB_KEY_A}},
    Shortcut{.group = "Quick Look",
             .keys = "Ctrl+C",
             .action = "Copy the text selected",
             .scope = scope::kQuickLook,
             .modifiers = kCtrl,
             .keysyms = {XKB_KEY_c, XKB_KEY_C}},
};

struct Bindings {
    std::vector<Shortcut> entries{kShortcuts.begin(), kShortcuts.end()};
    std::deque<std::string> labels;
};

Bindings active;

std::optional<std::size_t> binding_index(std::string_view name) {
    Command command = Command::none;
    if (name == "move_other_pane")
        command = Command::move_other_pane;
    if (name == "copy_other_pane")
        command = Command::copy_other_pane;
    if (name == "same_other_pane")
        command = Command::same_other_pane;
    if (name == "open_other_pane")
        command = Command::open_other_pane;
    if (name == "orient_panes")
        command = Command::orient_panes;
    if (name == "swap_panes")
        command = Command::swap_panes;
    if (name == "switch_pane")
        command = Command::switch_pane;
    if (name == "toggle_split")
        command = Command::toggle_split;
    if (name == "columns_view")
        command = Command::columns_view;
    if (name == "grid_view")
        command = Command::grid_view;
    if (name == "details_view")
        command = Command::details_view;
    if (name == "toggle_info")
        command = Command::toggle_info;
    if (name == "toggle_sidebar")
        command = Command::toggle_sidebar;
    if (name == "focus_sidebar")
        command = Command::focus_sidebar;
    if (name == "quick_look")
        command = Command::quick_look;
    if (name == "show_shortcuts")
        command = Command::show_shortcuts;
    if (name == "rename")
        command = Command::rename;
    if (name == "new_folder")
        command = Command::new_folder;
    if (name == "copy")
        command = Command::copy;
    if (name == "cut")
        command = Command::cut;
    if (name == "paste")
        command = Command::paste;
    if (name == "trash")
        command = Command::trash;
    if (name == "erase")
        command = Command::erase;
    if (name == "undo")
        command = Command::undo;
    if (name == "redo")
        command = Command::redo;
    if (name == "context_menu")
        command = Command::context_menu;
    if (name == "properties")
        command = Command::properties;
    if (name == "back")
        command = Command::back;
    if (name == "forward")
        command = Command::forward;
    if (name == "toggle_hidden")
        command = Command::toggle_hidden;
    if (name == "add_bookmark")
        command = Command::add_bookmark;
    if (name == "open_terminal")
        command = Command::open_terminal;
    if (name == "new_tab")
        command = Command::new_tab;
    if (name == "close_tab")
        command = Command::close_tab;
    if (name == "next_tab")
        command = Command::next_tab;
    if (name == "previous_tab")
        command = Command::previous_tab;
    if (name == "command_palette")
        command = Command::command_palette;
    if (name == "filter_names")
        command = Command::filter_names;
    if (name == "find_files")
        command = Command::find_files;
    if (name == "go_to")
        command = Command::go_to;
    for (std::size_t i = 0; i < kShortcuts.size(); ++i) {
        const auto& entry = kShortcuts[i];
        if (command != Command::none && entry.command == command)
            return i;
        if (name == "select_all" && entry.action == "Select all")
            return i;
        if (name == "toggle_selection" && entry.action == "Select or unselect")
            return i;
    }
    return std::nullopt;
}

bool parse_chord(std::string_view text, Shortcut& entry) {
    entry.modifiers = {};
    entry.any_shift = false;
    entry.keysyms = {};
    if (text.empty())
        return true;
    while (text.find('+') != std::string_view::npos) {
        const auto split = text.find('+');
        const auto part = text.substr(0, split);
        bool* flag = nullptr;
        if (part == "Ctrl")
            flag = &entry.modifiers.ctrl;
        if (part == "Shift")
            flag = &entry.modifiers.shift;
        if (part == "Alt")
            flag = &entry.modifiers.alt;
        if (part == "Super")
            flag = &entry.modifiers.super;
        if (flag == nullptr || *flag)
            return false;
        *flag = true;
        text.remove_prefix(split + 1);
    }
    const auto symbol =
        xkb_keysym_from_name(std::string{text}.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    if (symbol == XKB_KEY_NoSymbol)
        return false;
    entry.keysyms[0] = xkb_keysym_to_lower(symbol);
    entry.keysyms[1] = xkb_keysym_to_upper(symbol);
    if (symbol == XKB_KEY_Tab && entry.modifiers.shift) {
        entry.keysyms[2] = XKB_KEY_ISO_Left_Tab;
    }
    return true;
}

}  // namespace

bool Shortcut::matches(const ui::KeyEvent& event) const {
    ui::Modifiers pressed = event.modifiers;
    if (any_shift) {
        pressed.shift = false;
    }
    return pressed == modifiers && event.keysym != XKB_KEY_NoSymbol &&
           std::ranges::find(keysyms, event.keysym) != keysyms.end();
}

std::vector<std::string> configure_shortcuts(const std::map<std::string, std::string>& bindings,
                                             std::span<const core::Action> custom) {
    Bindings next;
    for (const auto& action : core::action_registry(custom)) {
        const bool bundled = action.id == "copy_location" || action.id == "extract_here" ||
                             action.id == "extract_to";
        if (!bindings.contains("action:" + action.id) &&
            !(bundled && bindings.contains(action.id))) {
            continue;
        }
        next.labels.push_back(action.id);
        const std::string_view id = next.labels.back();
        next.labels.push_back(action.label);
        Shortcut entry;
        entry.group = "Files";
        entry.action = next.labels.back();
        entry.scope = scope::kViews;
        entry.action_id = id;
        next.entries.push_back(entry);
    }
    std::vector<std::string> errors;
    std::vector<std::size_t> changed;
    // Split combined help rows when either half has been customized.
    if (bindings.contains("back") || bindings.contains("forward")) {
        auto& back = next.entries[*binding_index("back")];
        back.keys = "Alt+Left";
        back.action = "Back";
        next.entries[*binding_index("forward")].keys = "Alt+Right";
    }
    if (bindings.contains("next_tab") || bindings.contains("previous_tab")) {
        auto& tab = next.entries[*binding_index("next_tab")];
        tab.keys = "Ctrl+Tab";
        tab.action = "Next tab";
        next.entries[*binding_index("previous_tab")].keys = "Ctrl+Shift+Tab";
    }
    for (const auto& [name, chord] : bindings) {
        auto index = binding_index(name);
        const std::string_view id = std::string_view{name}.starts_with("action:")
                                        ? std::string_view{name}.substr(7)
                                        : std::string_view{name};
        if (!index) {
            for (std::size_t i = kShortcuts.size(); i < next.entries.size(); ++i) {
                if (next.entries[i].action_id == id) {
                    index = i;
                    break;
                }
            }
        }
        if (!index) {
            errors.push_back(std::format("keybindings.{}: unknown action", name));
            continue;
        }
        if (std::ranges::find(changed, *index) != changed.end()) {
            errors.push_back(std::format("keybindings.{}: action bound twice", name));
            continue;
        }
        auto& entry = next.entries[*index];
        if (!parse_chord(chord, entry)) {
            errors.push_back(std::format("keybindings.{}: invalid chord '{}'", name, chord));
            continue;
        }
        next.labels.push_back(chord);
        entry.keys = next.labels.back();
        changed.push_back(*index);
    }
    for (const auto index : changed) {
        const auto& entry = next.entries[index];
        for (std::size_t other = 0; other < next.entries.size(); ++other) {
            const auto& existing = next.entries[other];
            // Editors and dialogs own their input, independently of file commands.
            constexpr auto context =
                scope::kViews | scope::kSidebar | scope::kQuickLook | scope::kWindow;
            if (other == index || ((entry.scope & existing.scope & context) == 0 &&
                                   (existing.scope & scope::kWindow) == 0))
                continue;
            bool collision = false;
            for (const auto symbol : entry.keysyms) {
                ui::KeyEvent event;
                event.keysym = symbol;
                event.modifiers = entry.modifiers;
                collision = collision ||
                            (existing.matches(event) &&
                             (index >= kShortcuts.size() || !kShortcuts[index].matches(event)));
            }
            // Existing text-preview bindings intentionally share file copy/select-all chords.
            if (collision)
                errors.push_back(std::format("keybindings: '{}' conflicts with '{}'", entry.keys,
                                             existing.action));
        }
    }
    if (errors.empty())
        active = std::move(next);
    return errors;
}

ui::KeyEvent view_key_event(const ui::KeyEvent& event) {
    for (std::size_t i = 0; i < kShortcuts.size(); ++i) {
        const auto& entry = active.entries[i];
        if (entry.command == Command::none && (entry.scope & scope::kViews) != 0 &&
            entry.matches(event) &&
            (entry.action == "Select all" || entry.action == "Select or unselect")) {
            auto translated = event;
            translated.keysym = kShortcuts[i].keysyms[0];
            translated.modifiers = kShortcuts[i].modifiers;
            return translated;
        }
    }
    return event;
}

std::span<const Shortcut> shortcuts() { return active.entries; }

std::string keys_for(Command command) {
    const auto it = std::ranges::find(active.entries, command, &Shortcut::command);
    return it == active.entries.end() ? std::string{} : std::string{it->keys};
}

Command command_for(const ui::KeyEvent& event) {
    for (const Shortcut& shortcut : active.entries) {
        if (shortcut.command != Command::none && shortcut.matches(event)) {
            return shortcut.command;
        }
    }
    return Command::none;
}

std::string action_for(const ui::KeyEvent& event) {
    for (const auto& entry : active.entries) {
        if (!entry.action_id.empty() && entry.matches(event)) {
            return std::string{entry.action_id};
        }
    }
    return {};
}

std::string keys_for_action(std::string_view id) {
    const auto found = std::ranges::find(active.entries, id, &Shortcut::action_id);
    return found == active.entries.end() ? std::string{} : std::string{found->keys};
}

bool listed(const ui::KeyEvent& event, std::uint16_t where) {
    return std::ranges::any_of(active.entries, [&](const Shortcut& shortcut) {
        return (shortcut.scope & where) != 0U && shortcut.matches(event);
    });
}

}  // namespace rummage::views
