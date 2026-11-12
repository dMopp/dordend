# Embed the installed reference so first-run setup also works from a build tree.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/docs/config.example.toml")
file(READ "${PROJECT_SOURCE_DIR}/docs/config.example.toml" NORDEND_DEFAULT_CONFIG)
configure_file(config_template.hpp.in generated/config_template.hpp @ONLY)

add_library(nordend_core
    src/actions.cpp
    src/config.cpp
    src/event_loop.cpp
    src/file.cpp
    src/file_picker.cpp
    src/file_watcher.cpp
    src/i18n.cpp
    src/i18n_de.cpp
    src/log.cpp
    src/memory.cpp
    src/worker_pool.cpp)
add_library(nordend::core ALIAS nordend_core)

target_include_directories(nordend_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(nordend_core PRIVATE PkgConfig::TOMLPLUSPLUS nordend::warnings)
