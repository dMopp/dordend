# fzy (third_party/fzy): the fuzzy matcher of the command palette. Compiled on its own, as C,
# without our warnings.
add_library(rummage_fzy STATIC ${PROJECT_SOURCE_DIR}/third_party/fzy/src/match.c)
target_include_directories(rummage_fzy SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party)
target_compile_options(rummage_fzy PRIVATE -w)
if(RUMMAGE_SANITIZER)
    target_compile_options(rummage_fzy PRIVATE -fsanitize=${RUMMAGE_SANITIZER})
endif()

add_library(rummage_views
    src/breadcrumb.cpp
    src/browser_actions.cpp
    src/browser_panes.cpp
    src/browser.cpp
    src/browser_palette.cpp
    src/column.cpp
    src/column_view.cpp
    src/command_palette.cpp
    src/error_dialog.cpp
    src/confirm_dialog.cpp
    src/conflict_dialog.cpp
    src/context_menu.cpp
    src/menu_layout.cpp
    src/details_view.cpp
    src/file_clipboard.cpp
    src/draw_helpers.cpp
    src/folder.cpp
    src/format.cpp
    src/file_finder.cpp
    src/file_picker.cpp
    src/fuzzy.cpp
    src/grid_view.cpp
    src/info_pane.cpp
    src/location_dialog.cpp
    src/name_editor.cpp
    src/pane.cpp
    src/password_dialog.cpp
    src/properties_dialog.cpp
    src/shortcuts.cpp
    src/shortcuts_overlay.cpp
    src/sidebar.cpp
    src/status_bar.cpp
    src/state.cpp
    src/tab_bar.cpp
    src/title_bar.cpp
    src/quick_look.cpp
    src/text_selection.cpp
    src/thumbnails.cpp
    src/undo.cpp)
add_library(rummage::views ALIAS rummage_views)

target_include_directories(rummage_views PUBLIC include)
target_link_libraries(rummage_views
    PUBLIC rummage::core rummage::ui rummage::fs rummage::theme rummage::thumbs rummage::preview
    PRIVATE rummage::warnings PkgConfig::TOMLPLUSPLUS rummage_fzy)
