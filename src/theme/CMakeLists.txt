# Use the installed SVG as the source for accent variants and restoration.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/data/rummage.svg")
file(READ "${PROJECT_SOURCE_DIR}/data/rummage.svg" RUMMAGE_APP_ICON_SVG)
configure_file(app_icon_template.hpp.in generated/app_icon_template.hpp @ONLY)

add_library(rummage_theme
    src/app_icon.cpp
    src/desktop.cpp
    src/oklch.cpp
    src/palette.cpp
    src/syntax.cpp
    src/theme.cpp)
add_library(rummage::theme ALIAS rummage_theme)

target_include_directories(rummage_theme PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rummage_theme
    PUBLIC rummage::core rummage::ui
    PRIVATE PkgConfig::TOMLPLUSPLUS rummage::warnings)
